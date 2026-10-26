"""Build-tool platform gates and translator arguments, without invoking the translator."""

import importlib.util
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

import pytest

spec = importlib.util.spec_from_file_location("build", Path(__file__).parents[1] / "build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def test_mods_targets_match_the_game_fixture(tmp_path):
    spec = importlib.util.spec_from_file_location("test_runner", build.ROOT / "tools/test.py")
    test_runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(test_runner)

    assert test_runner.mods_targets(tmp_path) == ["pop_fixture", "mods_tests"]
    fixture = tmp_path / "mods/examples/luawalk/main.lua"
    fixture.parent.mkdir(parents=True)
    fixture.write_text("-- fixture\n")
    assert test_runner.mods_targets(tmp_path) == ["pop_fixture", "mods_tests", "present_events_tests"]


def test_android_templates_render(tmp_path):
    cfg = {"game": {"app_name": "StubRecomp", "bundle_id": "dev.recompkit.stub", "id": "stub"}}
    out = build.android_project(tmp_path, cfg, gen_dir=tmp_path / "gen")
    manifest = (out / "app/src/main/AndroidManifest.xml").read_text()
    assert 'package="dev.recompkit.stub"' in manifest
    gradle = (out / "app/build.gradle.kts").read_text()
    assert "StubRecomp" in gradle
    assert 'applicationId = "dev.recompkit.stub"' in gradle
    assert 'resValue("string", "game_id", "stub")' in gradle
    sdl_java = tmp_path / "gen/_deps/sdl3-src/android-project/app/src/main/java"
    assert sdl_java.resolve().as_posix() in gradle
    assert "externalNativeBuild" not in gradle
    android = "{http://schemas.android.com/apk/res/android}"
    root = ET.fromstring(manifest)
    feature = root.find("uses-feature")
    assert feature.get(android + "name") == "android.hardware.vulkan.version"
    assert int(feature.get(android + "version"), 16) == (1 << 22) | (1 << 12)
    assert feature.get(android + "required") == "true"
    app = root.find("application")
    assert app.get(android + "label") == "@string/app_name"
    assert app.get(android + "requestLegacyExternalStorage") == "false"
    assert app.find("activity").get(android + "screenOrientation") == "fullUser"


def test_android_core_assets_replace_stale_manifests_without_shipping_binaries(tmp_path):
    game = tmp_path / "game"
    core = game / "mods/core/display"
    core.mkdir(parents=True)
    for name in ("mod.toml", "palette.json", "display.c", "display.dylib", "display.so"):
        (core / name).write_text(name)
    out = tmp_path / "apk"
    stale = out / "app/src/main/assets/mods/core/stale/mod.toml"
    stale.parent.mkdir(parents=True)
    stale.write_text("stale")
    profile = out / "profile/save.dat"
    profile.parent.mkdir(parents=True)
    profile.write_text("keep")
    build.android_stage_core_assets(out, game)
    staged = out / "app/src/main/assets/mods/core"
    assert sorted(p.relative_to(staged).as_posix() for p in staged.rglob("*") if p.is_file()) == [
        "display/mod.toml", "display/palette.json"]
    assert profile.read_text() == "keep"
    (core / "mod.toml").write_text("new")
    build.android_stage_core_assets(out, game)
    assert (staged / "display/mod.toml").read_text() == "new"


def test_android_assets_carry_the_games_control_layouts(tmp_path):
    cfg = {"game": {"app_name": "StubRecomp", "bundle_id": "dev.recompkit.stub", "id": "stub"}}
    game_dir = tmp_path / "game"
    (game_dir / "layouts").mkdir(parents=True)
    (game_dir / "layouts/pad.json").write_text("{}\n")
    (game_dir / "layouts/pad.phone-portrait.json").write_text("{}\n")
    (game_dir / "layouts/readme.txt").write_text("not a layout\n")
    out = build.android_project(tmp_path, cfg, gen_dir=tmp_path / "gen")
    controls = out / "app/src/main/assets/controls"
    stale = controls / "gone.json"
    stale.parent.mkdir(parents=True, exist_ok=True)
    stale.write_text("{}\n")
    build.android_stage_layout_assets(out, game_dir)
    assert sorted(p.name for p in controls.iterdir()) == ["pad.json", "pad.phone-portrait.json"]


def test_android_assets_drop_controls_when_the_game_ships_none(tmp_path):
    cfg = {"game": {"app_name": "StubRecomp", "bundle_id": "dev.recompkit.stub", "id": "stub"}}
    out = build.android_project(tmp_path, cfg, gen_dir=tmp_path / "gen")
    controls = out / "app/src/main/assets/controls"
    controls.mkdir(parents=True)
    (controls / "pad.json").write_text("{}\n")
    build.android_stage_layout_assets(out, tmp_path / "game")
    assert not controls.exists()


@pytest.mark.parametrize("system", ["Darwin", "Linux", "Windows"])
def test_android_target_selects_the_ndk_preset(system):
    for extra, preset in [([], "android"), (["--stub"], "android-stub")]:
        args, _ = build.parse_args(["--target", "android"] + extra, system=system)
        assert build.preset_name(args.preset, args.config, stub=args.stub, target=args.target) == preset
    assert build.TARGETS["android"] == ["recomp_app"]


@pytest.mark.parametrize("devices, console, actions", [
    ("", True, []),
    ("phone\toffline\nlocked\tunauthorized\n", True, []),
    ("phone\tdevice\n", False, ["install", "shell"]),
    ("phone\tdevice\n", True, ["install", "shell", "logcat"]),
])
def test_android_install_requires_a_ready_device(tmp_path, monkeypatch, devices, console, actions):
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "List of devices attached\n" + devices)

    monkeypatch.setattr(build.shutil, "which", lambda name: "/sdk/adb")
    monkeypatch.setattr(build.subprocess, "run", run)
    apk = tmp_path / "app-debug.apk"
    build.android_install_and_launch(apk, "dev.recompkit.stub", console=console)
    assert calls[0] == ["/sdk/adb", "devices"]
    assert [command[3] for command in calls[1:]] == actions
    if actions:
        assert calls[1] == ["/sdk/adb", "-s", "phone", "install", "-r", str(apk)]
        assert calls[2] == ["/sdk/adb", "-s", "phone", "shell", "am", "start", "-n",
                            "dev.recompkit.stub/dev.recompkit.RecompActivity"]


def test_android_install_does_not_guess_between_devices(tmp_path, monkeypatch):
    monkeypatch.setattr(build.shutil, "which", lambda name: "/sdk/adb")
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "List of devices attached\nfirst\tdevice\nsecond\tdevice\n")

    monkeypatch.setattr(build.subprocess, "run", run)
    with pytest.raises(ValueError, match="Pass --device"):
        build.android_install_and_launch(tmp_path / "app-debug.apk", "dev.recompkit.stub")
    assert calls == [["/sdk/adb", "devices"]]


def test_android_push_stages_only_included_game_files(tmp_path, monkeypatch):
    source = tmp_path / "original/gog/app"
    files = {
        "Stub.exe": b"test executable", "Data/sprites.bin": b"sprites",
        "AUDIO/Music/track.mp3": b"music", "model.txt": b"model",
        "BINKS/intro.bik": b"video", "__support/helper.exe": b"installer",
        "sound.dll": b"dll", "Data/unused.dll": b"dll",
    }
    for name, data in files.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    cfg = {"game": {"executable": "Stub.exe", "bundle_id": "dev.recompkit.stub"},
           "developer_exe_path": source / "Stub.exe",
           "bundle": {"exclude": ["BINKS", "__support", "*.dll"]}}
    output = tmp_path / "build"
    staged = output / "android/game"
    staged.mkdir(parents=True)
    (staged / "stale.dll").write_bytes(b"old staging must not bypass exclusions")
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        if "push" in command:
            assert command == ["/sdk/adb", "-s", "tablet", "push", str(staged),
                               "/sdcard/Android/data/dev.recompkit.stub/files/"]
            assert sorted(p.relative_to(staged).as_posix() for p in staged.rglob("*") if p.is_file()) == [
                ".stamp", "AUDIO/Music/track.mp3", "Data/sprites.bin", "Stub.exe", "model.txt"]
            for name in ("Stub.exe", "Data/sprites.bin", "AUDIO/Music/track.mp3", "model.txt"):
                assert (staged / name).read_bytes() == files[name]
            import hashlib
            assert (staged / ".stamp").read_text().strip() == hashlib.sha256(files["Stub.exe"]).hexdigest()
        return subprocess.CompletedProcess(command, 0, "List of devices attached\ntablet\tdevice\n")

    monkeypatch.setattr(build.shutil, "which", lambda name: "/sdk/adb")
    monkeypatch.setattr(build.subprocess, "run", run)
    build.android_install_and_launch(output / "app-debug.apk", "dev.recompkit.stub",
                                     game_cfg=cfg, build_root=output)
    assert [command[3] for command in calls[1:]] == ["install", "shell", "push", "shell"]
    assert calls[2][4:] == ["mkdir", "-p", "/sdcard/Android/data/dev.recompkit.stub/files"]
    assert calls[-1][4:7] == ["am", "start", "-n"]


def test_android_push_without_device_fails(tmp_path, monkeypatch):
    monkeypatch.setattr(build.shutil, "which", lambda name: "/sdk/adb")
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "List of devices attached\n")

    monkeypatch.setattr(build.subprocess, "run", run)
    with pytest.raises(ValueError, match="No Android device attached.*--push-game"):
        build.android_install_and_launch(tmp_path / "app-debug.apk", "dev.recompkit.stub",
                                         game_cfg={}, build_root=tmp_path)
    assert calls == [["/sdk/adb", "devices"]]


@pytest.mark.parametrize("extra", [["--target", "app"], ["--target", "android", "--no-install"]])
def test_push_game_rejects_options_that_cannot_push(extra, capsys):
    with pytest.raises(SystemExit) as error:
        build.parse_args(extra + ["--push-game"])
    assert error.value.code == 2
    assert "--push-game requires --target android without --no-install" in capsys.readouterr().err


def test_app_target_allowed_on_linux():
    args, _ = build.parse_args(
        ["--target", "app", "--game-dir", str(build.ROOT / "games/stub")], system="Linux")
    assert args.preset == "linux"


def test_ios_target_still_needs_macos(capsys):
    with pytest.raises(SystemExit) as error:
        build.parse_args(
            ["--target", "ios", "--stub", "--game-dir", str(build.ROOT / "games/stub")], system="Linux")
    assert error.value.code == 2
    assert "The iOS packager runs on macOS" in capsys.readouterr().err


def test_allow_table_gaps_reaches_the_translator(tmp_path):
    jobs = build.translation_jobs(tmp_path, tmp_path / "build", allow_table_gaps="switch 004ab2af")
    args = jobs[0]["args"]
    assert args[args.index("--allow-table-gaps") + 1] == "switch 004ab2af"


def test_acceptances_reach_each_auxiliary_module(tmp_path):
    """A module's listing has the same gaps a game's has, so the build's
    acceptances cover it too."""
    jobs = build.translation_jobs(tmp_path, tmp_path / "build", allow_table_gaps="switch 1003c81c",
                                  aux_modules=("blit",), allow_unmodelled="SSE in the math library")
    assert [job["name"] for job in jobs] == ["exe", "aux-blit"]
    args = jobs[1]["args"]
    assert args[args.index("--module") + 1] == "blit"
    assert args[args.index("--allow-table-gaps") + 1] == "switch 1003c81c"
    assert args[args.index("--allow-unmodelled") + 1] == "SSE in the math library"
    assert jobs[1]["report"] == tmp_path / "build/recomp/translate-blit-report.json"


def test_discovered_reaches_the_translator(tmp_path):
    jobs = build.translation_jobs(tmp_path, tmp_path / "build", aux_modules=("blit",),
                                  discovered=tmp_path / "discovery.txt")
    assert jobs[0]["args"][jobs[0]["args"].index("--discovered") + 1] == str(tmp_path / "discovery.txt")
    assert "--discovered" not in jobs[1]["args"]


def test_no_table_gap_flag_by_default(tmp_path):
    jobs = build.translation_jobs(tmp_path, tmp_path / "build")
    assert "--allow-table-gaps" not in jobs[0]["args"]


def test_translation_parallelism_is_bounded_by_memory():
    assert build.translation_parallelism(system_memory=16 << 30, cpus=12) == 6
    assert build.translation_parallelism(system_memory=64 << 30, cpus=12) == 12
    assert build.translation_parallelism(system_memory=1 << 30, cpus=12) == 1


FAKE_TRANSLATOR = r"""
import os, sys, time
args = sys.argv[1:]
out = args[args.index("--out") + 1]
name = "aux-" + args[args.index("--module") + 1] if "--module" in args else "exe"
with open(os.environ["FAKE_CALLS"], "a") as fh:
    fh.write("%s start %f\n" % (name, time.time()))
time.sleep(float(os.environ.get("FAKE_SLEEP", "0")))
if name in os.environ.get("FAKE_FAIL", "").split(","):
    print("Traceback: %s cannot be translated" % name)
    sys.exit(3)
with open(os.path.join(out, "chunk_000.c"), "w") as fh:
    fh.write("/* %s */\n" % name)
with open(args[args.index("--report") + 1], "w") as fh:
    fh.write("{}")
print("translated 1/1 functions (%s)" % name)
"""


@pytest.fixture
def fake_translator(tmp_path, monkeypatch):
    """translate.py replaced by a script that writes one chunk per image, and
    each image's inputs held in `inputs` for the test to change."""
    script = tmp_path / "fake_translate.py"
    script.write_text(FAKE_TRANSLATOR)
    calls = tmp_path / "calls.txt"
    calls.write_text("")
    monkeypatch.setenv("FAKE_CALLS", str(calls))
    real_popen = build.subprocess.Popen

    def popen(command, **kw):
        assert command[1] == str(build.ROOT / "tools/recomp/translate.py")
        return real_popen([command[0], str(script)] + command[2:], **kw)

    monkeypatch.setattr(build.subprocess, "Popen", popen)
    cfg = {"developer_exe_path": tmp_path / "missing.exe", "listings_path": tmp_path,
           "aux_modules": [{"key": k, "path": tmp_path / ("%s.dll" % k), "listings_path": tmp_path}
                           for k in ("a", "b", "c")]}
    monkeypatch.setattr(build.game_config, "load", lambda game_dir: cfg)
    inputs = {name: {"binary": name} for name in ("exe", "aux-a", "aux-b", "aux-c")}
    monkeypatch.setattr(build, "job_inputs", lambda job, cfg: dict(inputs[job["name"]]))

    def translated():
        return [line.split()[0] for line in calls.read_text().splitlines()]

    def run(stage, **kw):
        stage.mkdir()
        build.run_translator(stage, tmp_path, tmp_path / "build", aux_modules=("a", "b", "c"),
                             log=lambda *a, **k: None, **kw)

    return run, translated, inputs, calls


def test_regeneration_keeps_finished_modules_and_resumes(tmp_path, monkeypatch, fake_translator):
    """2.13: a module is on disk as soon as it is translated, and a run after a
    failure translates only what is left."""
    run, translated, inputs, calls = fake_translator
    monkeypatch.setenv("FAKE_FAIL", "aux-b")
    with pytest.raises(subprocess.CalledProcessError):
        run(tmp_path / "stage1", parallel=1)
    cache = tmp_path / "build/recomp/modules"
    assert sorted(translated()) == ["aux-a", "aux-b", "aux-c", "exe"]
    assert sorted(p.name for p in cache.iterdir() if p.is_dir()) == ["aux-a", "aux-c", "exe"]
    assert not list(cache.glob("*.tmp.*"))
    assert "cannot be translated" in (cache / "aux-b.log").read_text()

    monkeypatch.delenv("FAKE_FAIL")
    calls.write_text("")
    run(tmp_path / "stage2", parallel=4)
    assert translated() == ["aux-b"]
    stage = tmp_path / "stage2"
    assert (stage / "chunk_000.c").read_text() == "/* exe */\n"
    for key in "abc":
        assert (stage / ("aux-" + key) / "chunk_000.c").read_text() == "/* aux-%s */\n" % key
    assert not (stage / build.FINGERPRINT).exists()
    assert (tmp_path / "build/recomp/translate-b-report.json").is_file()


def test_regeneration_translates_only_changed_modules(tmp_path, fake_translator):
    run, translated, inputs, calls = fake_translator
    run(tmp_path / "stage1", parallel=4)
    first = (tmp_path / "stage1/aux-a/chunk_000.c").stat().st_mtime_ns
    calls.write_text("")
    inputs["aux-c"]["binary"] = "changed"
    run(tmp_path / "stage2", parallel=4)
    assert translated() == ["aux-c"]
    # The unchanged module is the same file, so CMake recompiles nothing of it.
    assert (tmp_path / "stage2/aux-a/chunk_000.c").stat().st_mtime_ns == first


def test_retranslated_identical_output_keeps_its_mtime(tmp_path, fake_translator):
    """A game.toml edit retranslates every module; one that comes out the same
    must not be recompiled."""
    run, translated, inputs, calls = fake_translator
    run(tmp_path / "stage1", parallel=4)
    chunk = tmp_path / "build/recomp/modules/aux-b/chunk_000.c"
    os.utime(chunk, ns=(1_000_000_000, 1_000_000_000))
    inputs["aux-b"]["binary"] = "changed, same translation"
    calls.write_text("")
    run(tmp_path / "stage2", parallel=4)
    assert translated() == ["aux-b"]
    assert (tmp_path / "stage2/aux-b/chunk_000.c").stat().st_mtime_ns == 1_000_000_000


def test_a_module_reads_game_toml_without_the_other_modules(tmp_path):
    toml = tmp_path / "game.toml"
    base = '[game]\nid = "g"\n[modules.aux.a]\nentry_points = [1]\n[modules.aux.b]\nentry_points = [2]\n'
    toml.write_text(base)
    before = {k: build._game_toml_digest(toml, k) for k in ("a", "b", None)}
    toml.write_text(base.replace("[2]", "[2, 3]"))
    after = {k: build._game_toml_digest(toml, k) for k in ("a", "b", None)}
    assert after["a"] == before["a"]
    assert after["b"] != before["b"] and after[None] != before[None]
    toml.write_text(base.replace('"g"', '"h"'))
    assert build._game_toml_digest(toml, "a") != before["a"]


def test_launcher_only_keys_translate_nothing(tmp_path):
    toml = tmp_path / "game.toml"
    base = '[game]\nid = "g"\n[launcher]\ntitle = "G"\n[modules.aux.a]\nentry_points = [1]\n'
    toml.write_text(base)
    before = build._game_toml_digest(toml, "a")
    toml.write_text(base.replace('[1]\n', '[1]\nfrom = "a.dll"\nfrom_sha256 = "%s"\nprepare = ""\n' % ("0" * 64))
                    + '[launcher.steam]\ntool = "DepotDownloader"\napp = 1\n')
    assert build._game_toml_digest(toml, "a") == before
    toml.write_text(base.replace('"G"', '"H"'))
    assert build._game_toml_digest(toml, "a") != before


def test_launcher_config_renders_steam_and_staged_modules():
    import launcher_config

    steam = launcher_config.load_steam({"steam": {"tool": "DepotDownloader", "app": 39500, "tool_url": "https://x/y.zip",
                                                  "tool_sha256": "2" * 64, "depots": [
        {"id": 39501, "manifest": "8013367546631285348", "files": ["regex:^(?!redist/)"], "mb": 3230}]}}, "t")
    assert steam["depots"][0]["branch"] == "" and steam["depots"][0]["label"] == ""
    assert launcher_config.load_steam({}, "t") == {"tool": "", "tool_url": "", "tool_sha256": "", "tool_source": "",
                                                   "app": 0, "depots": [], "ini": []}
    launcher_config.check_ini_edits(["Ini/ge3.ini|Language|Audio=Polish", "Ini/mountlist.ini||+Data/X=Data/X"], "t")
    for bad in ("Ini/ge3.ini|Language|Audio", "Ini/a.ini||line", "../a.ini||+x", "a.ini|x"):
        with pytest.raises(ValueError):
            launcher_config.check_ini_edits([bad], "t")
    with pytest.raises(ValueError):  # a tool with nowhere to get it from
        launcher_config.load_steam({"steam": {"tool": "D", "app": 1, "depots": [{"id": 2, "manifest": "3"}]}}, "t")
    with pytest.raises(ValueError):
        launcher_config.load_steam({"steam": {"tool": "DepotDownloader", "app": 1}}, "t")
    with pytest.raises(ValueError):  # a manifest id as a TOML integer
        launcher_config.load_steam({"steam": {"tool": "D", "app": 1, "tool_url": "u", "tool_sha256": "2" * 64,
                                              "depots": [{"id": 2, "manifest": 3}]}}, "t")
    assert launcher_config.aux_stage({"name": "a.dll"}, "t", "a") is None
    staged = launcher_config.aux_stage({"from": "redist/x.cab", "from_sha256": "1" * 64, "prepare": "cab:a.dll"}, "t", "a")
    assert staged == {"from": "redist/x.cab", "from_sha256": "1" * 64, "prepare": "cab:a.dll"}
    for bad in ({"from": "../a.dll", "from_sha256": "1" * 64}, {"from": "a.dll", "from_sha256": "1"},
                {"from": "a.dll", "from_sha256": "1" * 64, "prepare": "upx"}):
        with pytest.raises(ValueError):
            launcher_config.aux_stage(bad, "t", "a")


def test_regeneration_runs_modules_at_once(tmp_path, monkeypatch, fake_translator):
    run, translated, inputs, calls = fake_translator
    monkeypatch.setenv("FAKE_SLEEP", "1")
    run(tmp_path / "stage", parallel=4)
    starts = [float(line.split()[2]) for line in calls.read_text().splitlines()]
    assert max(starts) - min(starts) < 0.9


def test_a_module_game_toml_drops_leaves_the_cache(tmp_path, fake_translator):
    run, translated, inputs, calls = fake_translator
    run(tmp_path / "stage1", parallel=4)
    (tmp_path / "build/recomp/modules/aux-gone").mkdir()
    run(tmp_path / "stage2", parallel=4)
    assert not (tmp_path / "build/recomp/modules/aux-gone").exists()


def test_incremental_build_refreshes_runtime_header_without_translation(tmp_path, monkeypatch):
    args, parser = build.parse_args(["--target", "gen"], system="Darwin")
    args.build_root = tmp_path / "build"
    gen = args.build_root / "recomp/gen"
    gen.mkdir(parents=True)
    header = gen / "x86.h"
    header.write_text("stale runtime header\n")
    table = gen / "table.c"
    table.write_text("existing translation\n")
    original_table = table.stat().st_mtime_ns
    monkeypatch.setattr(build, "parse_args", lambda argv: (args, parser))
    monkeypatch.setattr(build, "run_translator", lambda *a: pytest.fail("unexpected regeneration"))
    timestamps = []

    def configure(*a, **kw):
        assert header.read_bytes() == (build.ROOT / "runtime/x86.h").read_bytes()
        assert table.read_text() == "existing translation\n"
        assert table.stat().st_mtime_ns == original_table
        timestamps.append(header.stat().st_mtime_ns)

    monkeypatch.setattr(build, "configure", configure)
    monkeypatch.setattr(build, "build", lambda *a, **kw: None)
    build.main()
    build.main()
    assert timestamps[0] == timestamps[1]  # unchanged headers do not rebuild every chunk


def test_a_regeneration_keeps_the_time_of_an_unchanged_runtime_header(tmp_path):
    """Every chunk includes gen/x86.h: a new time on the same bytes would
    recompile the whole translation."""
    build.publish_generated(tmp_path, lambda stage: (stage / "table.c").write_text("t\n"))
    header = tmp_path / "recomp/gen/x86.h"
    assert header.read_bytes() == (build.ROOT / "runtime/x86.h").read_bytes()
    os.utime(header, ns=(1_000_000_000, 1_000_000_000))
    build.publish_generated(tmp_path, lambda stage: (stage / "table.c").write_text("t\n"))
    assert header.stat().st_mtime_ns == 1_000_000_000
