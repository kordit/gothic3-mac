"""tools/wine_polish.py GAME_DIR PROFILE_DIR: Polish speech and subtitles for the original under Wine.

The same changes the port carries (docs/PROGRESS.md, session 7, Polish): Speech_Polish.pak from the
Polish Steam depot (original-pl/), Data/Speech_Polish in Ini/mountlist.ini, [Language] Text/Audio=Polish
and Polish in the subtitle list in Ini/ge3.ini, VoiceLanguage/SubtitleLanguage=Polish in the player's
UserOptions.ini (and __UserOptions.ini, which tools/wine_run.sh restores after a crash). The Polish
texts are already in CP 1.75's Strings.pak. Idempotent; the ini files keep their CRLF and Latin-1.
"""
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def edit(path, fn):
    data = path.read_bytes().decode("latin-1")
    new = fn(data)
    if new != data:
        path.write_bytes(new.encode("latin-1"))
        print("edited", path)


def ge3(text):
    text = re.sub(r"(?m)^Text=English\r$", "Text=Polish\r", text)
    text = re.sub(r"(?m)^Audio=English\r$", "Audio=Polish\r", text)
    if "Subtitle.Entry_5=Polish" not in text:
        text = re.sub(r"(?m)^Subtitle\.SupportedCount=5\r$", "Subtitle.SupportedCount=6\r", text)
        text = re.sub(r"(?m)^(Subtitle\.Entry_4=[^\r\n]*\r\n)", r"\1Subtitle.Entry_5=Polish\r\n", text)
    return text


def window(text):
    # Fullscreen=true crashes most starts under Wine in wow64cpu while wined3d creates the device
    # (wine-gl*, wine-pl*: 6 of 7); windowed passed 2 of 4 at once (wine-win*). F2 toggles in game.
    return re.sub(r"(?m)^Fullscreen=true\r$", "Fullscreen=false\r", text)


def mountlist(text):
    line = "Data/Speech_Polish=Data/Speech_Polish"
    if line in text:
        return text
    return text.rstrip("\r\n") + "\r\n" + line


def options(text):
    text = re.sub(r"(?m)^VoiceLanguage=\w+", "VoiceLanguage=Polish", text)
    return re.sub(r"(?m)^SubtitleLanguage=\w+", "SubtitleLanguage=Polish", text)


def main():
    game, prof = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    pak = game / "Data" / "Speech_Polish.pak"
    if not pak.exists():
        subprocess.run(["cp", "-c", str(ROOT / "original-pl/Data/Speech_Polish.pak"), str(pak)], check=True)
        print("copied", pak)
    edit(game / "Ini" / "ge3.ini", ge3)
    edit(game / "Ini" / "mountlist.ini", mountlist)
    edit(game / "Ini" / "ge3.ini", window)
    for name in ("UserOptions.ini", "__UserOptions.ini"):
        if (prof / name).exists():
            edit(prof / name, options)
    text = (game / "Ini" / "ge3.ini").read_bytes().decode("latin-1")
    assert "Text=Polish" in text and "Audio=Polish" in text and "Subtitle.Entry_5=Polish" in text
    print("Polish: ok")


if __name__ == "__main__":
    main()
