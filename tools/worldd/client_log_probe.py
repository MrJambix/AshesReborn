#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
"""What the game client complained about in a session: reads the JSON log it writes on this PC, offline.

The client is not attached to and its memory is not read (it ships with EasyAntiCheat): this reads only the file the client
wrote.  Messages are grouped with their numbers and names blanked, so one problem hit ten thousand times is one line, and
the things that differ (the asset, the record) are kept as examples.

  client_log_probe.py [--log FILE] [--since ISO] [--top N] [--category TEXT] [--out FILE.md]

The log defaults to %LOCALAPPDATA%/AOC/Saved/Logs/AOC.log, the latest session.
"""
import argparse
import collections
import json
import os
import re
import sys

NOISE = ("LogStreamlineAPI", "LogConsoleManager", "LogShaderLibrary", "LogPBIKSolver", "LogChaosCloth", "LogD3D12RHI", "LogRHI")
GUID = re.compile(r"\b\d{10,}\b")
PATH = re.compile(r"(/(?:Game|Script|Engine)/[^\s'\",)\]]+)")
QUOTED = re.compile(r"'[^']*'|\"[^\"]*\"")
NUMBER = re.compile(r"-?\b\d+(?:\.\d+)?\b")


def shape(message):
    """The message with what changes between hits blanked: what groups it."""
    s = PATH.sub("<path>", message)
    s = GUID.sub("<id>", s)
    s = QUOTED.sub("<name>", s)
    s = NUMBER.sub("#", s)
    return re.sub(r"\s+", " ", s).strip()[:170]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    default = os.path.join(os.environ.get("LOCALAPPDATA", ""), "AOC", "Saved", "Logs", "AOC.log")
    ap.add_argument("--log", default=default)
    ap.add_argument("--since", default="", help="only entries at or after this time (2026-11-01T13:30)")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--category", default="", help="only categories holding this text")
    ap.add_argument("--all", action="store_true", help="keep the engine's own noise (shaders, upscaler, console variables)")
    ap.add_argument("--out", default="")
    a = ap.parse_args()

    groups = collections.defaultdict(lambda: {"count": 0, "examples": [], "assets": collections.Counter(), "first": ""})
    cats = collections.Counter()
    total = bad = 0
    # a message may run over several lines (an expression's text): only whole JSON lines are entries
    with open(a.log, encoding="utf-8-sig", errors="replace") as f:
        for line in f:
            if not line.startswith('{"timestamp"'):
                continue
            try:
                e = json.loads(line)
            except ValueError:
                bad += 1
                continue
            total += 1
            sev = e.get("severity", "")
            if sev not in ("Warning", "Error", "Fatal"):
                continue
            cat = e.get("category", "")
            if a.since and e.get("timestamp", "") < a.since:
                continue
            if a.category and a.category.lower() not in cat.lower():
                continue
            if not a.all and cat in NOISE:
                continue
            msg = e.get("message", "")
            cats[(cat, sev)] += 1
            g = groups[(sev, cat, shape(msg))]
            g["count"] += 1
            if not g["first"]:
                g["first"] = e.get("timestamp", "")
            if len(g["examples"]) < 2:
                g["examples"].append(msg[:300])
            for p in PATH.findall(msg):
                g["assets"][p.split("/")[-1].split(".")[0]] += 1

    out = [f"# Client log: {a.log}", "", f"{total} entries, {sum(cats.values())} warnings and errors kept.", "", "## By category", ""]
    for (cat, sev), n in cats.most_common(30):
        out.append(f"- {n} {sev} {cat}")
    out += ["", "## Problems, errors first", ""]
    ordered = sorted(groups.items(), key=lambda kv: (kv[0][0] != "Error" and kv[0][0] != "Fatal", -kv[1]["count"]))
    for (sev, cat, sh), g in ordered[: a.top]:
        out.append(f"### {g['count']}x {sev} {cat}")
        out.append(f"`{sh}`")
        out.append(f"example: {g['examples'][0]}")
        if g["assets"]:
            out.append("assets: " + ", ".join(f"{k} ({v})" for k, v in g["assets"].most_common(8)))
        out.append("")
    text = "\n".join(out)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"{a.out}: {len(groups)} kinds of problem")
    else:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
