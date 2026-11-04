#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
"""Writes server/worldd/quest/commission_table.h from dist/content/content.db.

The commission boards: contentc compiles the commissions as quests (flavor "Commission"), but not what a board needs of the raw
records: each CommissionBoardRecordBase's filter, size and refresh, and each commission's CommissionData (its type, tags and
biome).  A board offers the Generic commissions its CommissionTypeFilter matches (a tag matches its parents, as gameplay tags
do); PlayerSponsored ones are the mayors'.  Run from the repository root; `--out FILE` writes elsewhere.
"""
import gzip
import json
import re
import sqlite3
import sys

DB = "dist/content/content.db"
OUT = "server/worldd/quest/commission_table.h"
# EBiomeType (the usmap): a commission's Biome.<Name> tag as the zone's biome number
BIOMES = {"AlpineForest": 1, "AncientForest": 2, "ArcticTundra": 3, "Asian": 4, "Badlands": 5, "Desert": 6, "Fjords": 7, "Glacier": 8, "Grasslands": 9,
          "Iceland": 10, "Jungle": 11, "MagicalForest": 12, "RedwoodForest": 13, "Riverlands": 14, "Swamp": 15, "Taiga": 16, "TabletopMountains": 17,
          "Tropics": 18, "Volcano": 19, "UnderRealm": 20, "Highlands": 21}
# The board record of each board Blueprint whose CommissionBoardId names no CommissionBoardRecordBase (mapc bp; modelled: the record its name and place
# point to).  The others keep their own: NodeDefault and NodeMayoral Adventure, NodeHunting Hunting.
BLUEPRINT_BOARDS = [
    ("CommissionBoard_NodeDefault_C", "CommissionBoard_Adventure"),
    ("CommissionBoard_NodeMayoral_C", "CommissionBoard_Adventure"),
    ("CommissionBoard_NodeHunting_C", "CommissionBoard_Hunting"),
    ("CommissionBoard_NodeArtisan_C", "CommissionBoard_Artisan"),
    ("CommissionBoard_NodeArtisan_Dunir_C", "CommissionBoard_Artisan"),
    ("CommissionBoard_NodeAdventure_Dunir_C", "CommissionBoard_Adventure"),
    ("Commission_Board_Lionhold_Temp_C", "CommissionBoard_StartingArea_Briarmoor"),
    ("Commission_Board_SamiasHope_Temp_C", "CommissionBoard_StartingArea_AvensEnd"),
    ("Commission_Board_Sandsquall_Desert_C", "CommissionBoard_Generic"),
    ("BP_CommissionBulletinBoard_C", "CommissionBoard_Generic"),
]


def cpp_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def tag_matches(have, want):
    return have == want or have.startswith(want + ".")


def main():
    out_path = OUT
    if "--out" in sys.argv:
        out_path = sys.argv[sys.argv.index("--out") + 1]
    db = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)
    raw = {}
    for guid, data in db.execute("select guid, data from record where schema in ('CommissionBoardRecordBase', 'NarrativeQuestV2')"):
        raw[guid] = data

    def record(guid):
        d = raw.get(guid)
        return json.loads(gzip.decompress(d)) if d else {}

    boards = []
    for guid, name in db.execute("select guid, name from record where schema = 'CommissionBoardRecordBase' order by guid"):
        r = record(guid)
        f = r.get("CommissionTypeFilter") or {}
        tags = [t["TagName"] for t in f.get("TagDictionary") or []]
        desc = f.get("AutoDescription") or ""
        # the filters are ALL( ANY(...) ) or ALL( ALL(...) ): one inner group (anything else is refused, so a new form is seen)
        m = re.fullmatch(r"\s*ALL\(\s*(ANY|ALL)\((.*)\)\s*\)\s*", desc)
        if not m:
            sys.exit(f"{name}: a filter this tool does not read: {desc!r}")
        boards.append({"guid": guid, "name": name, "mode": m.group(1), "tags": tags, "per": int(r.get("CommissionsPerBoard") or 8),
                       "refresh": int(r.get("RefreshTimeMinutes") or 60)})

    commissions = []
    for guid, name, level, disabled in db.execute("select guid, name, level, disabled from quest where flavor = 'Commission' order by guid"):
        if disabled or name.endswith("_TEST") or "Test" in name.split("_"):
            continue
        cd = record(guid).get("CommissionData") or {}
        if cd.get("CommissionType") != "ENarrativeCommissionType::Generic":
            continue
        tags = [t["TagName"] if isinstance(t, dict) else t for t in cd.get("CommissionGameplayTags") or []]
        biome = 0
        for t in tags:
            if t.startswith("Biome."):
                biome = BIOMES.get(t.split(".")[1], 0) or biome
        commissions.append({"guid": guid, "name": name, "level": int(level or 0), "biome": biome, "tags": tags})

    for b in boards:
        ok = []
        for i, c in enumerate(commissions):
            hits = [any(tag_matches(t, w) for t in c["tags"]) for w in b["tags"]]
            if (b["mode"] == "ANY" and any(hits)) or (b["mode"] == "ALL" and hits and all(hits)):
                ok.append(i)
        b["offers"] = ok

    by_name = {b["name"]: b["guid"] for b in boards}
    lines = []
    w = lines.append
    w("// SPDX-License-Identifier: AGPL-3.0-only")
    w("// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).")
    w("// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.")
    w("// ============================================================================")
    w("//  quest/commission_table.h: GENERATED by tools/worldd/gen_commissions.py from dist/content/content.db; do not edit.")
    w("//")
    w("//  The commission boards' records (CommissionBoardRecordBase: filter, CommissionsPerBoard, RefreshTimeMinutes), the Generic commissions (the")
    w("//  quests of flavor Commission whose CommissionData.CommissionType is Generic: the PlayerSponsored ones are the mayors') with their level and biome")
    w("//  (EBiomeType of their Biome.<Name> tag, 0 for none), each board's matching commissions, and the board record of each board Blueprint (modelled")
    w("//  where its CommissionBoardId names no record).")
    w("// ============================================================================")
    w("#pragma once")
    w("")
    w("#include <cstdint>")
    w("#include <span>")
    w("")
    w("namespace aoc::worldd::quest::commissions {")
    w("")
    w("struct Commission {")
    w("    int64_t     quest;   // the quest record")
    w("    int         level;")
    w("    int         biome;   // EBiomeType; 0: any")
    w("    const char* name;")
    w("};")
    w("")
    w("inline constexpr Commission kCommissions[] = {")
    for c in commissions:
        w(f"    {{{c['guid']}, {c['level']}, {c['biome']}, {cpp_str(c['name'])}}},")
    w("};")
    w("")
    for b in boards:
        w(f"inline constexpr uint16_t k{b['name']}[] = {{" + ", ".join(str(i) for i in b["offers"]) + ("" if b["offers"] else "0") + "};")
    w("")
    w("struct Board {")
    w("    int64_t                   record;    // CommissionBoardRecordBase")
    w("    const char*               name;")
    w("    int                       per_board; // CommissionsPerBoard")
    w("    int                       refresh_minutes;")
    w("    std::span<const uint16_t> offers;    // indices into kCommissions the filter matches")
    w("};")
    w("")
    w("inline constexpr Board kBoards[] = {")
    for b in boards:
        span = f"std::span<const uint16_t>{{k{b['name']}}}" if b["offers"] else "std::span<const uint16_t>{}"
        w(f"    {{{b['guid']}, {cpp_str(b['name'])}, {b['per']}, {b['refresh']}, {span}}},   // {b['mode']}({', '.join(b['tags'])}): {len(b['offers'])}")
    w("};")
    w("")
    w("/// The board record a board Blueprint (its class name) opens.")
    w("struct BlueprintBoard {")
    w("    const char* blueprint;")
    w("    int64_t     record;")
    w("};")
    w("")
    w("inline constexpr BlueprintBoard kBlueprintBoards[] = {")
    for bp, name in BLUEPRINT_BOARDS:
        w(f"    {{{cpp_str(bp)}, {by_name[name]}}},   // {name}")
    w("};")
    w("")
    w("}  // namespace aoc::worldd::quest::commissions")
    open(out_path, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print(f"{out_path}: {len(commissions)} commissions, {len(boards)} boards: " + ", ".join(f"{b['name']} {len(b['offers'])}" for b in boards))


if __name__ == "__main__":
    main()
