// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Text;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Objects.UObject;

namespace Mapc;

/// <summary>
/// The native class of each Blueprint in the folders worldd makes quest objects from (server/worldd/spawns/generic_npc.cpp, prop_template): one line per class, in archetype order,
///   archetype  native  parents
/// `archetype` is the class default object as the population data names it ("/Game/.../Pkg.Default__Pkg_C"), `native` the first class up the chain that is no Blueprint
/// (InteractableStateMachineBase, Actor, NPCCharacter ...) and `parents` the Blueprints between them, nearest first, comma separated ("" for none).
/// worldd opens a quest object from a recorded InteractableStateMachineBase, whose actor block writes that class's properties; a client whose Blueprint is another class
/// (JUN_HAR_ARCQ_1204_ForgingFurnace is a plain Actor) reads a handle its class does not have and closes the connection, so worldd opens only the lines whose native class is that one.
/// </summary>
public static class PropClasses
{
    public const string FileName = "prop_classes.tsv";
    public const string Header = "archetype\tnative\tparents";

    // the package paths of worldd's kFolders (generic_npc.cpp prop_template), under the containers' root
    public static readonly string[] Folders = {
        "AOC/Content/DesignAssets/Interactables/Quest/", "AOC/Content/DesignAssets/Quests/", "AOC/Content/DesignAssets/Node/ServiceBuildings/ServiceBuilding_Scaffolding.",
        "AOC/Content/DesignAssets/Interactables/SimpleInteractables/BASE_Despawning_Interactable_Sparkly.", "AOC/Content/Effects/FX_Environment/POI/Dun_Stoic_Shield/Blueprints/BP_VFX_Dun_Statue_Stoic_Shield.",
        "AOC/Content/DesignAssets/World/Props/Interactables/Prop_Despawning_Crate." };

    public static IEnumerable<string> AllPaths(Game g) =>
        g.Provider.Files.Keys.Where(k => k.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase) && Folders.Any(f => k.StartsWith(f, StringComparison.OrdinalIgnoreCase))).OrderBy(k => k, StringComparer.Ordinal);

    public static List<(string Archetype, string Native, string Parents)> Collect(Game g, IEnumerable<string> packagePaths, out int packages, out int failed)
    {
        var rows = new List<(string, string, string)>();
        packages = 0; failed = 0;
        foreach (var path in packagePaths)
        {
            packages++;
            IPackage? pkg;
            try { pkg = g.Provider.LoadPackage(path); } catch { failed++; continue; }
            if (pkg == null) { failed++; continue; }
            string pkgPath = "/Game/" + path.Substring("AOC/Content/".Length, path.Length - "AOC/Content/".Length - ".uasset".Length);
            foreach (var e in pkg.GetExports())
            {
                if (e is not UClass cls || !e.Name.EndsWith("_C", StringComparison.Ordinal)) continue;
                var parents = new List<string>();
                string native = "";
                UStruct? s = cls;
                for (int i = 0; s != null && i < 16; i++)
                {
                    if (s.GetType().Name != "UBlueprintGeneratedClass") { native = s.Name; break; }
                    if (i > 0) parents.Add(s.Name);
                    UStruct? sup = null;
                    try { sup = s.SuperStruct?.Load<UStruct>(); } catch { }
                    if (sup == null) { native = s.SuperStruct?.Name ?? ""; break; }
                    s = sup;
                }
                if (native.Length == 0) { failed++; continue; }
                rows.Add((pkgPath + ".Default__" + cls.Name, native, string.Join(",", parents)));
            }
        }
        rows.Sort((a, b) => string.CompareOrdinal(a.Item1, b.Item1));
        return rows;
    }

    public static string ToTsv(IEnumerable<(string Archetype, string Native, string Parents)> rows)
    {
        var sb = new StringBuilder();
        sb.Append(Header).Append('\n');
        foreach (var (a, n, p) in rows) sb.Append(a).Append('\t').Append(n).Append('\t').Append(p).Append('\n');
        return sb.ToString();
    }
}
