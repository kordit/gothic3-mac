#!/bin/sh
# tools/g3fmodcheck.sh [--wait]: the game's Ogg sounds made by FMOD Ex the way the game made them (fmodex decoding
# with its own libvorbis) and from native/fmodogg.c's PCM, compared sample by sample, without the game (D-053,
# native/fmodcheck.c, docs/perf/fmod-ogg.md).
#
# 1. From the player's archives (their game folder's Data and the archives their profile lays over it, read only):
#    every .ogg of the sound archives (Sound.pak and what the mods lay over it), every 100th of the speech, every 10th
#    of the music, each distinct content once, unpacked to work/fmod-check/ogg/ and listed in list.txt.
# 2. build/recomp/pop_smoke with RECOMP_FMOD_CHECK: once the modules have attached the tool takes over instead of the
#    game's entry (no window, no world, no profile of the player's).
#
# Resumable: work/fmod-check/results.tsv has a line per file as soon as it is done, and a file is skipped once it is
# there; one that began and did not finish is left out on the next run. Progress: work/fmod-check/check.log. The tool
# runs under nohup; its PID is in work/fmod-check/check.pid. --wait stays until it ends.
cd "$(dirname "$0")/.."
out="$PWD/work/fmod-check"
player="$HOME/Library/Application Support/Gothic 3 Native"
bin=build/recomp/pop_smoke
mkdir -p "$out/ogg" "$out/profile" || exit 1
if [ -f "$out/check.pid" ] && kill -0 "$(cat "$out/check.pid")" 2>/dev/null; then
  echo "already running: PID $(cat "$out/check.pid"), log $out/check.log"
  exit 0
fi
[ -x "$bin" ] || { echo "no $bin: build first" >&2; exit 1; }

.venv/bin/python - "$player" "$out" <<'EOF' || exit 1
import hashlib, sys
from pathlib import Path
sys.path.insert(0, "tools")
import g3pak
player, out = Path(sys.argv[1]), Path(sys.argv[2])
names, seen, kept = [], set(), {}
for folder in (player / "game/Data", player / "Data"):
    for p in sorted(folder.glob("*")):
        if not p.is_file():
            continue
        kind = p.name.lower()
        every = 100 if kind.startswith("speech") else 10 if kind.startswith("music") else 1 if kind.startswith("sound") else 0
        if not every:
            continue
        try:
            pak = g3pak.Pak(p)
        except (SystemExit, Exception):
            continue
        keys = [k for k in sorted(pak.files) if k.lower().endswith(".ogg") and pak.files[k][3] != 0]
        for i, key in enumerate(keys):
            if i % every:
                continue
            data = pak.read(key)
            digest = hashlib.sha1(data).hexdigest()[:12]
            if digest in seen:
                continue
            seen.add(digest)
            name = "%s_%s" % (digest, Path(key).name)
            f = out / "ogg" / name
            if not f.exists():
                f.write_bytes(data)
            names.append(name)
            kept[kind] = kept.get(kind, 0) + 1
(out / "list.txt").write_text("".join(n + "\n" for n in names))
print("%d sounds in %s: %s" % (len(names), out / "list.txt", kept))
EOF

echo "$(date +%H:%M:%S) g3fmodcheck.sh: start ($(wc -l < "$out/list.txt" | tr -d ' ') sounds)" >> "$out/check.log"
printf 'wait 3600000\nquit\n' > "$out/check.smoke"
RECOMP_EXE="$player/game/Gothic3Final.exe" RECOMP_PROFILE_DIR="$out/profile" RECOMP_SMOKE_SECONDS=3600 \
  RECOMP_FMOD_CHECK="$out" nohup "$bin" --script "$out/check.smoke" > "$out/run.log" 2>&1 &
echo $! > "$out/check.pid"
echo "running: PID $!, log $out/check.log"
if [ "$1" = --wait ]; then
  while kill -0 "$(cat "$out/check.pid")" 2>/dev/null; do sleep 5; done
  tail -3 "$out/check.log"
fi
