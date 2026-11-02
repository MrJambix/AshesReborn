#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
"""What the game's own data holds against what this server reads of it.  Offline: content.db and the sources, nothing else.

  client_capability_report.py [--out FILE.md]    run from the repository root

Part 1: every kind of record in the client's design database, how many it holds, and whether the server's compiled tables or
sources name that kind at all.  Part 2: every kind the ability tables compile, how many of its records made it and what is left
out.  Part 3: every player ability by class: its hits, zones, projectiles and visual effects, whether a retail recording covers
it, and the gaps the replay test lists for it (tests/ability_npc_known.inc).

A kind no source names is not necessarily missing: the client reads the same database itself, so much of it (dialogue text, UI
settings) needs nothing from the server.  This says what to look at, not what is broken.
"""
import argparse
import collections
import gzip
import json
import os
import re
import sqlite3
import sys

DB = "dist/content/content.db"
SOURCES = ["server/worldd", "services", "libs/aocnet", "tools/npcabilities", "tools/mapc", "tools/worldd", "tools/capture"]
SKIP_DIRS = {"bin", "obj", "generated", "gen"}
EXTENSIONS = (".cpp", ".h", ".inc", ".go", ".cs", ".py")
COMPILED = {"AoCAbilityRecordBase": "ab_ability", "EffectRecordBase": "ab_effect", "AbilityHitRecordBase": "ab_hit", "ProjectileRecordBase": "ab_projectile",
            "ReactionRecordBase": "ab_reaction", "AbilityTargetingRecordBase": "ab_targeting_def"}
CLASSES = ["Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"]


def source_text():
    parts = []
    for root in SOURCES:
        for path, dirs, files in os.walk(root):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for name in files:
                # (a generated list of every kind, contentc's exe_gen.go, names them all and says nothing)
                if name.endswith(EXTENSIONS) and not name.startswith("capture_") and not name.endswith("_gen.go") and not name.endswith(".pb.go"):
                    try:
                        with open(os.path.join(path, name), encoding="utf-8", errors="replace") as f:
                            parts.append(f.read())
                    except OSError:
                        pass
    return "\n".join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    db = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)
    db.row_factory = sqlite3.Row
    text = source_text()
    out = ["# Client data against the server", ""]

    # ---- part 1
    rows = db.execute("select schema, count(*) n from record group by 1 order by n desc").fetchall()
    read = [r for r in rows if r["schema"] in text or r["schema"].replace("RecordBase", "") in text]
    unread = [r for r in rows if r not in read]
    out += [f"## 1. Kinds of record: {len(rows)}, of which the server's sources name {len(read)}", ""]
    out += ["Not named by any server source (most records first):", ""]
    for r in unread:
        sample = [x[0] for x in db.execute("select name from record where schema = ? limit 3", (r["schema"],))]
        out.append(f"- {r['n']} {r['schema']}: {', '.join(sample)}")
    out.append("")

    # ---- part 2
    out += ["## 2. Records the ability tables leave out", ""]
    for schema, table in COMPILED.items():
        have = {x[0] for x in db.execute(f"select guid from {table}")}
        records = db.execute("select guid, name, dev_only from record where schema = ?", (schema,)).fetchall()
        missing = [r for r in records if r["guid"] not in have]
        shipped = [r for r in missing if not r["dev_only"]]
        out.append(f"- {schema}: {len(records)} in the client, {len(have)} compiled, {len(missing)} left out ({len(shipped)} not marked dev-only). Left out, for example: {', '.join(r['name'] for r in shipped[:12])}")
    out.append("")

    # ---- part 3
    with open("server/worldd/tests/ability_npc_known.inc", encoding="utf-8") as f:
        known_text = f.read()
    gaps = collections.defaultdict(collections.Counter)
    for name, _case, what, reason in re.findall(r'\{"([^"]+)",\s*(\d+),\s*"([^"]*)",\s*(R_[A-Z]+)\}', known_text):
        gaps[name][reason] += 1
    with open("server/worldd/tests/capture_casts.h", encoding="utf-8", errors="replace") as f:
        casts = f.read()
    recorded = collections.Counter(re.findall(r'^\s*// ([A-Z][A-Za-z0-9_]+): S\d', casts, flags=re.M))
    skipped = collections.Counter(re.findall(r'\{\d+, \d+, "([^"]+)", "', casts))
    abilities = db.execute("select distinct a.guid, a.name from ab_ability a join skill_rank r on r.ability_guid = a.guid order by a.name").fetchall()
    out += [f"## 3. Player abilities: {len(abilities)}", "", "Recorded: casts in retail recordings the replay test compares against. Unrecorded means nothing has ever checked it against the real game.", ""]
    totals = collections.Counter()
    for cls in CLASSES + ["Other"]:
        rows3 = []
        for ab in abilities:
            prefix = ab["name"].split("_")[0]
            if (prefix if prefix in CLASSES else "Other") != cls:
                continue
            guid = ab["guid"]
            hits = db.execute("select count(distinct hit_guid) from ab_ability_hit where ability_guid = ?", (guid,)).fetchone()[0]
            zones = db.execute("select count(distinct z.zone_guid) from ab_ability_hit x join ab_hit_zone z on z.hit_guid = x.hit_guid where x.ability_guid = ?", (guid,)).fetchone()[0]
            shots = db.execute("select count(distinct source || key) from ab_ability_hit where ability_guid = ? and source = 'projectile'", (guid,)).fetchone()[0]
            data = db.execute("select data from record where guid = ? and schema = 'AoCAbilityRecordBase'", (guid,)).fetchone()
            fx = len(json.loads(gzip.decompress(data[0])).get("AbilityFXIds") or []) if data else 0
            seen = recorded[ab["name"]] + skipped[ab["name"]]
            gap = ", ".join(f"{k[2:].lower()} {v}" for k, v in gaps[ab["name"]].most_common())
            rows3.append((ab["name"], hits, zones, shots, fx, seen, gap))
            totals["recorded" if seen else "unrecorded"] += 1
            totals["with zones"] += 1 if zones else 0
        if not rows3:
            continue
        out += [f"### {cls}: {len(rows3)}", "", "| Ability | Hits | Zones | Shots | FX | Recorded | Gaps listed |", "|---|---|---|---|---|---|---|"]
        for name, hits, zones, shots, fx, seen, gap in rows3:
            out.append(f"| {name} | {hits} | {zones} | {shots} | {fx} | {seen or 'no'} | {gap} |")
        out.append("")
    out.insert(2, f"Player abilities recorded in retail: {totals['recorded']}; never recorded: {totals['unrecorded']}; with lingering zones: {totals['with zones']}.")
    out.insert(3, "")

    report = "\n".join(out)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(report)
        print(f"{a.out}: {len(report)} characters; {out[2]}")
    else:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
