#!/usr/bin/env python3
"""Render docs/CHECKLIST.md from docs/milestones.json, or print what is next.

    tools/checklist.py           # rewrite docs/CHECKLIST.md
    tools/checklist.py --next 5  # the first open milestones, for a session start

milestones.json is the source of truth; never edit CHECKLIST.md by hand."""

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HEADER = """# Gothic 3 na Maca: checklista

Generowane z `docs/milestones.json` przez `tools/checklist.py`; nie edytuj ręcznie.
Port przez statyczną rekompilację, tą samą metodą co
[nfs-most-wanted-mac](https://github.com/elforeign/nfs-most-wanted-mac): kod x86
z plików gry tłumaczony na C i kompilowany na Apple Silicon, DirectX przez Metal.
Gra pochodzi z własnej kopii gracza (Steam, app 39500, build 1037023).

Gotowe: {done} z {total}. Dziennik sesji: [PROGRESS.md](PROGRESS.md), decyzje: [DECISIONS.md](DECISIONS.md).
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--next", type=int, metavar="N")
    args = parser.parse_args()
    data = json.loads((ROOT / "docs/milestones.json").read_text())
    items = data["milestones"]
    if args.next:
        for m in [m for m in items if not m["passes"]][:args.next]:
            print("  [ ] %s  %s\n        sprawdzenie: %s" % (m["id"], m["description"], m["verify"]))
        return
    out = [HEADER.format(done=sum(m["passes"] for m in items), total=len(items))]
    for key, title in data["stages"].items():
        stage = [m for m in items if m["stage"] == key]
        done = sum(m["passes"] for m in stage)
        out.append("## Etap %s. %s (%d/%d)\n" % (key, title, done, len(stage)))
        for m in stage:
            out.append("- [%s] **%s** %s  \n  sprawdzenie: %s" % ("x" if m["passes"] else " ", m["id"],
                                                                 m["description"], m["verify"]))
        out.append("")
    (ROOT / "docs/CHECKLIST.md").write_text("\n".join(out).rstrip() + "\n")


if __name__ == "__main__":
    main()
