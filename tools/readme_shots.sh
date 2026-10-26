#!/bin/sh
# tools/readme_shots.sh <app> <scratch dir>: the README's launcher screenshots (docs/screens/*.jpg) without a
# window and without anyone's data. The launcher runs from a copy of <app> outside the checkout (so it is no
# developer run) with HOME set to an empty folder under <scratch dir>, and its DepotDownloader is a stand-in
# that prints a QR code of the repository's address, a download and an account without the game. The "ready"
# screen uses an APFS clone of original-cp175/, staged by launcher_tests. Nothing logs in to Steam.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
app=$1
scratch=$2
mkdir -p "$scratch"
scratch=$(cd "$scratch" && pwd)
home="$scratch/home"
copy="$scratch/Gothic3Recomp.app"
shots="$root/docs/screens"
rm -rf "$home" "$copy"
mkdir -p "$home" "$shots"
ditto "$app" "$copy"

"$root/.venv/bin/python" - "$scratch/qr.txt" <<'EOF'
import sys, qrcode
q = qrcode.QRCode(border=4, error_correction=qrcode.constants.ERROR_CORRECT_L)
q.add_data("https://github.com/kordit/gothic3-mac")
q.make(fit=True)
open(sys.argv[1], "w").write("".join("".join("██" if c else "  " for c in row) + "\n" for row in q.get_matrix()))
EOF

cat > "$copy/Contents/MacOS/DepotDownloader" <<EOF
#!/bin/sh
dir=""
while [ \$# -gt 0 ]; do [ "\$1" = -dir ] && dir="\$2"; shift; done
echo "Connecting to Steam3... Done!"
mode=\$(cat "\$HOME/mode")
if [ "\$mode" = qr ]; then
  echo "Logging in with QR code..."
  echo "Use the Steam Mobile App to sign in with this QR code:"
  cat "$scratch/qr.txt"
  exec sleep 600
fi
echo "Success! Next time you can login with -username gracz -remember-password instead of -qr."
echo "Got 4 licenses for account!"
echo "Got AppInfo for 39500"
if [ "\$mode" = notowned ]; then
  echo "App 39500 (Gothic 3 Classic) is not available from this account."
  exit 1
fi
echo "Downloading depot 39501"
for p in 03.10 09.85 17.42 26.03 34.61 41.27; do
  echo " \$p% \$dir/Data/Projects_compiled.pak"
  sleep 0.6
done
exec sleep 600
EOF
chmod +x "$copy/Contents/MacOS/DepotDownloader"

profile="$home/Library/Application Support/Gothic 3 Native"
shot() { # <name> <mode> <keys>
    mkdir -p "$profile/steam"
    echo "$2" > "$profile/steam/mode"
    HOME="$home" SDL_VIDEO_DRIVER=dummy RECOMP_GPU_BACKEND=none RECOMP_LAUNCHER=1 \
      RECOMP_LAUNCHER_SIZE=2560x1600 RECOMP_LAUNCHER_DENSITY=2 \
      RECOMP_LAUNCHER_KEYS="$3,dump:$scratch/$1.ppm,wait,quit" \
      "$copy/Contents/MacOS/Gothic3Recomp" > "$scratch/$1.log" 2>&1 || true
    sips -s format jpeg -s formatOptions 82 -Z 1600 "$scratch/$1.ppm" --out "$shots/$1.jpg" > /dev/null
    echo "$shots/$1.jpg"
}
waits() {
    i=0 out=""
    while [ $i -lt "$1" ]; do out="${out}wait,"; i=$((i + 1)); done
    printf '%s' "${out%,}"
}

shot 01-kod-qr qr "$(waits 20)"
rm -rf "$profile/steam"
shot 02-login-haslo qr "$(waits 12),id:4001,type:gracz_z_myrtany,enter,type:haslo1234,wait,wait"
rm -rf "$profile/steam"
shot 03-pobieranie download "$(waits 22)"
rm -rf "$profile/steam" "$profile/game"
shot 04-brak-gry notowned "$(waits 14)"
rm -rf "$profile/steam" "$profile/game"

# Ready to play: a clone of the install, prepared as the launcher prepares a download.
mkdir -p "$profile"
cp -cR "$root/original-cp175" "$profile/game"
rm -rf "$profile/game/steamapps" "$profile/game/.DepotDownloader"
"$root/.venv/bin/python" - "$root" "$scratch/stage.tsv" <<'EOF'
import sys, tomllib
root, out = sys.argv[1], sys.argv[2]
g = tomllib.load(open(root + "/game.toml", "rb"))
rows = []
for m in g["modules"]["aux"].values():
    rows.append("\t".join([m["name"], m["path"][len("stage/"):], m["from"], m["from_sha256"], m.get("prepare") or "-",
                           "%x" % m["base"], m["sha256"]]))
open(out, "w").write("\n".join(rows) + "\n")
EOF
"$root/build/recomp/launcher_tests" --stage "$profile/game" "$scratch/stage.tsv" > /dev/null
"$root/build/recomp/launcher_tests" --ini "$profile/game" "Ini/ge3.ini|Language|Text=Polish" > /dev/null
shot 05-gotowe-do-gry ready "$(waits 6)"
shot 06-zarzadzaj ready "$(waits 4),id:6,wait,id:4301,wait"
rm -rf "$home" "$copy"
