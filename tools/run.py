#!/usr/bin/env python3
"""Start the port with an autostart mode, without editing stage/ or the player's profile.

    tools/run.py                      # G3_AUTOSTART from the environment, else "menu"
    G3_AUTOSTART=world tools/run.py   # straight into the world (the engine's NoMenu), no logo videos
    tools/run.py --autostart newgame  # no logo videos, the script clicks New Game and Yes, no intro film

Every mode but "menu" runs on a profile of its own (work/autostart-<mode>/profile, RECOMP_PROFILE_DIR):
the kit reads ini/ge3.ini and ini/logo.ini there before the game directory's, so NoMenu and the empty
logo list never touch stage/Ini or build/recomp/profile. The game options and compiled shaders
(Documents/gothic3/UserOptions.ini, Shader.Cache) are copied from the player's profile the first
time, so the detail level is theirs and the start does not compile every shader again.
NoMenu loads the world without handing the player the hero: it is for checking that the world loads
and draws, not for playing. Extra RECOMP_* settings pass through the environment."""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

# G3_ROOT: the checkout that holds stage/ and build/ when this script runs from a worktree without them.
ROOT = Path(os.environ.get("G3_ROOT") or Path(__file__).resolve().parents[1])
APP = ROOT / "build/Gothic3Recomp.app/Contents/MacOS/Gothic3Recomp"
PLAYER_PROFILE = ROOT / "build/recomp/profile"
MODES = ("menu", "world", "newgame")
# New Game, then Yes on the Alternative Balancing question, in drawable pixels of the 1280x960 game
# image; the times are from the start with the logo videos skipped.
NEWGAME_CLICKS = "60:move:630:470,61:move:640:480,63:click:640:480,72:move:480:375,73:move:486:380,75:click:486:380"


def set_ini(text, key, value):
    """Sets `key=value` in an ini body (CRLF, Latin-1 as the game writes it), whatever it was."""
    lines = text.split("\r\n")
    for i, line in enumerate(lines):
        if line.split("=", 1)[0].strip() == key:
            lines[i] = "%s=%s" % (key, value)
            return "\r\n".join(lines)
    # A key the file lacks (Cache.SizeImage and the other Cache.* the engine reads in OnInitializeEngine) goes at
    # the end of [Engine.Setup], where those keys live.
    for i, line in enumerate(lines):
        if line.strip() == "[Engine.Setup]":
            j = i + 1
            while j < len(lines) and not lines[j].startswith("["):
                j += 1
            while j > i + 1 and not lines[j - 1].strip():
                j -= 1
            lines.insert(j, "%s=%s" % (key, value))
            return "\r\n".join(lines)
    raise SystemExit("ge3.ini has no %s and no [Engine.Setup]" % key)


def prepare_profile(mode, profile=None):
    """The profile directory a mode runs on, created or refreshed; None for the player's own.
    `profile` names another directory to prepare the same way (a test profile, e.g. work/mods-test)."""
    if mode == "menu" and profile is None:
        return None
    # G3_SHADER_SEED=install starts the profile's Shader.Cache from the one the install ships (about 4600
    # shaders) instead of the player's; such a profile gets a directory of its own. G3_SHADER_SEED=none starts
    # it with no Shader.Cache at all, so the game compiles every shader through d3dx9_40 (milestone 6.1).
    seed = os.environ.get("G3_SHADER_SEED", "player")
    if profile is None:
        profile = ROOT / "work" / ("autostart-" + mode + ("" if seed == "player" else "-" + seed)) / "profile"
    profile = Path(profile)
    (profile / "ini").mkdir(parents=True, exist_ok=True)
    source = PLAYER_PROFILE / "ini/ge3.ini"
    if not source.exists():
        source = ROOT / "stage/Ini/ge3.ini"
    ge3 = source.read_bytes().decode("latin-1")
    ge3 = set_ini(ge3, "NoMenu", "true" if mode == "world" else "false")
    # G3_INI_SET="Physics.Threads=0,Entity.ROI=3000.0": ge3.ini keys for this run only (bench A/B).
    for pair in filter(None, os.environ.get("G3_INI_SET", "").split(",")):
        key, value = pair.split("=", 1)
        ge3 = set_ini(ge3, key.strip(), value.strip())
    (profile / "ini/ge3.ini").write_bytes(ge3.encode("latin-1"))
    (profile / "ini/logo.ini").write_bytes(b"")
    # The options and the compiled shaders: without Shader.Cache the game compiles every shader again
    # through d3dx9_40 before the first frame, minutes of start that the player's profile does not pay.
    # The options are copied on every start: a run stopped by a signal can leave only the game's
    # __UserOptions.ini behind (its save renames that over UserOptions.ini), and without the options
    # the game starts on its high defaults, another shader set and the long tree AO.
    for name in ("UserOptions.ini", "Shader.Cache"):
        mine = PLAYER_PROFILE / "Documents/gothic3" / name
        if name == "Shader.Cache" and seed == "install":
            mine = ROOT / "stage/Shader.Cache"
        if name == "Shader.Cache" and seed == "none":
            (profile / "Documents/gothic3" / name).unlink(missing_ok=True)
            continue
        copy = profile / "Documents/gothic3" / name
        if (name == "UserOptions.ini" or not copy.exists()) and mine.exists():
            copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(mine, copy)
    return profile


def build_mod_view(profile):
    """The launcher's active-mods.json in `profile` as links in <profile>/Data (tools/g3mods.py).
    Without the file any view a previous start left is removed, so a profile is unmodded again."""
    import g3mods  # noqa: E402 - tools/ is on the path when run.py is

    record = g3mods.build(Path(profile), g3mods.game_data_dir())
    for line in record["errors"]:
        print("run: mods: %s" % line, file=sys.stderr)
    return record


def environment(mode, extra=None):
    env = dict(os.environ)
    profile = prepare_profile(mode)
    # The mods the launcher picked, as links in the profile the game is about to read.
    build_mod_view(profile if profile is not None else PLAYER_PROFILE)
    if profile is not None:
        env["RECOMP_PROFILE_DIR"] = str(profile)
        # The kit's on-disk MSL cache: every translated shader recorded once, compiled in the background
        # at the next start (what the launcher sets for a player).
        env.setdefault("RECOMP_D3D9_SHADER_WARM_DIR", str(profile / "shader-warm"))
    if mode == "newgame":
        env.setdefault("RECOMP_INPUT_SCRIPT", NEWGAME_CLICKS)
        env.setdefault("RECOMP_BINK_SKIP", "G3_Intro")
    if mode != "menu":
        env.setdefault("RECOMP_ASSUME_FOCUS", "1")
    env.update(extra or {})
    return env


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--autostart", choices=MODES, default=os.environ.get("G3_AUTOSTART", "menu"))
    args = ap.parse_args()
    if args.autostart not in MODES:
        sys.exit("G3_AUTOSTART must be one of %s" % ", ".join(MODES))
    os.execve(str(APP), [str(APP)], environment(args.autostart))


if __name__ == "__main__":
    main()
