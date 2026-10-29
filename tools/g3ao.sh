#!/bin/sh
# tools/g3ao.sh [--wait]: every SpeedTree's ambient occlusion computed once, outside the game, into
# ~/Library/Caches/Gothic3Recomp/ao/ (D-052, native/aobake.c, native/aocache.c, docs/perf/ao-drzew.md).
#
# 1. Every .spt of the player's archives (their game folder's Data and the archives their profile lays over it, read
#    only), each distinct content once, unpacked to work/ao-bake/spt/<sha1 prefix>_<name>.spt, listed in list.txt.
# 2. build/recomp/pop_smoke (the game build without a window) with RECOMP_AO_BAKE: once the modules have attached the
#    tool takes over instead of the game's entry, so there is no world, no profile of the player's and no save. It
#    goes through the list with the game's own code and writes each tree's three results as soon as they are done.
#
# Resumable: a finished tree has work/ao-bake/done/<name>.done and is skipped; one that began and did not finish (a
# crash) is left out on the next run and named in the log. Progress: work/ao-bake/bake.log. The tool runs under
# nohup; its PID is in work/ao-bake/bake.pid. --wait stays until it ends.
cd "$(dirname "$0")/.."
out="$PWD/work/ao-bake"
player="$HOME/Library/Application Support/Gothic 3 Native"
bin=build/recomp/pop_smoke
mkdir -p "$out/spt" "$out/done" "$out/profile" || exit 1
if [ -f "$out/bake.pid" ] && kill -0 "$(cat "$out/bake.pid")" 2>/dev/null; then
  echo "already running: PID $(cat "$out/bake.pid"), log $out/bake.log"
  exit 0
fi
[ -x "$bin" ] || { echo "no $bin: build first" >&2; exit 1; }

.venv/bin/python - "$player" "$out" <<'EOF' || exit 1
import hashlib, sys
from pathlib import Path
sys.path.insert(0, "tools")
import g3pak
player, out = Path(sys.argv[1]), Path(sys.argv[2])
names, seen = [], set()
for folder in (player / "game/Data", player / "Data"):
    for p in sorted(folder.glob("*")):
        if not p.is_file() or p.suffix.lower() not in (".pak", ".cpt") and not p.suffix.lower()[1:2] in ("p", "m"):
            continue
        try:
            pak = g3pak.Pak(p)
        except SystemExit:
            continue
        for key in sorted(pak.files):
            if not key.endswith(".spt") or pak.files[key][3] == 0:
                continue
            data = pak.read(key)
            digest = hashlib.sha1(data).hexdigest()[:12]
            if digest in seen:
                continue
            seen.add(digest)
            name = "%s_%s" % (digest, Path(key).name)
            f = out / "spt" / name
            if not f.exists():
                f.write_bytes(data)
            names.append(name)
(out / "list.txt").write_text("".join(n + "\n" for n in names))
print("%d trees in %s" % (len(names), out / "list.txt"))
EOF

echo "$(date +%H:%M:%S) g3ao.sh: start ($(wc -l < "$out/list.txt" | tr -d ' ') trees, $(ls "$out/done" | grep -c '\.done$') done before)" >> "$out/bake.log"
# pop_smoke wants a script; the tool ends the process long before this one would
printf 'wait 3600000\nquit\n' > "$out/bake.smoke"
RECOMP_EXE="$player/game/Gothic3Final.exe" RECOMP_PROFILE_DIR="$out/profile" RECOMP_SMOKE_SECONDS=3600 \
  RECOMP_AO_BAKE="$out" RECOMP_AO_BAKE_LOG="$out/bake.log" \
  nohup "$bin" --script "$out/bake.smoke" > "$out/run.log" 2>&1 &
echo $! > "$out/bake.pid"
echo "running: PID $!, log $out/bake.log"
if [ "$1" = --wait ]; then
  while kill -0 "$(cat "$out/bake.pid")" 2>/dev/null; do sleep 5; done
  tail -3 "$out/bake.log"
fi
