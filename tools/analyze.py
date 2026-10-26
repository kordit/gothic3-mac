#!/usr/bin/env python3
"""Export the translation inputs of every staged module with Ghidra's own analysis.

    tools/analyze.py --ghidra-home ~/tools/ghidra_12.1.3_PUBLIC [--only Engine.dll ...]

Adapted from nfs-most-wanted-mac's tools/analyze.py: each module in
stage/manifest.json is imported at its staged base, analysed with Ghidra's
default analyzers, given functions at the addresses in entries/<module>.txt
(tools/ghidra/AddEntries.java: code Ghidra left outside every function) and
exported with the kit's ExportProgram.java into analysis/<module>/
(functions.tsv, functions/*.asm, strings.tsv). A module whose export already
matches its staged SHA-256 and entry list is skipped. Logs go to
analysis/logs/<module>.log.

--stage and --output point it at another staged set, e.g. the script DLLs of
mods, read only for their listings (docs/mods/plan-poprawki.md, phase 2)."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / "kit"
GHIDRA_VERSION = "12.1.3"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ghidra-home", type=Path, default=os.environ.get("GHIDRA_HOME"))
    parser.add_argument("--java-home", type=Path, default=os.environ.get("JAVA_HOME"))
    parser.add_argument("--max-memory", default="6G", help="Ghidra's Java heap (default 6G)")
    parser.add_argument("--only", nargs="*", help="module names to export (default: all, smallest first)")
    parser.add_argument("--stage", type=Path, default=ROOT / "stage", help="folder with manifest.json (default stage/)")
    parser.add_argument("--output", type=Path, default=ROOT / "analysis", help="listings folder (default analysis/)")
    args = parser.parse_args()
    stage = args.stage.resolve()
    if not (KIT / "tools/ExportProgram.java").is_file():
        sys.exit("kit/tools/ExportProgram.java is missing")
    if not args.ghidra_home:
        sys.exit("Set --ghidra-home or GHIDRA_HOME to the extracted Ghidra %s directory" % GHIDRA_VERSION)
    ghidra = args.ghidra_home.expanduser().resolve()
    properties = ghidra / "Ghidra/application.properties"
    if not properties.is_file() or "application.version=%s\n" % GHIDRA_VERSION not in properties.read_text():
        sys.exit("Use Ghidra %s; set --ghidra-home to its extracted directory" % GHIDRA_VERSION)
    manifest = json.loads((stage / "manifest.json").read_text())
    modules = sorted(manifest, key=lambda m: m["size"])
    if args.only:
        modules = [m for m in modules if m["name"] in args.only]

    env = dict(os.environ, MAXMEM=args.max_memory)
    if args.java_home:
        env["JAVA_HOME"] = str(args.java_home.expanduser().resolve())
        env["PATH"] = str(Path(env["JAVA_HOME"]) / "bin") + os.pathsep + env.get("PATH", "")
    output = args.output.resolve()
    logs = output / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    for m in modules:
        listings = output / m["name"]
        inputs = listings / "inputs.json"
        extra = ROOT / "entries" / (m["name"] + ".txt")
        extra_sha = hashlib.sha256(extra.read_bytes()).hexdigest() if extra.is_file() else None
        recorded = json.loads(inputs.read_text()) if inputs.is_file() else {}
        if recorded.get("sha256") == m["sha256"] and recorded.get("entries_sha256") == extra_sha:
            print("%-18s up to date" % m["name"])
            continue
        project = output / "ghidra" / m["name"]
        if any(part.startswith(".") for part in project.parts):
            # Ghidra refuses a project path with a dot element (a worktree under .claude/);
            # the project is deleted after the export anyway.
            project = Path(tempfile.mkdtemp(prefix="g3ghidra-")) / m["name"]
        project.mkdir(parents=True, exist_ok=True)
        command = [
            str(ghidra / "support/analyzeHeadless"), str(project), "g3",
            "-import", str(stage / m.get("path", m["name"])), "-overwrite", "-deleteProject",
            "-scriptPath", "%s;%s" % (KIT / "tools", ROOT / "tools/ghidra"),
            "-postScript", "AddEntries.java", str(ROOT / "entries"),
            "-postScript", "ExportProgram.java", str(output),
        ]
        started = time.time()
        print("%-18s analysing (%#x bytes)..." % (m["name"], m["size"]), flush=True)
        with open(logs / (m["name"] + ".log"), "w") as log:
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        index = listings / "functions.tsv"
        if result.returncode or not index.is_file() or len(index.read_text().splitlines()) < 2:
            sys.exit("%s: Ghidra did not export a function index; see %s" % (m["name"], logs / (m["name"] + ".log")))
        functions = len(index.read_text().splitlines()) - 1
        inputs.write_text(json.dumps({
            "sha256": m["sha256"], "base": m["base"], "ghidra_version": GHIDRA_VERSION,
            "ghidra_analysis": "default analyzers", "functions": functions, "entries_sha256": extra_sha,
        }, indent=2) + "\n")
        print("%-18s %d functions in %.0f s" % (m["name"], functions, time.time() - started), flush=True)


if __name__ == "__main__":
    main()
