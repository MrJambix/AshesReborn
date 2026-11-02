// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Globalization;
using System.Text.RegularExpressions;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Objects.Engine.Curves;
using CUE4Parse.UE4.Objects.UObject;
using Mapc;

namespace NpcAbilities;

/// <summary>
/// The conditions of a state tree, one by one: what each tests, which values it tests them against, and where those values come from.  A state's enter conditions are
/// a list; each has an operand (how it joins what came before: the first copies, the rest `And` or `Or`) and an indent change (a positive one opens that many brackets before it,
/// a negative one closes that many after it).  A condition's own settings sit in its instance data, and a setting can be bound to a value the tree has elsewhere (the
/// creature's own actor, the target the hate list names, a task's timer, one of the tree's parameters): the tree's property bindings say which.
///
/// The table says a condition as an atom, one of:
///   T / F            true / false for good (a parameter switched it)
///   in(D) / out(D)   the target is nearer than D / farther than D (cm, from surface to surface)
///   cin(D) / cout(D) the centres of the creature and its target are nearer than D / not (cm, in 3D)
///   ang(A)           the target is within A degrees of the creature's facing
///   nang(A)          ... is not
///   cd(NAME)         the cooldown of that name is over
///   rand(P)          a fresh roll under P
///   hp(V) / nhp(V)   the creature's own health is under V of its maximum / is not
///   fc(OP,V)         the seconds since the creature's fight began compare as OP (eq ne lt le gt ge) to V
///   fcr(OP,B,P,L,H,N) the same against a number the creature rolls once when its fight begins (the tree's variable N): B, or with the chance P a fresh number between L and H
///   los / nlos       the creature sees its target / does not
///   eff(G) / neff(G) the creature holds the effect record G / does not (STC_HasEffect_C on its own Actor)
///   teff(G) / nteff(G) the character it hates most holds the effect record G / does not
///   tgt / ntgt       the creature has a target / has none
///   hate(D)          someone on the creature's hate list stands nearer than D (cm, centre to centre)
///   hateb(LO,HI)     ... farther than LO and nearer than HI
///   elite(OP,N)      the creature's elite rank compares as OP to N
///   effc(OP,N,G) / teffc(OP,N,G) the stacks of the effect record G on the creature / on the character it hates most compare as OP to N
///   ?NAME            a condition this tool cannot say (the task is left out)
/// </summary>
public static partial class Reader
{
    /// <summary>A tree and the arrays of it the conditions need.</summary>
    sealed class TreeInfo
    {
        public FStructFallback[] States = Array.Empty<FStructFallback>(), Nodes = Array.Empty<FStructFallback>(), Shared = Array.Empty<FStructFallback>(), Defaults = Array.Empty<FStructFallback>();
        public FStructFallback[] Batches = Array.Empty<FStructFallback>(), Paths = Array.Empty<FStructFallback>(), Sources = Array.Empty<FStructFallback>();
        public FStructFallback[] Transitions = Array.Empty<FStructFallback>();
    }

    /// <summary>Where a bound setting gets its value.</summary>
    sealed class Source
    {
        public string Kind = "", Class = "", Name = "", Path = "";   // Context / Parameter / Evaluator / Task / PropertyFunction ...; the struct's class and name; the property inside it
        public bool Global;
        public int State = -1;
        public FStructFallback? Handle;   // the data handle of the source in the tree's list of sources (source, index and state)
        public override string ToString() => Kind + ":" + (Class.Length > 0 ? Class + ":" : "") + Name + (Path.Length > 0 ? "." + Path : "");
    }

    /// <summary>One condition as the table says it.</summary>
    sealed class Atom
    {
        public string Text = "";
        public string Kind = "";        // dist, ang or cd for a condition the table's own columns carry, else empty
        public double Value;
        public bool Inverse;
        public string Name = "";
        public bool Unknown;
        public string Constant = "";    // "T" or "F"
    }

    static TreeInfo LoadTree(UObject tree)
    {
        var t = new TreeInfo
        {
            States = StructsOf(tree, "States").ToArray(),
            Nodes = Container(tree.Properties, "Nodes"),
            Shared = InstanceStructs(tree, "SharedInstanceData"),
            Defaults = InstanceStructs(tree, "DefaultInstanceData"),
            Transitions = StructsOf(tree, "Transitions").ToArray()
        };
        if (tree.TryGetValue(out FStructFallback bindings, "PropertyBindings") && bindings != null)
        {
            t.Batches = StructArray(bindings, "CopyBatches");
            t.Paths = StructArray(bindings, "PropertyPathBindings");
            t.Sources = StructArray(bindings, "SourceStructs");
        }
        return t;
    }

    static FStructFallback[] StructArray(FStructFallback owner, string name)
    {
        var prop = owner.Properties.FirstOrDefault(p => p.Name.Text == name);
        if (prop?.Tag?.GenericValue is not UScriptArray arr) return Array.Empty<FStructFallback>();
        return arr.Properties.Select(x => x.GenericValue is FScriptStruct ss && ss.StructType is FStructFallback f ? f : new FStructFallback()).ToArray();
    }

    static string TextOf(FStructFallback s, string name)
    {
        var v = s.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue;
        if (v == null) return "";
        string text = v is FName n ? n.Text : v is FPackageIndex pi ? (pi.IsNull ? "" : pi.Name) : v.ToString() ?? "";
        int colons = text.LastIndexOf("::", StringComparison.Ordinal);
        return colons >= 0 ? text.Substring(colons + 2) : text;
    }

    static int IndexOf(FStructFallback s, string name)
    {
        var v = s.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue;
        if (v is FScriptStruct ss && ss.StructType is FStructFallback f) return IntOf(f, "Value", IntOf(f, "Index"));
        return -1;
    }

    static string PathText(FStructFallback? p)
    {
        if (p == null) return "";
        var segs = p.Properties.FirstOrDefault(x => x.Name.Text == "Segments")?.Tag?.GenericValue as UScriptArray;
        if (segs == null) return "";
        return string.Join(".", segs.Properties.Select(x => x.GenericValue is FScriptStruct ss && ss.StructType is FStructFallback f ? TextOf(f, "Name") : "?"));
    }

    static FStructFallback? StructOf(FStructFallback s, string name) => s.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue is FScriptStruct ss ? ss.StructType as FStructFallback : null;

    /// <summary>The state a data handle belongs to (the index of its state handle); -1 when it names none.</summary>
    static int StateIndexOf(FStructFallback? handle)
    {
        var state = handle != null ? StructOf(handle, "StateHandle") : null;
        return state != null ? IntOf(state, "Index") : -1;
    }

    /// <summary>Whether two data handles name the same data: the same kind of source, the same index and the same state.</summary>
    static bool SameHandle(FStructFallback a, FStructFallback b) => TextOf(a, "Source") == TextOf(b, "Source") && TextOf(a, "Index") == TextOf(b, "Index") && StateIndexOf(a) == StateIndexOf(b);

    static Source SourceOf(TreeInfo t, FStructFallback? handle, string path)
    {
        var src = new Source { Path = path };
        if (handle == null) return src;
        string source = TextOf(handle, "Source");
        foreach (var s in t.Sources)
        {
            // (the same source kind and index name a task in each of several states: the state tells them apart)
            var dh = StructOf(s, "DataHandle");
            if (dh == null || !SameHandle(dh, handle)) continue;
            src.Kind = TextOf(s, "DataSource");
            src.Class = TextOf(s, "Struct");
            src.Name = TextOf(s, "Name");
            src.Global = source.Contains("Global", StringComparison.Ordinal);
            src.State = StateIndexOf(dh);
            src.Handle = dh;
            return src;
        }
        return src;
    }

    /// <summary>The bound settings of a node: the setting's name -> where its value comes from.</summary>
    static Dictionary<string, Source> BindingsOf(TreeInfo t, FStructFallback node)
    {
        var result = new Dictionary<string, Source>(StringComparer.Ordinal);
        int batch = IndexOf(node, "BindingsBatch");
        if (batch < 0 || batch >= t.Batches.Length) return result;
        int begin = IndexOf(t.Batches[batch], "BindingsBegin"), end = IndexOf(t.Batches[batch], "BindingsEnd");
        for (int i = begin; i >= 0 && i < end && i < t.Paths.Length; i++)
        {
            var p = t.Paths[i];
            string target = PathText(StructOf(p, "TargetPropertyPath"));
            if (target.Length > 0) result[target] = SourceOf(t, StructOf(p, "SourceDataHandle"), PathText(StructOf(p, "SourcePropertyPath")));
        }
        return result;
    }

    static double? NumberOf(object? v) => v switch
    {
        double d => d,
        float f => f,
        int i => i,
        long l => l,
        uint u => u,
        ulong ul => ul,
        short s => s,
        ushort us => us,
        byte b => b,
        sbyte sb => sb,
        _ => null
    };

    /// <summary>A setting's number from the node's instance data (an object or a struct).</summary>
    static double? InstanceNumber(UObject? obj, FStructFallback? str, string name)
    {
        if (obj != null) return NumberOf(obj.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue);
        if (str != null) return NumberOf(str.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue);
        return null;
    }

    static bool? InstanceBool(UObject? obj, FStructFallback? str, string name)
    {
        object? v = obj != null ? obj.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue : str?.Properties.FirstOrDefault(p => p.Name.Text == name)?.Tag?.GenericValue;
        return v is bool b ? b : null;
    }

    static bool HasInstanceProperty(UObject? obj, FStructFallback? str, string name) => obj != null ? obj.Properties.Any(p => p.Name.Text == name) : str != null && str.Properties.Any(p => p.Name.Text == name);

    /// <summary>The node's instance data: an object (a Blueprint or native condition class) or a struct (an engine condition).</summary>
    static void InstanceData(FStructFallback node, FStructFallback[] instances, out UObject? obj, out FStructFallback? str)
    {
        obj = null;
        str = null;
        if (!node.TryGetValue(out FStructFallback at, "InstanceTemplateIndex")) return;
        int i = IntOf(at, "Value");
        if (i < 0 || i >= instances.Length || instances[i] == null) return;
        if (instances[i].TryGetValue(out FPackageIndex o, "InstanceObject") && o != null && !o.IsNull) obj = Lv.Resolve(o);
        else str = instances[i];
    }

    /// <summary>A setting as a number: the value of the parameter it is bound to, or its own; null when it is bound to anything else or the parameter has no value.</summary>
    static double? Setting(UObject? obj, FStructFallback? str, Dictionary<string, Source> binds, Dictionary<string, object> prm, string name)
    {
        if (binds.TryGetValue(name, out var src))
        {
            if (src.Kind != "Parameter" || !prm.TryGetValue(src.Path, out var v)) return null;
            return NumberOf(v);
        }
        return InstanceNumber(obj, str, name);
    }

    static bool? SettingBool(UObject? obj, FStructFallback? str, Dictionary<string, Source> binds, Dictionary<string, object> prm, string name)
    {
        if (binds.TryGetValue(name, out var src))
        {
            if (src.Kind != "Parameter" || !prm.TryGetValue(src.Path, out var v)) return null;
            return v is bool b ? b : NumberOf(v) is { } n ? n != 0 : null;
        }
        return InstanceBool(obj, str, name);
    }

    static bool IsSelf(Source s) => s.Kind == "Context" && s.Name == "Actor" && s.Path.Length == 0;
    static bool IsPrimaryTarget(Source s) =>
        (s.Kind == "Evaluator" && (s.Class == "AoCSTE_UpdateMostHated" || s.Class == "STE_UpdateMostHated_C") && s.Path == "PrimaryTarget") ||   // (both name the most hated character)
        // a creature kind's own target evaluator (the Gorilla's, a Raptor's, the Hippo's tunnel vision, the Living Geode's, the petrified Basilisk's most hated) and the actor's own
        // PrimaryTarget: each names the character the creature fights, which the world's creature has one of (modelled: their own rules for picking it are not followed)
        (s.Kind == "Evaluator" && s.Class.StartsWith("STE_", StringComparison.Ordinal) && (s.Class.Contains("UpdateTarget", StringComparison.Ordinal) || s.Class.Contains("UpdateMostHated", StringComparison.Ordinal) ||
                                                                                         s.Class.Contains("TunnelVisionTarget", StringComparison.Ordinal) || s.Class.Contains("GeodeTarget", StringComparison.Ordinal)) &&
         s.Path.EndsWith("PrimaryTarget", StringComparison.Ordinal)) ||
        (s.Kind == "Context" && s.Name == "Actor" && s.Path == "PrimaryTarget");

    static string Escape(string s)
    {
        var sb = new System.Text.StringBuilder();
        foreach (char c in s)
        {
            if (char.IsAsciiLetterOrDigit(c) || c == '_' || c == '.' || c == '-') sb.Append(c);
            else sb.Append('%').Append(((int)c).ToString("X2", CultureInfo.InvariantCulture));
        }
        return sb.ToString();
    }

    static string Num(double v) => v.ToString("R", CultureInfo.InvariantCulture);

    static Atom Constant(bool value) => new Atom { Text = value ? "T" : "F", Constant = value ? "T" : "F" };
    static Atom Unsaid(string why) => new Atom { Text = "?" + Escape(why), Unknown = true };

    static string OpOf(string generic) => generic switch
    {
        "Equal" => "eq",
        "NotEqual" => "ne",
        "Less" => "lt",
        "LessOrEqual" => "le",
        "Greater" => "gt",
        "GreaterOrEqual" => "ge",
        _ => ""
    };

    static string Negate(string op) => op switch { "eq" => "ne", "ne" => "eq", "lt" => "ge", "ge" => "lt", "le" => "gt", "gt" => "le", _ => op };

    static bool Compare(string op, double a, double b) => op switch { "eq" => Math.Abs(a - b) < 1e-9, "ne" => Math.Abs(a - b) >= 1e-9, "lt" => a < b, "le" => a <= b, "gt" => a > b, "ge" => a >= b, _ => false };

    /// <summary>Where the task whose instance data is `handle` takes its input `input` from: "" when nothing is bound to it, "Kind:Class.Path" when something is, null for no such task.</summary>
    static string? TaskInputSource(TreeInfo t, FStructFallback handle, string input)
    {
        foreach (var tn in t.Nodes)
        {
            var th = tn == null ? null : StructOf(tn, "InstanceDataHandle");
            if (th == null || !SameHandle(th, handle)) continue;
            return BindingsOf(t, tn).TryGetValue(input, out var src) ? src.Kind + ":" + src.Class + "." + src.Path : "";
        }
        return null;
    }

    /// <summary>One condition node of a state's enter conditions, said as an atom.</summary>
    static Atom Encode(TreeInfo t, FStructFallback node, HashSet<int> path, Dictionary<string, object> prm)
    {
        string mode = TextOf(node, "EvaluationMode");
        if (mode == "ForcedTrue") return Constant(true);
        if (mode == "ForcedFalse") return Constant(false);
        string cls = ClassOf(node, "ConditionClass");
        InstanceData(node, t.Shared, out var obj, out var str);
        var binds = BindingsOf(t, node);
        bool Inv(string name) => (SettingBool(obj, str, binds, prm, name) ?? false);
        bool Targeted(string a, string b, bool ordered)
        {
            if (!binds.TryGetValue(a, out var sa) || !binds.TryGetValue(b, out var sb)) return false;
            return (IsSelf(sa) && IsPrimaryTarget(sb)) || (!ordered && IsPrimaryTarget(sa) && IsSelf(sb));
        }
        switch (cls)
        {
            case "AoCSTC_DistanceCheck":
            {
                var d = Setting(obj, str, binds, prm, "Distance");
                if (d == null) return Unsaid(cls + "(distance)");
                if (!Targeted("Agent", "Target", false)) return Unsaid(cls + "(other target)");
                bool inverse = Inv("bInverse");
                return new Atom { Text = (inverse ? "out(" : "in(") + Num(d.Value) + ")", Kind = "dist", Value = d.Value, Inverse = inverse };
            }
            case "AoCSTC_AngleCheck":
            {
                var a = Setting(obj, str, binds, prm, "Angle");
                if (a == null) return Unsaid(cls + "(angle)");
                if (!Targeted("Agent", "Target", true)) return Unsaid(cls + "(other target)");
                if (Inv("bInverse")) return new Atom { Text = "nang(" + Num(a.Value) + ")" };   // the target is NOT within the angle of the facing (behind it, to a side)
                return new Atom { Text = "ang(" + Num(a.Value) + ")", Kind = "ang", Value = a.Value };
            }
            case "AoCSTC_CooldownCheck":
            {
                string name = obj != null ? NameOf(obj, "TaskIdentifier") : "";
                if (name.Length == 0 || Inv("bInverse")) return Unsaid(cls + (name.Length == 0 ? "(no name)" : "(inverse)"));
                return new Atom { Text = "cd(" + Escape(name) + ")", Kind = "cd", Name = name };
            }
            case "STC_Distance_C":
            {
                var d = Setting(obj, str, binds, prm, "Distance");
                if (d == null) return Unsaid(cls + "(distance)");
                if (!Targeted("Agent", "Target", false)) return Unsaid(cls + "(other target)");
                return new Atom { Text = (Inv("Inverse") ? "cout(" : "cin(") + Num(d.Value) + ")" };
            }
            case "STC_Angle_C":
            {
                var a = Setting(obj, str, binds, prm, "Angle");
                if (a == null) return Unsaid(cls + "(angle)");
                if (!Targeted("Agent", "Target", true) || Inv("Inverse") || Inv("Directional")) return Unsaid(cls + (Inv("Directional") ? "(directional)" : Inv("Inverse") ? "(inverse)" : "(other target)"));
                return new Atom { Text = "ang(" + Num(a.Value) + ")", Kind = "ang", Value = a.Value };
            }
            case "AoCSTC_HealthPercentageCheck":
            case "STC_HealthPercentage_C":
            {
                var thr = Setting(obj, str, binds, prm, "Threshold");
                if (thr == null) return Unsaid(cls + "(threshold)");
                if (!binds.TryGetValue("Target", out var who) || !IsSelf(who)) return Unsaid(cls + "(other creature)");
                bool inverse = Inv(cls == "STC_HealthPercentage_C" ? "Inverse" : "bInverse");
                return new Atom { Text = (inverse ? "nhp(" : "hp(") + Num(thr.Value) + ")" };
            }
            case "AoCSTC_EvaluateExpression":
            case "STC_ExpressionBool_C":
            {
                // "GetEffectCount(GetSelf(), <effect>) > 0": the effects another creature's ability puts on a creature (a rally) are not in our world, so every creature carries none and the count is 0
                var holder = obj?.Properties.FirstOrDefault(x => x.Name.Text == (cls == "AoCSTC_EvaluateExpression" ? "ExpressionToEvaluate" : "ExpressionValue"))?.Tag?.GenericValue is FScriptStruct es ? es.StructType as FStructFallback : null;
                if (holder == null || !holder.TryGetValue(out string expression, "Expression")) return Unsaid(cls + "(expression)");
                // "GetArraySize(GetHatedCharacters() # {GetDistanceBetween(GetSource(),@) < D}) > 0": someone on the creature's hate list is nearer than D (cm, centre to centre)
                var hated = HatedWithin.Match(expression);
                if (hated.Success) return new Atom { Text = "hate(" + Num(Parsed(hated.Groups[1])) + ")" };
                // "GetNpcEliteRank(GetSource()) >= N": the creature's own elite rank
                var elite = EliteRank.Match(expression);
                if (elite.Success)
                {
                    string eop = elite.Groups[1].Value switch { ">" => "gt", ">=" => "ge", "<" => "lt", "<=" => "le", "==" => "eq", "!=" => "ne", _ => "" };
                    if (eop.Length == 0) return Unsaid(cls + "(operator)");
                    return new Atom { Text = "elite(" + eop + "," + Num(Parsed(elite.Groups[2])) + ")" };
                }
                var counted = EffectCount.Match(expression);
                if (!counted.Success) return Unsaid(cls);
                string op = counted.Groups[1].Value switch { ">" => "gt", ">=" => "ge", "<" => "lt", "<=" => "le", "==" => "eq", "!=" => "ne", _ => "" };
                if (op.Length == 0) return Unsaid(cls + "(operator)");
                return Constant(Compare(op, 0, Parsed(counted.Groups[2])));
            }
            case "STC_HasEffect_C":
            {
                // the creature itself (its Actor), or the character it hates most, holds the effect record (Effect) / does not (Invert)
                var holder = obj?.Properties.FirstOrDefault(x => x.Name.Text == "Effect")?.Tag?.GenericValue is FScriptStruct es ? es.StructType as FStructFallback : null;
                if (holder == null || !holder.TryGetValue(out long effect, "Guid") || effect == 0) return Unsaid(cls + "(effect)");
                if (!binds.TryGetValue("Target", out var who)) return Unsaid(cls + "(no target)");
                string kind = IsSelf(who) ? "eff" : IsPrimaryTarget(who) ? "teff" : "";
                if (kind.Length == 0) return Unsaid(cls + "(other actor)");
                return new Atom { Text = (Inv("Invert") ? "n" : "") + kind + "(" + effect.ToString(CultureInfo.InvariantCulture) + ")" };
            }
            case "STC_Random_C":
            {
                var p = Setting(obj, str, binds, prm, "Probability");
                return p == null ? Unsaid(cls + "(probability)") : new Atom { Text = "rand(" + Num(p.Value) + ")" };
            }
            case "STC_HasLineOfSight_C":
            {
                if (!binds.TryGetValue("Agent", out var agent) || !IsSelf(agent) || !binds.TryGetValue("TargetActor", out var target) || !IsPrimaryTarget(target)) return Unsaid(cls + "(other target)");
                return new Atom { Text = Inv("Inverse") ? "nlos" : "los" };
            }
        }
        if (cls == "STC_UtilityScoreTargetSelector_C")
        {
            // it picks a target out of its consideration context (the creature's hated characters, maybe within a band of distance) into the blackboard, and holds when there is one to
            // pick: said by the context alone (the pick's own target is not followed: the ability goes to the creature's target, a model)
            var context = obj?.Properties.FirstOrDefault(x => x.Name.Text == "Consideration Context")?.Tag?.GenericValue is FScriptStruct cs2 ? cs2.StructType as FStructFallback : null;
            if (context == null || !context.TryGetValue(out string ctxExpr, "Expression")) return Unsaid(cls + "(context)");
            if (AllHated.IsMatch(ctxExpr)) return Constant(true);
            var within = HatedNearer.Match(ctxExpr);
            if (within.Success) return new Atom { Text = "hate(" + Num(Parsed(within.Groups[1])) + ")" };
            var band = HatedBand.Match(ctxExpr);
            if (band.Success)
            {
                double hi = Parsed(band.Groups["hi"]), lo = Parsed(band.Groups["lo"]);
                if (lo >= hi) return Unsaid(cls + "(band)");
                return new Atom { Text = "hateb(" + Num(lo) + "," + Num(hi) + ")" };
            }
            return Unsaid(cls + "(context)");
        }
        if (cls == "STC_EliteRankThreshold_C")
        {
            // the creature's own elite rank is at least EliteRank (GreaterThanOrEqual), or under it
            if (!binds.TryGetValue("Target", out var who) || !IsSelf(who)) return Unsaid(cls + "(other actor)");
            var rank = InstanceNumber(obj, str, "EliteRank");
            if (rank == null) return Unsaid(cls + "(rank)");
            bool atLeast = InstanceBool(obj, str, "GreaterThanOrEqual") ?? false;
            return new Atom { Text = "elite(" + (atLeast ? "ge" : "lt") + "," + Num(rank.Value) + ")" };
        }
        if (cls == "STC_EffectCount_C")
        {
            // the stacks of an effect record the creature (or the character it hates most) holds, compared with Compare Value
            var holder = obj?.Properties.FirstOrDefault(x => x.Name.Text == "Effect")?.Tag?.GenericValue is FScriptStruct es2 ? es2.StructType as FStructFallback : null;
            if (holder == null || !holder.TryGetValue(out long effect, "Guid") || effect == 0) return Unsaid(cls + "(effect)");
            var count = InstanceNumber(obj, str, "Compare Value");
            if (count == null) return Unsaid(cls + "(count)");
            string op = (obj?.Properties.FirstOrDefault(x => x.Name.Text == "CompareOperation")?.Tag?.GenericValue?.ToString() ?? "") switch
            {
                "ECompareOperationEnum::Dataflow_Compare_Equal" => "eq",
                "ECompareOperationEnum::Dataflow_Compare_NotEqual" => "ne",
                "ECompareOperationEnum::Dataflow_Compare_Greater" => "gt",
                "ECompareOperationEnum::Dataflow_Compare_GreaterOrEqual" => "ge",
                "ECompareOperationEnum::Dataflow_Compare_Smaller" => "lt",
                "ECompareOperationEnum::Dataflow_Compare_SmallerOrEqual" => "le",
                _ => ""
            };
            if (op.Length == 0) return Unsaid(cls + "(operator)");
            if (!binds.TryGetValue("Target", out var who)) return Unsaid(cls + "(no target)");
            string kind = IsSelf(who) ? "effc" : IsPrimaryTarget(who) ? "teffc" : "";
            if (kind.Length == 0) return Unsaid(cls + "(other actor)");
            return new Atom { Text = kind + "(" + op + "," + Num(count.Value) + "," + effect.ToString(CultureInfo.InvariantCulture) + ")" };
        }
        if (cls == "STC_IncreasedRandomChanceForMissingHealth_C")
        {
            // its script (read from the client): false without a Target; true when HealthThresholdForMaxChance is not under 1 or the target's health (GetHealthPercent / 100) is not
            // over it; else a RandomFloat <= HealthCurve at 1 - that health.  Said of the creature itself; the threshold's class default is 0.2
            if (!binds.TryGetValue("Target", out var who) || !IsSelf(who)) return Unsaid(cls + "(other actor)");
            double threshold = InstanceNumber(obj, str, "HealthThresholdForMaxChance") ?? 0.2;
            var curveRef = obj?.Properties.FirstOrDefault(x => x.Name.Text == "HealthCurve")?.Tag?.GenericValue as FPackageIndex;
            if (curveRef == null || !curveRef.TryLoad(out UObject? curveAsset) || curveAsset == null || !curveAsset.TryGetValue(out FRichCurve curve, "FloatCurve") || curve?.Keys == null || curve.Keys.Length == 0)
                return Unsaid(cls + "(curve)");
            if (curve.PreInfinityExtrap != ERichCurveExtrapolation.RCCE_Constant || curve.PostInfinityExtrap != ERichCurveExtrapolation.RCCE_Constant) return Unsaid(cls + "(extrapolation)");
            var keys = new List<string>();
            for (int i = 0; i < curve.Keys.Length; i++)
            {
                var key = curve.Keys[i];
                string interp = key.InterpMode switch { ERichCurveInterpMode.RCIM_Constant => "0", ERichCurveInterpMode.RCIM_Linear => "1", ERichCurveInterpMode.RCIM_Cubic => "2", _ => "" };
                if (interp.Length == 0) return Unsaid(cls + "(curve mode)");
                if (interp == "2" && i + 1 < curve.Keys.Length && WeightedSegment(key, curve.Keys[i + 1]))
                {
                    // a cubic with a weighted tangent on either side: the engine's weighted Bezier, said as straight pieces
                    var next = curve.Keys[i + 1];
                    for (int step = 0; step < WeightedSteps; step++)
                    {
                        double time = key.Time + (next.Time - key.Time) * step / WeightedSteps, value = WeightedCubic(key, next, time);
                        if (double.IsNaN(value)) return Unsaid(cls + "(weighted curve that turns back)");
                        keys.Add("1," + Num(time) + "," + Num(value) + ",0,0");
                    }
                    continue;
                }
                keys.Add(interp + "," + Num(key.Time) + "," + Num(key.Value) + "," + Num(key.ArriveTangent) + "," + Num(key.LeaveTangent));
            }
            return new Atom { Text = "mhr(" + Num(threshold) + "," + string.Join(",", keys) + ")" };
        }
        if (cls.Length > 0) return Unsaid(cls);
        // an engine condition: it is told apart by the settings of its instance data
        if (str == null) return Unsaid("struct:" + TextOf(node, "Name"));
        bool invert = node.TryGetValue(out bool bInvert, "bInvert") && bInvert;
        if (HasInstanceProperty(null, str, "bLeft"))
        {
            var left = SettingBool(null, str, binds, prm, "bLeft");
            var right = SettingBool(null, str, binds, prm, "bRight");
            if (left == null || right == null) return Unsaid("Bool Compare(" + (binds.TryGetValue("bLeft", out var s0) ? s0.ToString() : "") + ")");
            return Constant((left.Value == right.Value) != invert);
        }
        if (HasInstanceProperty(null, str, "Threshold") && !HasInstanceProperty(null, str, "Left"))
        {
            var thr = Setting(null, str, binds, prm, "Threshold");
            return thr == null ? Unsaid("Random(threshold)") : new Atom { Text = "rand(" + Num(thr.Value) + ")" };
        }
        if (HasInstanceProperty(null, str, "Left") && HasInstanceProperty(null, str, "Right"))
        {
            string op = OpOf(TextOf(node, "Operator"));
            if (op.Length == 0) return Unsaid("Compare(operator)");
            if (invert) op = Negate(op);
            // a timer's charge (the creature's accumulators: its global cooldown, a special attack's): the timer holds one charge once its seconds have passed since it was emptied, so "has a charge" is a cooldown that is over
            if (binds.TryGetValue("Left", out var timer) && timer.Kind == "Task" && !timer.Global && IsTimerAccumulator(timer.Class) && timer.Path == "Charge")
            {
                var charges = Setting(null, str, binds, prm, "Right");
                var accumulator = charges == null ? null : AccumulatorOf(t, timer);
                if (charges == null || accumulator == null) return Unsaid("Compare(accumulator:" + timer.Name + ")");
                bool atZero = Compare(op, 0, charges.Value), atOne = Compare(op, 1, charges.Value);
                if (atOne && !atZero && accumulator.Full) return new Atom { Text = "cd(" + Escape("acc." + accumulator.Identifier) + ")", Kind = "cd", Name = "acc." + accumulator.Identifier };
                if (atOne && !atZero)
                {
                    // one that starts empty, in the fight's own state or the one just above it (Aggroed > Combat: entered when the fight begins): its first charge comes its
                    // seconds into the fight (rolled once per fight for a range), and after that it is the cooldown its use starts
                    bool IsCombat(int s) => string.Equals(TextOf(t.States[s], "Name"), "Combat", StringComparison.OrdinalIgnoreCase);
                    bool fightState = path.Contains(timer.State) && (IsCombat(timer.State) ||
                        path.Any(p => IsCombat(p) && t.States[p].TryGetValue(out FStructFallback up, "Parent") && IntOf(up, "Index") == timer.State));
                    string where = timer.State >= 0 && timer.State < t.States.Length ? TextOf(t.States[timer.State], "Name") : "";
                    if (!fightState) return Unsaid("Compare(accumulator empty, in state " + where + ")");
                    // (it stays in `conds`, not `gates`: the atom asks its cooldown itself, with the fight's clock)
                    return new Atom { Text = "accf(" + Escape("acc." + accumulator.Identifier) + "," + Num(accumulator.Low) + "," + Num(accumulator.High) + ")" };
                }
                if (atOne == atZero) return Constant(atOne);
                return Unsaid("Compare(accumulator empty:" + timer.Name + ")");
            }
            // the fight's clock: the elapsed time of the "Combat Timer" task of a state on the way down to this condition
            bool clock = binds.TryGetValue("Left", out var l) && l.Kind == "Task" && !l.Global && (l.Name == "Combat Timer" || l.Name == "CombatTimer" || l.Name == "CombatStartTime" || l.Name == "Temporary Combat Timer (Delete when flag fixed)") && l.Path == "TaskElapsedTime" && path.Contains(l.State);
            // an AoCSTT_Base task's TreeElapsedTime (read from the client's code): the class never writes it (its constructor sets -1; its tick counts TaskElapsedTime, its enter copies the
            // tree's time into TaskStartTime), so it is what the tree binds to it: nothing (-1 for good), or the AoCSTE_TrackTime evaluator's TreeElapsedTimeInSeconds, which is 0 at the
            // tree's start and counts every tick from there: the seconds since the creature's tree began.  A creature's tree runs from its spawn, far longer than the few seconds these
            // compare with, so it is said as long run (modelled: a creature that fights within those seconds of its respawn would differ)
            if (l != null && l.Kind == "Task" && l.Class == "AoCSTT_Base" && l.Path == "TreeElapsedTime" && l.Handle != null)
            {
                var against = Setting(null, str, binds, prm, "Right");
                string bound = TaskInputSource(t, l.Handle, "TreeElapsedTime");
                if (against != null && bound == "") return Constant(Compare(op, -1, against.Value));
                if (against != null && bound == "Evaluator:AoCSTE_TrackTime.TreeElapsedTimeInSeconds") return Constant(Compare(op, 1e6, against.Value));
            }
            var right = Setting(null, str, binds, prm, "Right");
            if (right == null)
            {
                // ... against a number the tree rolled when the fight began
                if (clock && binds.TryGetValue("Right", out var rolled) && FightRoll(t, rolled, path) is { } roll) return new Atom { Text = "fcr(" + op + "," + roll + ")" };
                return Unsaid("Compare(right:" + (binds.TryGetValue("Right", out var s1) ? s1.ToString() : "") + ")");
            }
            if (clock) return new Atom { Text = "fc(" + op + "," + Num(right.Value) + ")" };
            var leftValue = Setting(null, str, binds, prm, "Left");
            if (leftValue == null) return Unsaid("Compare(left:" + (binds.TryGetValue("Left", out var s2) ? s2.ToString() : "") + ")");
            return Constant(Compare(op, leftValue.Value, right.Value));
        }
        // "Object Is Valid" on the creature's target: whether it has one (or, inverted, has none)
        if (HasInstanceProperty(null, str, "Object") && binds.TryGetValue("Object", out var checkedObject) && IsPrimaryTarget(checkedObject)) return new Atom { Text = invert ? "ntgt" : "tgt" };
        return Unsaid("struct:" + TextOf(node, "Name"));
    }

    const string NumberText = @"(?:-?[0-9]+\.?[0-9]*|-?\.[0-9]+)";
    static readonly Regex PlainNumber = new(@"^\s*(" + NumberText + @")\s*;?\s*$", RegexOptions.CultureInvariant);
    static readonly Regex RangeOnly = new(@"^\s*RandDoubleRange\(\s*(" + NumberText + @")\s*,\s*(" + NumberText + @")\s*\)\s*;?\s*$", RegexOptions.CultureInvariant);
    static readonly Regex ChanceOfRange = new(@"^\s*var\s+(\w+)\s*=\s*(" + NumberText + @")\s*;\s*if\s*\(\s*RandDouble\(\)\s*<\s*(" + NumberText + @")\s*\)\s*\{\s*set\s+\1\s*=\s*(?:RandDoubleRange\(\s*(" + NumberText + @")\s*,\s*(" + NumberText + @")\s*\)|(" + NumberText + @"))\s*;\s*\}\s*\1\s*;?\s*$", RegexOptions.CultureInvariant);

    static double Parsed(Group g) => double.Parse(g.Value, NumberStyles.Float, CultureInfo.InvariantCulture);

    /// <summary>An expression that compares how many of an effect the creature itself (or the condition's source, the creature) carries with a number.</summary>
    static readonly Regex EffectCount = new(@"^\s*GetEffectCount\(\s*Get(?:Self|Source)\(\)\s*,\s*\$#\d+:\d+\$\s*\)\s*(>=|<=|==|!=|>|<)\s*(" + NumberText + @")\s*$", RegexOptions.CultureInvariant);
    const int WeightedSteps = 32;

    /// <summary>Whether the engine takes its weighted path between two keys: the first's leave tangent or the second's arrive tangent has a weight.</summary>
    static bool WeightedSegment(FRichCurveKey a, FRichCurveKey b) =>
        a.TangentWeightMode is ERichCurveTangentWeightMode.RCTWM_WeightedLeave or ERichCurveTangentWeightMode.RCTWM_WeightedBoth ||
        b.TangentWeightMode is ERichCurveTangentWeightMode.RCTWM_WeightedArrive or ERichCurveTangentWeightMode.RCTWM_WeightedBoth;

    /// <summary>The engine's weighted cubic between two keys at `time` (FRichCurve's WeightedEvalForTwoKeys): a Bezier in time and value whose inner points lie along each
    /// tangent at its weight (an unweighted side: a third of the span's length along the tangent); the Bezier's parameter is the one at which its time is `time`.</summary>
    static double WeightedCubic(FRichCurveKey a, FRichCurveKey b, double time)
    {
        double span = b.Time - a.Time;
        if (!(span > 0)) return a.Value;
        double SideWeight(double tangent, bool weighted, double weight) => weighted ? weight : Math.Sqrt(span * span + tangent * span * tangent * span) / 3;
        double leaveAngle = Math.Atan(a.LeaveTangent), arriveAngle = Math.Atan(b.ArriveTangent);
        double leave = SideWeight(a.LeaveTangent, a.TangentWeightMode is ERichCurveTangentWeightMode.RCTWM_WeightedLeave or ERichCurveTangentWeightMode.RCTWM_WeightedBoth, a.LeaveTangentWeight);
        double arrive = SideWeight(b.ArriveTangent, b.TangentWeightMode is ERichCurveTangentWeightMode.RCTWM_WeightedArrive or ERichCurveTangentWeightMode.RCTWM_WeightedBoth, b.ArriveTangentWeight);
        double x1 = Math.Cos(leaveAngle) * leave / span, y1 = a.Value + Math.Sin(leaveAngle) * leave;
        double x2 = (span - Math.Cos(arriveAngle) * arrive) / span, y2 = b.Value - Math.Sin(arriveAngle) * arrive;
        if (!(x1 >= 0 && x1 <= 1 && x2 >= 0 && x2 <= 1)) return double.NaN;
        static double Bezier(double p0, double p1, double p2, double p3, double u) => (1 - u) * (1 - u) * (1 - u) * p0 + 3 * (1 - u) * (1 - u) * u * p1 + 3 * (1 - u) * u * u * p2 + u * u * u * p3;
        // the time runs one way between the ends when the inner points lie between them (as the client's curves have it): halve towards the parameter at `time`
        double alpha = (time - a.Time) / span, lo = 0, hi = 1;
        for (int i = 0; i < 60; i++)
        {
            double mid = (lo + hi) / 2;
            if (Bezier(0, x1, x2, 1, mid) < alpha) lo = mid; else hi = mid;
        }
        return Bezier(a.Value, y1, y2, b.Value, (lo + hi) / 2);
    }

    static readonly Regex HatedWithin = new(@"^\s*GetArraySize\(\s*GetHatedCharacters\(\)\s*#\s*\{\s*GetDistanceBetween\(\s*(?:GetSource\(\)\s*,\s*@|@\s*,\s*GetSource\(\))\s*\)\s*<\s*(" + NumberText + @")\s*\}\s*\)\s*>\s*0\s*;?\s*$", RegexOptions.CultureInvariant);
    const string HatedDistance = @"GetDistanceBetween\(\s*(?:GetSource\(\)\s*,\s*@|@\s*,\s*GetSource\(\))\s*\)";
    static readonly Regex AllHated = new(@"^\s*GetHatedCharacters\(\)\s*;?\s*$", RegexOptions.CultureInvariant);
    static readonly Regex HatedNearer = new(@"^\s*GetHatedCharacters\(\)\s*#\s*\{\s*" + HatedDistance + @"\s*<\s*(" + NumberText + @")\s*\}\s*;?\s*$", RegexOptions.CultureInvariant);
    static readonly Regex HatedBand = new(@"^\s*GetHatedCharacters\(\)\s*#\s*\{\s*(?:" + HatedDistance + @"\s*<\s*(?<hi>" + NumberText + @")\s*&&\s*" + HatedDistance + @"\s*>\s*(?<lo>" + NumberText + @")|" +
                                          HatedDistance + @"\s*>\s*(?<lo>" + NumberText + @")\s*&&\s*" + HatedDistance + @"\s*<\s*(?<hi>" + NumberText + @"))\s*\}\s*;?\s*$", RegexOptions.CultureInvariant);
    static readonly Regex EliteRank = new(@"^\s*GetNpcEliteRank\(\s*GetSource\(\)\s*\)\s*(>=|<=|==|!=|>|<)\s*(" + NumberText + @")\s*;?\s*$", RegexOptions.CultureInvariant);

    /// <summary>
    /// A number the tree rolls when the fight begins and then compares the fight's clock with: the "Stored Value" of a "Set BBFloat" task in a state the fight began in.  The task's expression is a number, a range
    /// ("RandDoubleRange(6.0, 9.0)") or a chance of one ("var Result = 0.; if (RandDouble() < 0.65) { set Result = RandDoubleRange(6.0, 9.0); } Result").  Said as "base,chance,low,high,name": the number is
    /// `base`, or with that chance a fresh one between `low` and `high`.  Null when the binding is anything else or the expression is of another form.
    /// </summary>
    static string? FightRoll(TreeInfo t, Source src, HashSet<int> path)
    {
        // a "Set BBFloat" task's stored value, or a "Generate Float" task's value (a tree's "initial cooldown" of a special attack)
        // a "Generate Random Float In Range" task's output: a fresh number between its Min and Max each time its state is entered (said as a roll of the fight: with certainty, between them)
        if (src.Kind == "Task" && !src.Global && src.Class == "STT_GenerateRandomFloatInRange_C" && src.Path == "Output" && src.Handle != null && path.Contains(src.State))
        {
            foreach (var node in t.Nodes)
            {
                var handle = node == null ? null : StructOf(node, "InstanceDataHandle");
                if (handle == null || !SameHandle(handle, src.Handle)) continue;
                if (node.TryGetValue(out bool on, "bTaskEnabled") && !on) return null;
                if (BindingsOf(t, node).ContainsKey("Min") || BindingsOf(t, node).ContainsKey("Max")) return null;
                var task = InstanceOf(node, t.Defaults);
                var lo = task == null ? null : NumberOf(task.Properties.FirstOrDefault(p => p.Name.Text == "Min")?.Tag?.GenericValue);
                var hi = task == null ? null : NumberOf(task.Properties.FirstOrDefault(p => p.Name.Text == "Max")?.Tag?.GenericValue);
                if (lo == null || hi == null || !(lo.Value <= hi.Value) || src.Name.Length == 0) return null;
                return Num(lo.Value) + ",1," + Num(lo.Value) + "," + Num(hi.Value) + "," + Escape(src.Name);
            }
            return null;
        }
        bool stored = src.Class == "STT_SetBBFloat_C" && src.Path == "Stored Value", generated = src.Class == "STT_GenerateFloat_C" && src.Path == "Value";
        if (src.Kind != "Task" || src.Global || !(stored || generated) || src.Handle == null || !path.Contains(src.State)) return null;
        foreach (var node in t.Nodes)
        {
            if (node == null || ClassOf(node, "TaskClass") != src.Class) continue;
            var handle = StructOf(node, "InstanceDataHandle");
            if (handle == null || !SameHandle(handle, src.Handle)) continue;
            if (node.TryGetValue(out bool enabled, "bTaskEnabled") && !enabled) return null;
            var task = InstanceOf(node, t.Defaults);
            if (task == null) return null;
            string name = stored ? NameOf(task, "Variable Name") : src.Name;
            var expression = task.Properties.FirstOrDefault(p => p.Name.Text == (stored ? "Value Expression" : "Value Expr"))?.Tag?.GenericValue is FScriptStruct ss ? ss.StructType as FStructFallback : null;
            if (name.Length == 0 || expression == null || !expression.TryGetValue(out string text, "Expression")) return null;
            double basis, chance, low, high;
            Match m;
            if ((m = PlainNumber.Match(text)).Success) { basis = Parsed(m.Groups[1]); chance = 0; low = high = basis; }
            else if ((m = RangeOnly.Match(text)).Success) { basis = 0; chance = 1; low = Parsed(m.Groups[1]); high = Parsed(m.Groups[2]); }
            else if ((m = ChanceOfRange.Match(text)).Success)
            {
                basis = Parsed(m.Groups[2]);
                chance = Parsed(m.Groups[3]);
                low = m.Groups[4].Success ? Parsed(m.Groups[4]) : Parsed(m.Groups[6]);
                high = m.Groups[5].Success ? Parsed(m.Groups[5]) : low;
            }
            else return null;
            if (!(chance >= 0 && chance <= 1) || !(low <= high)) return null;
            return Num(basis) + "," + Num(chance) + "," + Num(low) + "," + Num(high) + "," + Escape(name);
        }
        return null;
    }

    /// <summary>A timer of the tree that holds a charge once `Low` to `High` seconds (a number, or a range the tree rolls anew each time) have passed since it was emptied.  `Full`: it
    /// starts with its charge (InitialCharge 1); else it starts empty and its first charge comes those seconds after its state is entered.</summary>
    sealed record Accumulator(string Identifier, double Low, double High, bool Full = true);

    static bool TargetSeconds(string text, out double low, out double high)
    {
        Match m;
        low = high = 0;
        if ((m = PlainNumber.Match(text)).Success) { low = high = Parsed(m.Groups[1]); return true; }
        if ((m = RangeOnly.Match(text)).Success) { low = Parsed(m.Groups[1]); high = Parsed(m.Groups[2]); return low <= high; }
        return false;
    }

    /// <summary>An accumulator task: its identifier and seconds; null when it holds more than one charge or its seconds are of another form.  (STT_Accumulator_C, read from the
    /// client's script: entering its state sets Charge to InitialCharge, whose class default is 0, and ProgressValue to 0; each tick adds the seconds while Charge is under
    /// MaxCharge, class default 1, and a charge comes once they reach the target.)</summary>
    static Accumulator? AccumulatorTask(TreeInfo t, FStructFallback node)
    {
        if (node.TryGetValue(out bool enabled, "bTaskEnabled") && !enabled) return null;
        var task = InstanceOf(node, t.Defaults);
        if (task == null) return null;
        string id = NameOf(task, "Identifier");
        var expression = task.Properties.FirstOrDefault(p => p.Name.Text == "Target Value Expr")?.Tag?.GenericValue is FScriptStruct ss ? ss.StructType as FStructFallback : null;
        if (id.Length == 0 || expression == null || !expression.TryGetValue(out string text, "Expression") || !TargetSeconds(text, out double low, out double high)) return null;
        double initial = InstanceNumber(task, null, "InitialCharge") ?? 0, most = InstanceNumber(task, null, "MaxCharge") ?? 1;
        if (most != 1) return null;
        return new Accumulator(id, low, high, initial >= 1);
    }

    /// <summary>
    /// A timer accumulator: STT_Accumulator_Timer_C, or a creature's own kind whose Accumulate (read from the client's script) calls the timer's and then adds more: the zombie's ranged
    /// attack while its target stands farther than MinTooFarDis, the gorilla's smack and leap for each further character it hates in front of it.  Those are said as the timer
    /// alone (modelled: their charge comes no sooner than the timer's, at times later than the game's).
    /// </summary>
    static bool IsTimerAccumulator(string cls) => cls is "STT_Accumulator_Timer_C" or "STT_Accumulator_Zombie_RangedAttack_C" or "STT_Accumulator_Gorilla_PowerSmack_C";

    /// <summary>The accumulator timer that a bound source names.</summary>
    static Accumulator? AccumulatorOf(TreeInfo t, Source src)
    {
        if (src.Handle == null) return null;
        foreach (var node in t.Nodes)
        {
            if (node == null || !IsTimerAccumulator(ClassOf(node, "TaskClass"))) continue;
            var handle = StructOf(node, "InstanceDataHandle");
            if (handle != null && SameHandle(handle, src.Handle)) return AccumulatorTask(t, node);
        }
        return null;
    }

    /// <summary>
    /// The accumulators a task's completion empties: a state on the task's way down (below the Combat state) moves on, when it is done and with no condition, to a state with a task that consumes the
    /// timer's charge.  Said as (identifier, low, high seconds until it holds a charge again).
    /// </summary>
    static List<(string Name, double Low, double High)> ConsumedAfter(TreeInfo t, List<int> path)
    {
        var found = new List<(string Name, double Low, double High)>();
        var timers = new Dictionary<string, Accumulator>(StringComparer.Ordinal);
        foreach (var node in t.Nodes)
            if (node != null && IsTimerAccumulator(ClassOf(node, "TaskClass")) && AccumulatorTask(t, node) is { } timer) timers.TryAdd(timer.Identifier, timer);
        for (int i = path.Count - 1; i >= 0; i--)
        {
            int ps = path[i];
            if (string.Equals(TextOf(t.States[ps], "Name"), "Combat", StringComparison.OrdinalIgnoreCase)) break;
            int trb = IntOf(t.States[ps], "TransitionsBegin", 0), trn = IntOf(t.States[ps], "TransitionsNum", 0);
            for (int k = trb; k < trb + trn && k < t.Transitions.Length; k++)
            {
                var tr = t.Transitions[k];
                if (!TextOf(tr, "Trigger").Contains("OnStateCompleted", StringComparison.Ordinal) || IntOf(tr, "ConditionsNum", 0) != 0) continue;
                var target = StructOf(tr, "State");
                int to = target != null ? IntOf(target, "Index") : -1;
                if (to < 0 || to >= t.States.Length) continue;
                int tb = IntOf(t.States[to], "TasksBegin", 0), tn = IntOf(t.States[to], "TasksNum", 0);
                for (int j = tb; j < tb + tn && j < t.Nodes.Length; j++)
                {
                    // STT_ConsumeAccumulatorCharges_C, or STT_ChargeAccumulator_C adding a negative Number (its class default: -1, resetting and rolling anew), which is the same
                    // (both read from the client's scripts: each calls the creature's delegate of that name)
                    if (t.Nodes[j] == null) continue;
                    string consumer = ClassOf(t.Nodes[j], "TaskClass");
                    var task = InstanceOf(t.Nodes[j], t.Defaults);
                    if (consumer == "STT_ChargeAccumulator_C") { if (task == null || (InstanceNumber(task, null, "Number") ?? -1) >= 0) continue; }
                    else if (consumer != "STT_ConsumeAccumulatorCharges_C") continue;
                    if (task != null && timers.TryGetValue(NameOf(task, "Identifier"), out var timer) && !found.Any(f => f.Name == timer.Identifier)) found.Add((timer.Identifier, timer.Low, timer.High));
                }
            }
        }
        return found;
    }

    /// <summary>The names of the states on a task's way down, root first.</summary>
    static bool InFight(TreeInfo t, List<int> path) => path.Any(p => string.Equals(TextOf(t.States[p], "Name"), "Combat", StringComparison.OrdinalIgnoreCase));
}
