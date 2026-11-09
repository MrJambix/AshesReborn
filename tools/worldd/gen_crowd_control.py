#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
"""Writes server/worldd/combat/crowd_control_table.h from dist/content/content.db.

contentc keeps CrowdControlRecordBase and FragileEffectDefRecordBase only as raw records, and worldd reads no raw record,
so what a creature's effects need of them is generated: what each crowd control stops and how hard it slows, and which
effects a hit breaks.  Run from the repository root after a content rebuild.
"""
import gzip
import json
import re
import sqlite3
import sys

DB = "dist/content/content.db"
OUT = "server/worldd/combat/crowd_control_table.h"

ANIMATIONS = {"Stunned": 0, "Tripped": 1, "Sleep": 2, "Custom": 242, "None": 255}   # ECrowdControlAnimationRequest, layouts/aoc.usmap
DISABLE_ALL = 6064631787339448346
DISABLE_SILENCE = 6064631437284477277
DISABLE_DISARM = 6064631437318293854
BREAK_INSTANT = 6064629064270479361
BREAK_SHARE = {6064629165877559296: 0.25, 6064629217391214593: 0.10}
BREAK_IF_HOLDS = 6064634205801943203    # HunterIncapBreak: GetEffectCount(GetOwner(), <effect>) >= 1
BREAK_WITH_CAST = 6064634206182515774   # HunterIncapBreakRanged: !IsUsingAbility(GetInstigator(), Incapacitate)
PUSH_CM = {"Prototype_KnockBack_Light": 200.0, "Prototype_KnockBack_Medium": 400.0, "Prototype_KnockBack_Heavy": 700.0}
# EffectModifier records: their DeltaSeconds are expressions no table compiles, and there are six.
MODIFY_KINDS = {
    6064633533119271241: ("Delta", -1.0, 0, True),                     # Summoner_Hex_1s: me or my pet
    6064634160833708079: ("Delta", -2.0, 0, False),                    # Summoner_Hex_2s
    6064634218302079773: ("Delta", -2.0, 6064634217384187415, False),  # Occultism
    6064634171570284970: ("Delta", 6.0, 6064634284702040774, False),   # Prolonging
    6064631953332947302: ("Elapsed", 0.0, 0, False),                   # Mage_ThermalEquilibrium: its elapsed time again
    6064633187206235248: ("AtLeastOne", 0.0, 0, False),                # Mage_ArcaneEye_Reveal: at least a second left
}
# HitModsIds: multipliers on a hit's damage, as each record's one term and trigger say.
HIT_MODS = {
    "Status_Humiliated": (0.85, "OutgoingNotAtInstigator"),
    "Status_Exposed": (1.25, "IncomingFromEffects"),
    "Rogue_DeathsMark": (1.10, "IncomingFromInstigator"),
    "Rogue_DeathsMark_Final": (1.10, "IncomingFromInstigator"),
}


def volume_radius(node):
    """The largest size of the volumes a targeting record lists, 0 for a record that lists none."""
    best = 0.0
    if isinstance(node, dict):
        shape = next((v for v in node.values() if isinstance(v, str) and v.startswith("EHitboxShape::")), None)
        if shape:
            if shape.endswith("SHAPE_Sphere"):
                best = float(node.get("SphereRadius") or 0)
            elif shape.endswith("SHAPE_Box"):
                box = node.get("BoxExtent") or {}
                best = max(float(box.get("X") or 0), float(box.get("Y") or 0))
            else:
                best = float(node.get("CapsuleRadius") or 0)
        for v in node.values():
            best = max(best, volume_radius(v))
    elif isinstance(node, list):
        for v in node:
            best = max(best, volume_radius(v))
    return best


def main() -> int:
    db = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)
    rows = []
    for guid, name, data in db.execute("select guid, name, data from record where schema = 'CrowdControlRecordBase' order by guid"):
        d = json.loads(gzip.decompress(data))
        disable = d["AbilitiesToDisableId"]["Guid"]
        stops = "All" if disable == DISABLE_ALL else "Magic" if disable == DISABLE_SILENCE else "Physical" if disable == DISABLE_DISARM else "None"
        try:
            snare = float(d["SnareStrength"] or 0)
        except ValueError:
            snare = 0.0
        anim = ANIMATIONS[d["AnimationRequest"].split("::")[1]]
        push = PUSH_CM.get(name, 400.0) if d["AppliedStatus"] & 32 else 0.0
        rows.append((guid, d["AppliedStatus"], anim, snare, d["Priority"], stops, name, push))
    fragile = []
    hit_mods = []
    for guid, name, extra in db.execute("select guid, name, extra from ab_effect order by guid"):
        x = json.loads(extra)
        ids = x.get("FragilityDefinitionsIds") or []
        shares = [0.0 if i == BREAK_INSTANT else BREAK_SHARE[i] for i in ids if i == BREAK_INSTANT or i in BREAK_SHARE]
        holds = 0
        if BREAK_IF_HOLDS in ids:
            text = json.loads(gzip.decompress(db.execute("select data from record where guid = ?", (BREAK_IF_HOLDS,)).fetchone()[0]))["ShouldBreakEffectImmediately"]
            holds = int(re.findall(r"\$#\d+:(\d+)\$", text)[0])
        with_cast = BREAK_WITH_CAST in ids
        if shares or holds or with_cast:
            fragile.append((guid, min(shares) if shares else -1.0, holds, with_cast, name))
        if name in HIT_MODS and x.get("HitModsIds"):
            hit_mods.append((guid, HIT_MODS[name][0], HIT_MODS[name][1], name))
    text = json.loads(gzip.decompress(db.execute("select data from record where guid = ?", (BREAK_INSTANT,)).fetchone()[0]))["ShouldBreakEffectImmediately"]
    exempt = sorted({int(g) for g in re.findall(r"\$#3672506661632452365:(\d+)\$", text)})
    modify = []
    for hit, name, extra in db.execute("select guid, name, extra from ab_hit order by guid"):
        for record in json.loads(extra).get("ModifyEffectsIds") or []:
            if record not in MODIFY_KINDS:
                continue
            d = json.loads(gzip.decompress(db.execute("select data from record where guid = ?", (record,)).fetchone()[0]))
            target = re.findall(r"\$#\d+:(\d+)\$", " ".join(d["TargetEffect"]))
            if not target:   # GetEffectRecordByName("...")
                by_name = re.findall(r'"([^"]+)"', " ".join(d["TargetEffect"]))
                target = [db.execute("select guid from ab_effect where name = ?", (by_name[0],)).fetchone()[0]]
            kind, delta, talent, pet_too = MODIFY_KINDS[record]
            modify.append((hit, int(target[0]), kind, delta, talent, pet_too, name))
    radii = []
    for guid, in db.execute("select distinct targeting_guid from ab_hit_recursive order by targeting_guid"):
        row = db.execute("select data from record where guid = ? and schema = 'AbilityTargetingRecordBase'", (guid,)).fetchone()
        radius = volume_radius(json.loads(gzip.decompress(row[0]))) if row else 0.0
        if radius > 0:
            radii.append((guid, radius))
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("// SPDX-License-Identifier: AGPL-3.0-only\n")
        f.write("// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).\n")
        f.write("// Generated by tools/worldd/gen_crowd_control.py from content.db.  Do not edit.\n")
        f.write("#pragma once\n\n#include <algorithm>\n#include <cstdint>\n#include <iterator>\n\nnamespace aoc::worldd::combat {\n\n")
        f.write("/// What a crowd control stops (AppliedStatus): its bits are the record's.\n")
        f.write("inline constexpr uint8_t kCrowdStopsMoving = 1, kCrowdStopsActing = 4, kCrowdSlows = 16;\n")
        f.write("enum class CrowdStops : uint8_t { None, All, Magic, Physical };\n")
        f.write("inline constexpr uint8_t kCrowdAnimationNone = 255;   // ECrowdControlAnimationRequest::None\n")
        f.write("struct CrowdControlRow {\n    int64_t    guid;\n    uint8_t    status;\n    uint8_t    animation;   // ECrowdControlAnimationRequest\n")
        f.write("    double     snare;       // the share of its speed the holder loses\n    int        priority;\n    CrowdStops stops;\n    double     push_cm;     // how far it throws the holder [modelled: the records' forces need a mass no table holds]\n};\n")
        f.write("inline constexpr CrowdControlRow kCrowdControls[] = {\n")
        for guid, status, anim, snare, prio, stops, name, push in rows:
            f.write(f"    {{{guid}ll, {status}, {anim}, {snare!r}, {prio}, CrowdStops::{stops}, {push!r}}},   // {name}\n")
        f.write("};\n")
        f.write("inline const CrowdControlRow* crowd_control(int64_t guid) {\n")
        f.write("    const auto it = std::lower_bound(std::begin(kCrowdControls), std::end(kCrowdControls), guid, [](const CrowdControlRow& r, int64_t g) { return r.guid < g; });\n")
        f.write("    return it != std::end(kCrowdControls) && it->guid == guid ? it : nullptr;\n}\n\n")
        f.write("/// The effects that break.  `share`: damage taken worth that share of the instigator's maximum Health ends it (0: any damage; < 0: damage does not).  `holds`: holding that effect ends it.\n")
        f.write("/// `with_cast`: it lasts while its instigator's pet plays the ability that gave it.\n")
        f.write("struct FragileRow {\n    int64_t effect;\n    double  share;\n    int64_t holds;\n    bool    with_cast;\n};\n")
        f.write("inline constexpr FragileRow kFragileEffects[] = {\n")
        for guid, share, holds, with_cast, name in fragile:
            f.write(f"    {{{guid}ll, {share!r}, {holds}ll, {'true' if with_cast else 'false'}}},   // {name}\n")
        f.write("};\n")
        f.write("inline const FragileRow* fragile_effect(int64_t guid) {\n")
        f.write("    const auto it = std::lower_bound(std::begin(kFragileEffects), std::end(kFragileEffects), guid, [](const FragileRow& r, int64_t g) { return r.effect < g; });\n")
        f.write("    return it != std::end(kFragileEffects) && it->effect == guid ? it : nullptr;\n}\n\n")
        f.write("/// The hits that do not break an effect that any damage breaks (InstantBreak's own list).\n")
        f.write("inline constexpr int64_t kInstantBreakExempt[] = {" + ", ".join(f"{g}ll" for g in exempt) + "};\n\n")
        f.write("/// What a hit changes in the effects its target holds (ModifyEffectsIds).\n")
        f.write("enum class ModifyKind : uint8_t { Delta, Elapsed, AtLeastOne };\n")
        f.write("struct ModifyRow {\n    int64_t    hit;\n    int64_t    effect;\n    ModifyKind kind;\n    double     seconds;\n    int64_t    talent;    // it counts only with this talent (0: always)\n    bool       pet_too;   // the effect may be the instigator's or its pet's\n};\n")
        f.write("inline constexpr ModifyRow kModifyEffects[] = {\n")
        for hit, target, kind, delta, talent, pet_too, name in modify:
            f.write(f"    {{{hit}ll, {target}ll, ModifyKind::{kind}, {delta!r}, {talent}ll, {'true' if pet_too else 'false'}}},   // {name}\n")
        f.write("};\n\n")
        f.write("/// What the effects a creature holds do to the damage of a hit (HitModsIds).\n")
        f.write("enum class HitModKind : uint8_t { OutgoingNotAtInstigator, IncomingFromEffects, IncomingFromInstigator };\n")
        f.write("struct HitModRow {\n    int64_t    effect;\n    double     mult;\n    HitModKind kind;\n};\n")
        f.write("inline constexpr HitModRow kHitMods[] = {\n")
        for guid, mult, kind, name in hit_mods:
            f.write(f"    {{{guid}ll, {mult!r}, HitModKind::{kind}}},   // {name}\n")
        f.write("};\n\n")
        f.write("/// How far the targeting record of a hit that fires other hits reaches, for those that list a volume.\n")
        f.write("struct TargetingRadius {\n    int64_t targeting;\n    double  radius_cm;\n};\n")
        f.write("inline constexpr TargetingRadius kTargetingRadii[] = {\n")
        for guid, radius in radii:
            f.write(f"    {{{guid}ll, {radius!r}}},\n")
        f.write("};\n")
        f.write("inline double targeting_radius(int64_t guid) {\n    for (const TargetingRadius& r : kTargetingRadii) if (r.targeting == guid) return r.radius_cm;\n    return 0.0;\n}\n\n")
        f.write("inline constexpr int64_t kStatDamageMult = 6064630854084526095ll, kStatHealingMult = 6064630634099769360ll;\n\n")
        f.write("}  // namespace aoc::worldd::combat\n")
    print(f"{OUT}: {len(rows)} crowd controls, {len(fragile)} fragile effects, {len(modify)} modifying hits, {len(hit_mods)} hit mods, {len(radii)} targeting radii")
    return 0


if __name__ == "__main__":
    sys.exit(main())
