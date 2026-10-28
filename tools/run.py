#!/usr/bin/env python3
"""Profiles for runs of the port that are not the player's game: tools/bench.sh and tools/smoke.py.

prepare_profile(mode) makes work/autostart-<mode>/profile (RECOMP_PROFILE_DIR): the kit reads ini/ge3.ini and
ini/logo.ini there before the game directory's, so NoMenu and the empty logo list never touch the player's files.
ge3.ini, the game options and the compiled shaders (Documents/gothic3/UserOptions.ini, Shader.Cache) are the
player's, copied from their profile, so a bench draws what the player's game draws. Until session 19 they came from
the developer profile build/recomp/profile, which had 1280x960 and "Medium" against the player's 1920x1200 "Custom".
NoMenu loads the world without handing the player the hero: it is for checking that the world loads and draws,
not for playing."""

import os
import shutil
import sys
from pathlib import Path

# G3_ROOT: the checkout that holds stage/ and build/ when this script runs from a worktree without them.
ROOT = Path(os.environ.get("G3_ROOT") or Path(__file__).resolve().parents[1])
APP = ROOT / "build/Gothic3Recomp.app/Contents/MacOS/Gothic3Recomp"
# The player's profile, the one the app uses outside a checkout (kit/runtime/layout.cpp).
PLAYER_PROFILE = Path.home() / "Library/Application Support/Gothic 3 Native"
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
    if profile is None:
        profile = ROOT / "work" / ("autostart-" + mode) / "profile"
    profile = Path(profile)
    (profile / "ini").mkdir(parents=True, exist_ok=True)
    source = PLAYER_PROFILE / "ini/ge3.ini"
    if not source.exists():
        source = ROOT / "stage/Ini/ge3.ini"
    ge3 = source.read_bytes().decode("latin-1")
    ge3 = set_ini(ge3, "NoMenu", "true" if mode == "world" else "false")
    (profile / "ini/ge3.ini").write_bytes(ge3.encode("latin-1"))
    (profile / "ini/logo.ini").write_bytes(b"")
    # The options and the compiled shaders: without Shader.Cache the game compiles every shader again
    # through d3dx9_40 before the first frame, minutes of start that the player's profile does not pay.
    # Both are copied on every start, so a run always has what the player's game has now: a run stopped by a
    # signal can leave only the game's __UserOptions.ini behind (its save renames that over UserOptions.ini), and
    # without the options the game starts on its high defaults, another shader set and the long tree AO.
    for name in ("UserOptions.ini", "Shader.Cache"):
        mine = PLAYER_PROFILE / "Documents/gothic3" / name
        copy = profile / "Documents/gothic3" / name
        if mine.exists():
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
