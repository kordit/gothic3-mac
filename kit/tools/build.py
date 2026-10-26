#!/usr/bin/env python3
"""Build the native app through CMake, regenerating original-game code only when needed.

The game is a directory holding game.toml (tools/build.py --game-dir). Its
outputs (the translation, the texture pack, the apps, the logs) go under
<game-dir>/build when the game lives outside the kit, else under the kit's
build/. The kit's own default is games/stub, a game that does not exist."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import tomllib
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/recomp"))
sys.path.insert(0, str(ROOT / "tools"))
import game_config  # noqa: E402
import buildlock  # noqa: E402
import copy_layouts  # noqa: E402
import package_desktop  # noqa: E402
import stage_game_files  # noqa: E402
import web_launcher  # noqa: E402

# What each --target builds. `plugins` is every mod plugin the game ships.
TARGETS = {
    "app": ["recomp_app"],
    "smoke": ["pop_smoke"],
    "headless": ["pop_headless"],
    "fixture": ["pop_fixture"],
    "gen": ["recomp_gen"],
    "plugins": ["plugins"],
    "ios": ["recomp_app"],
    "android": ["recomp_app"],
    "web": ["recomp_app"],
}
MACOS_ONLY = {"ios"}
NEEDS_GEN = {"app", "smoke", "headless", "fixture", "gen", "ios", "android", "web"}
# Targets with presets of their own, whatever the host system.
OWN_PRESET = {"ios", "android", "web"}


def default_preset(system=None):
    """The CMake preset for this operating system."""
    return {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}[system or platform.system()]


def preset_name(preset, config, stub=False, target=None):
    """Debug, stub, mobile and web builds use separate presets and binary directories."""
    if target in OWN_PRESET:
        return target + "-stub" if stub else target
    if stub:
        return preset + "-stub"
    return preset if config == "Release" else preset + "-debug"


def build_root_for(game_dir, root=ROOT):
    """Outputs live beside the game when it is outside the kit, else in the kit's build/."""
    game_dir = Path(game_dir)
    try:
        game_dir.relative_to(root)
        return Path(root) / "build"
    except ValueError:
        return game_dir / "build"


def build_dir_for(build_root, preset):
    """The CMake binary directory: what the presets name inside the kit, beside the game outside it."""
    return Path(build_root) / "cmake" / preset


def archive_path(build_root, system=None, preset=None):
    """Where CMake writes the translated archive on this platform."""
    name = "recomp_gen.lib" if (system or platform.system()) == "Windows" else "librecomp_gen.a"
    return Path(build_root) / "cmake" / (preset or default_preset(system)) / "lib" / name


def cmake_tool(name):
    """Prefer the venv's pinned cmake/ctest beside this interpreter, then PATH."""
    beside = Path(sys.executable).parent / name
    if beside.exists():
        return str(beside)
    return shutil.which(name) or name


def game_defines(game_dir, build_root, version=None):
    """The two cache paths every configure needs, and the version baked into the hosts when one is given (else the
    CMake default: git describe, or "unknown" in a source archive)."""
    defines = ["-DRECOMP_GAME_DIR=%s" % Path(game_dir).as_posix(), "-DPOP_BUILD_ROOT=%s" % Path(build_root).as_posix()]
    if version:
        defines.append("-DPOP_RECOMP_VERSION=%s" % version)
    return defines


def configure(preset, extra=(), build_dir=None):
    """Configure a preset; `build_dir` overrides the preset's binary directory."""
    command = [cmake_tool("cmake"), "--preset", preset, "-DPython3_EXECUTABLE=" + sys.executable]
    if build_dir is not None:
        command += ["-B", str(build_dir)]
    subprocess.run(command + list(extra), cwd=ROOT, check=True)


def build(preset, targets, jobs, extra=(), build_dir=None, config="Release"):
    """`extra` goes after the targets: a leading "--" hands the rest to the native tool.

    A build directory named directly (not through the build preset) needs the
    configuration spelled out too: the Xcode generator is multi-config and
    would otherwise build Debug, whose -O0 translation overflows the guest's
    stack on the device."""
    command = [cmake_tool("cmake"), "--build"]
    command += [str(build_dir), "--config", config] if build_dir is not None else ["--preset", preset]
    command += ["--parallel", str(jobs), "--target"] + list(targets) + list(extra)
    subprocess.run(command, cwd=ROOT, check=True)


def devicectl_list():
    """Paired devices as devicectl reports them."""
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        path = tmp.name
    subprocess.run(["xcrun", "devicectl", "list", "devices", "--json-output", path], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(path) as fh:
        return json.load(fh)["result"]["devices"]


def pick_device(devices):
    """The one paired iPad, or exit asking for --device."""
    ipads = [d for d in devices
             if d.get("hardwareProperties", {}).get("productType", "").startswith("iPad")
             and d.get("connectionProperties", {}).get("pairingState") == "paired"]
    if len(ipads) == 1:
        return ipads[0]["identifier"]
    names = ", ".join("%s (%s)" % (d.get("deviceProperties", {}).get("name", "?"), d["identifier"]) for d in ipads)
    sys.exit("Pass --device <identifier>; paired iPads: %s" % (names or "none"))


def ios_app_bundle(app_name, build_root):
    """The signed bundle under <build root>/ios; Xcode adds a configuration directory (Release-iphoneos)."""
    found = sorted((Path(build_root) / "ios").glob("**/%s.app" % app_name))
    if not found:
        sys.exit("No %s.app under %s/ios; did the iOS build succeed?" % (app_name, build_root))
    return found[-1]


def install_and_launch(app, bundle_id, device, console):
    """Install the bundle with devicectl and launch it, optionally streaming its console."""
    subprocess.run(["xcrun", "devicectl", "device", "install", "app", "--device", device, str(app)], check=True)
    launch = ["xcrun", "devicectl", "device", "process", "launch", "--terminate-existing", "--device", device]
    if console:
        launch.append("--console")
    launch.append(bundle_id)
    subprocess.run(launch, check=True)


def android_project(build_root, cfg, *, gen_dir):
    """Render the Gradle project without building it; gen_dir is the CMake binary directory.

    SDL's Java sources stay in that build's FetchContent checkout. Gradle
    only packages the native library built by CMake, never invokes CMake.
    """
    template = ROOT / "platform/android"
    out = Path(build_root) / "android"
    shutil.copytree(template, out, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns(".gradle", "build", ".gitignore", "local.properties"))
    values = {key: cfg["game"][key] for key in ("app_name", "bundle_id", "id")}
    values["sdl_java_dir"] = (Path(gen_dir).resolve() / "_deps/sdl3-src/android-project/app/src/main/java").as_posix()
    for original in template.rglob("*.in"):
        source = out / original.relative_to(template)
        text = source.read_text()
        for key, value in values.items():
            if source.name.endswith(".xml.in"):
                value = escape(value, {'"': "&quot;", "'": "&apos;"})
            else:
                value = json.dumps(value, ensure_ascii=False)[1:-1].replace("$", r"\$")
            text = text.replace("@%s@" % key, value)
        source.with_suffix("").write_text(text)
        source.unlink()
    return out


def android_stage_layout_assets(out, game_dir):
    """Put the game's control layouts in the APK's assets, as controls/.

    RecompActivity copies them out to the app's external files folder on
    start, which is where host_resource("controls") looks on Android. The
    directory is rebuilt from scratch so a layout the game dropped also
    leaves the APK, as the ffmpeg notice does when video goes off.
    """
    controls = Path(out) / "app/src/main/assets/controls"
    if controls.exists():
        shutil.rmtree(controls)
    return copy_layouts.copy_layouts(game_dir, controls)


def android_stage_core_assets(out, game_dir):
    """Package core manifests/data; native plugins are linked into libmain.so.

    Never carry a developer's desktop binaries, sources or debug bundles into
    the APK. Only the app-owned core tree is rebuilt; profiles are unrelated.
    """
    from copy_core_mods import copy_core_mods
    copy_core_mods(game_dir, Path(out) / "app/src/main/assets/mods/core")


def android_apk(build_root, cfg, *, gen_dir, game_dir):
    """Stage the native libraries beside the SDL activity and assemble a debug APK."""
    library = Path(gen_dir) / "host/libmain.so"
    sdl_activity = Path(gen_dir) / "_deps/sdl3-src/android-project/app/src/main/java/org/libsdl/app/SDLActivity.java"
    for path in (library, sdl_activity):
        if not path.is_file():
            raise FileNotFoundError("Android CMake build input is missing: %s" % path)
    out = android_project(build_root, cfg, gen_dir=gen_dir)
    jni = out / "app/src/main/jniLibs/arm64-v8a"
    jni.mkdir(parents=True, exist_ok=True)
    shutil.copy2(library, jni / "libmain.so")
    # Read the configured option, not leftover installed libraries: switching
    # video off must remove the previous build's copies from the APK too.
    cache = (Path(gen_dir) / "CMakeCache.txt").read_text().splitlines()
    video = any(line.startswith("RECOMP_VIDEO:BOOL=") and
                line.partition("=")[2].upper() in {"1", "ON", "YES", "TRUE", "Y"}
                for line in cache)
    for component in ("avformat", "avcodec", "avutil"):
        name = "lib%s.so" % component
        if video:
            shutil.copy2(Path(gen_dir) / "ffmpeg/lib" / name, jni / name)
        else:
            (jni / name).unlink(missing_ok=True)
    notice = out / "app/src/main/assets/ffmpeg-NOTICE.md"
    if video:
        notice.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / "third_party/ffmpeg/NOTICE.md", notice)
    else:
        notice.unlink(missing_ok=True)
    android_stage_layout_assets(out, game_dir)
    android_stage_core_assets(out, game_dir)
    wrapper = "gradlew.bat" if platform.system() == "Windows" else "./gradlew"
    subprocess.run([wrapper, "assembleDebug"], cwd=out, check=True)
    apk = out / "app/build/outputs/apk/debug/app-debug.apk"
    if not apk.is_file():
        raise FileNotFoundError("No APK after assembleDebug: %s" % apk)
    print("Packaged %s (%d bytes)" % (apk, apk.stat().st_size), flush=True)
    return apk


def android_push_game(command, cfg, build_root):
    """Stage the configured install, then push game/ without deleting device saves.

    Recreate only our generated staging directory so exclusions also apply to
    files staged by an earlier build. The shared stager uses excluded() and
    writes the executable hash to .stamp, just as it does for an iOS bundle.
    """
    source = cfg["developer_exe_path"].parent
    executable = cfg["game"]["executable"]
    if not (source / executable).is_file():
        raise FileNotFoundError("Prepare your own game installation with tools/setup.py before --push-game")
    staged = Path(build_root) / "android/game"
    if staged.exists():
        shutil.rmtree(staged)
    count = stage_game_files.stage(source, staged, executable, cfg["bundle"]["exclude"],
                                   stage_game_files.kept(cfg))
    destination = "/sdcard/Android/data/%s/files" % cfg["game"]["bundle_id"]
    print("Staged %d game files in %s; pushing to %s/game" % (count, staged, destination), flush=True)
    subprocess.run(command + ["shell", "mkdir", "-p", destination], check=True)
    # Push game/ into its parent on every run: never create game/game/.
    subprocess.run(command + ["push", str(staged), destination + "/"], check=True)


def android_install_and_launch(apk, bundle_id, device=None, console=False, *, game_cfg=None, build_root=None):
    """Install, optionally push game data, then launch on one ready adb device.

    An explicit data push requires a device; a build alone may skip device
    actions. The APK is installed first so Android owns the external files path.
    """
    adb = shutil.which("adb")
    if not adb and os.environ.get("ANDROID_HOME"):
        name = "adb.exe" if platform.system() == "Windows" else "adb"
        candidate = Path(os.environ["ANDROID_HOME"]) / "platform-tools" / name
        if candidate.is_file():
            adb = str(candidate)
    if not adb:
        if game_cfg is not None:
            raise ValueError("adb unavailable; --push-game requires Android platform-tools and a ready device")
        print("adb unavailable; skipped Android install, launch and logcat.")
        return
    result = subprocess.run([adb, "devices"], check=True, capture_output=True, text=True)
    devices = [fields[0] for line in result.stdout.splitlines()
               if len(fields := line.split()) == 2 and fields[1] == "device"]
    if not devices:
        if game_cfg is not None:
            raise ValueError("No Android device attached; --push-game requires a ready device in adb devices")
        print("No Android device attached; skipped install, launch and logcat.")
        return
    if device is not None and device not in devices:
        raise ValueError("Android device %s is not ready in adb devices" % device)
    if device is None and len(devices) != 1:
        raise ValueError("Pass --device <adb serial>; ready Android devices: %s" % ", ".join(devices))
    command = [adb, "-s", device or devices[0]]
    subprocess.run(command + ["install", "-r", str(apk)], check=True)
    if game_cfg is not None:
        android_push_game(command, game_cfg, build_root)
    subprocess.run(command + ["shell", "am", "start", "-n", bundle_id + "/dev.recompkit.RecompActivity"], check=True)
    if console:
        subprocess.run(command + ["logcat"], check=True)


def publish_generated(build_root, translate):
    """Stage a translation, then publish gen/ and symbols.json by rename.

    `translate(stage_dir)` writes the sources and raises on failure; the
    published tree is untouched in that case. Publishing is renames only, so
    a reader under the same lock never sees half a generation. What survives
    a failure or an interruption is each finished module's own translation
    (run_translator keeps them under recomp/modules)."""
    recomp = Path(build_root) / "recomp"
    recomp.mkdir(parents=True, exist_ok=True)
    gen, old = recomp / "gen", recomp / "gen.old"
    stage = recomp / ("gen.new.%d" % os.getpid())
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir()
    try:
        translate(stage)
        # x86.h sits beside the generated sources so #include "x86.h" resolves.
        # Every chunk includes it: an unchanged header keeps its time, or a
        # regeneration would recompile the whole translation.
        published = gen / "x86.h"
        if published.is_file() and published.read_bytes() == (ROOT / "runtime/x86.h").read_bytes():
            shutil.copy2(published, stage / "x86.h")
        else:
            shutil.copy(ROOT / "runtime/x86.h", stage / "x86.h")
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    shutil.rmtree(old, ignore_errors=True)
    if gen.exists():
        gen.rename(old)
    stage.rename(gen)
    symbols = gen / "symbols.json"
    if symbols.is_file():
        temporary = recomp / ("symbols.json.new.%d" % os.getpid())
        shutil.copy(symbols, temporary)
        temporary.replace(recomp / "symbols.json")
    shutil.rmtree(old, ignore_errors=True)


#: What a translation reads besides its game's files. A change to either
#: retranslates every module.
TRANSLATOR_SOURCES = ("tools/recomp/translate.py", "tools/game_config.py")
#: A module's translation peaks near 1.8 GB (Gothic 3's d3dx9_40.dll; its
#: Engine.dll, 29k functions, 1.4 GB). Parallel translations get 2.5 GB of
#: physical memory each, which leaves the rest of a 16 GB machine its own
#: (six at once there; the largest modules start first and overlap).
TRANSLATION_MEMORY = 5 << 29
FINGERPRINT = ".inputs.json"
#: game.toml keys tools/launcher_config.py reads and no translation does.
LAUNCHER_ONLY_KEYS = {"launcher": ("steam",), "aux": ("from", "from_sha256", "prepare")}


def translation_jobs(game_dir, build_root, allow_table_gaps=None, aux_modules=(),
                     allow_unmodelled=None, discovered=None, forget=None):
    """One translation per image: the executable, then each auxiliary module
    (game.toml [modules.aux.<key>]), which cmake/Translate.cmake compiles from
    gen/aux-<key> into its own library. A module is translated under the same
    --allow-table-gaps/--allow-unmodelled acceptances as the image: they are
    the build's, and a module's listing has the same gaps a game's has.

    Each job is {name, args, report}; `args` lacks only --out, which
    run_translator supplies."""
    recomp = Path(build_root) / "recomp"
    common = ["--game", str(game_dir)]
    if allow_table_gaps:
        common += ["--allow-table-gaps", allow_table_gaps]
    if allow_unmodelled:
        common += ["--allow-unmodelled", allow_unmodelled]
    image = common[:]
    if discovered:
        image += ["--discovered", str(discovered)]
    if forget:
        image += ["--forget", forget]
    jobs = [{"name": "exe", "key": None, "args": image, "report": recomp / "translate-report.json"}]
    for key in aux_modules:
        jobs.append({"name": "aux-" + key, "key": key, "args": common + ["--module", key],
                     "report": recomp / ("translate-%s-report.json" % key)})
    return jobs


def _file_digest(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _listing_digest(listings):
    """functions.tsv by content, the per-function listings by name, size and
    mtime: a Ghidra export rewrites every file it touches."""
    listings = Path(listings)
    digest = hashlib.sha256()
    tsv = listings / "functions.tsv"
    digest.update(_file_digest(tsv).encode() if tsv.is_file() else b"-")
    functions = listings / "functions"
    if functions.is_dir():
        with os.scandir(functions) as entries:
            for entry in sorted(entries, key=lambda e: e.name):
                st = entry.stat()
                digest.update(("%s %d %d\n" % (entry.name, st.st_size, st.st_mtime_ns)).encode())
    return digest.hexdigest()


def job_image(job, cfg):
    """The binary and the listings directory a job translates."""
    if job["key"] is None:
        return cfg["developer_exe_path"], cfg["listings_path"]
    module = {m["key"]: m for m in cfg["aux_modules"]}[job["key"]]
    return module["path"], module["listings_path"]


def _game_toml_digest(path, key):
    """game.toml as the job with this module key reads it. A module's
    translation reads its own [modules.aux.<key>] and the rest of the file, but
    never another module's table, so an entry point added to one DLL leaves the
    others translated. The executable's job (key None) counts the whole file."""
    if key is None:
        return _file_digest(path)
    with open(path, "rb") as fh:
        doc = tomllib.load(fh)
    aux = doc.get("modules", {}).get("aux")
    if isinstance(aux, dict):
        doc["modules"]["aux"] = {k: {n: x for n, x in v.items() if n not in LAUNCHER_ONLY_KEYS["aux"]}
                                 if isinstance(v, dict) else v for k, v in aux.items() if k == key}
    # What only the launcher reads (tools/launcher_config.py) translates nothing.
    if isinstance(doc.get("launcher"), dict):
        doc["launcher"] = {n: x for n, x in doc["launcher"].items() if n not in LAUNCHER_ONLY_KEYS["launcher"]}
    return hashlib.sha256(json.dumps(doc, sort_keys=True, default=str).encode()).hexdigest()


def job_inputs(job, cfg):
    """Everything the job's translation reads, digested: a cached module whose
    inputs are equal is the translation it would produce again. The globals
    file counts whole, and game.toml whole but for the other modules' tables
    (_game_toml_digest); that is cheaper than missing a key a module reads."""
    game_dir = Path(cfg["dir"])
    binary, listings = job_image(job, cfg)
    args = list(job["args"])
    if "--discovered" in args:
        at = args.index("--discovered") + 1
        args[at] = "sha256:" + _file_digest(args[at])
    globals_file = game_dir / cfg["translate"].get("globals", "globals.toml")
    return {
        "args": args[2:],  # without --game <dir>: the same game moved is the same translation
        "translator": {name: _file_digest(ROOT / name) for name in TRANSLATOR_SOURCES},
        "game.toml": _game_toml_digest(game_dir / "game.toml", job["key"]),
        "globals": _file_digest(globals_file) if globals_file.is_file() else None,
        "binary": _file_digest(binary) if Path(binary).is_file() else None,
        "listings": _listing_digest(listings),
    }


def translation_parallelism(system_memory=None, cpus=None):
    """As many translations as there are cores and memory for, at least one."""
    if system_memory is None:
        try:
            system_memory = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
        except (AttributeError, ValueError, OSError):
            system_memory = TRANSLATION_MEMORY
    return max(1, min(cpus or os.cpu_count() or 1, system_memory // TRANSLATION_MEMORY))


def _cached(directory):
    try:
        return json.loads((Path(directory) / FINGERPRINT).read_text())
    except (OSError, ValueError):
        return None


def _link_tree(source, destination):
    """Hard links where the file system has them: a published module keeps the
    cached files' mtimes, so an unchanged module recompiles nothing."""
    destination.mkdir(parents=True, exist_ok=True)
    for entry in source.iterdir():
        if entry.name == FINGERPRINT:
            continue
        target = destination / entry.name
        if entry.is_dir():
            _link_tree(entry, target)
            continue
        try:
            os.link(entry, target)
        except OSError:
            shutil.copy2(entry, target)


def _keep_unchanged_mtimes(new, old):
    """A file translated again to the same bytes takes the old file's times, so
    CMake does not recompile it: a change to game.toml retranslates every
    module, and most of them come out the same."""
    if not old.is_dir():
        return
    for entry in new.rglob("*"):
        previous = old / entry.relative_to(new)
        if not entry.is_file() or not previous.is_file() or entry.name == FINGERPRINT:
            continue
        before = previous.stat()
        if before.st_size == entry.stat().st_size and previous.read_bytes() == entry.read_bytes():
            os.utime(entry, ns=(before.st_atime_ns, before.st_mtime_ns))


def _translate_jobs(pending, cache, parallel, log=print):
    """Run the translations, at most `parallel` at once and the longest first.
    A finished module is renamed into the cache at once, so an interrupted or
    failed build keeps it. A failure does not stop the other modules (make -k):
    it is raised once they are done, and the next run translates only it."""
    running, failed = {}, []
    queue = list(pending)
    total, done = len(queue), 0
    try:
        while queue or running:
            while queue and len(running) < parallel:
                job = queue.pop(0)
                scratch = cache / ("%s.tmp.%d" % (job["name"], os.getpid()))
                shutil.rmtree(scratch, ignore_errors=True)
                scratch.mkdir(parents=True)
                logfile = open(cache / ("%s.log" % job["name"]), "w")
                command = ([sys.executable, str(ROOT / "tools/recomp/translate.py"), "--out", str(scratch)]
                           + job["args"] + ["--report", str(scratch) + ".report.json"])
                process = subprocess.Popen(command, cwd=ROOT, stdout=logfile, stderr=subprocess.STDOUT)
                running[process] = (job, scratch, logfile, time.monotonic())
            if not running:
                break
            time.sleep(0.2)
            for process in [p for p in running if p.poll() is not None]:
                job, scratch, logfile, started = running.pop(process)
                logfile.close()
                done += 1
                seconds = time.monotonic() - started
                lines = (cache / ("%s.log" % job["name"])).read_text(errors="replace").splitlines()
                if process.returncode:
                    shutil.rmtree(scratch, ignore_errors=True)
                    failed.append((job, process.returncode))
                    log("[%d/%d] %s FAILED (exit %d) after %.0f s; last lines of %s:\n%s"
                        % (done, total, job["name"], process.returncode, seconds,
                           cache / ("%s.log" % job["name"]), "\n".join("    " + l for l in lines[-15:])))
                    continue
                inputs = dict(job["inputs"], seconds=round(seconds, 1))
                (scratch / FINGERPRINT).write_text(json.dumps(inputs, indent=1, sort_keys=True))
                final = cache / job["name"]
                _keep_unchanged_mtimes(scratch, final)
                shutil.rmtree(final, ignore_errors=True)
                scratch.rename(final)
                report = Path(str(scratch) + ".report.json")
                if report.is_file():
                    report.replace(job["report"])
                summary = next((l for l in reversed(lines) if l.startswith("translated ")), "")
                log("[%d/%d] %s in %.0f s %s" % (done, total, job["name"], seconds, summary[:160]), flush=True)
    finally:
        for process, (job, scratch, logfile, _) in running.items():
            process.terminate()
        for process, (job, scratch, logfile, _) in running.items():
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            logfile.close()
            shutil.rmtree(scratch, ignore_errors=True)
    if failed:
        job, code = failed[0]
        raise subprocess.CalledProcessError(code, "translate.py " + " ".join(job["args"]))


def run_translator(stage, game_dir, build_root, allow_table_gaps=None, aux_modules=(),
                   allow_unmodelled=None, discovered=None, forget=None, parallel=None, log=print):
    """Translate every image into `stage`: the executable at its top, each
    module in aux-<key> (translation_jobs).

    Translations run in parallel processes and land in <build root>/recomp/
    modules/<name> as each one finishes, beside a digest of its inputs
    (job_inputs). A module whose inputs are unchanged is not translated again,
    so a regeneration that was interrupted or failed resumes with what is
    left. `stage` is then filled with links to the cached modules."""
    cfg = game_config.load(game_dir)
    cache = Path(build_root) / "recomp/modules"
    cache.mkdir(parents=True, exist_ok=True)
    for stale in cache.glob("*.tmp.*"):  # an interrupted build's: the build lock is ours
        if stale.is_dir():
            shutil.rmtree(stale, ignore_errors=True)
        else:
            stale.unlink()
    jobs = translation_jobs(game_dir, build_root, allow_table_gaps, aux_modules,
                            allow_unmodelled, discovered, forget)
    pending = []
    for job in jobs:
        job["inputs"] = job_inputs(job, cfg)
        cached = _cached(cache / job["name"])
        if cached is not None and dict(cached, seconds=None) == dict(job["inputs"], seconds=None):
            continue
        # Seconds the last translation took, else a guess from the size:
        # about 20 s per MB of image on an M-series core.
        binary = Path(job_image(job, cfg)[0])
        size = binary.stat().st_size if binary.is_file() else 0
        job["estimate"] = (cached or {}).get("seconds") or size / 50000
        pending.append(job)
    reused = len(jobs) - len(pending)
    log("translating %d of %d images (%d unchanged since their last translation), %d at once"
        % (len(pending), len(jobs), reused, parallel or translation_parallelism()), flush=True)
    # The longest first: the slowest module bounds the whole regeneration.
    pending.sort(key=lambda job: -job["estimate"])
    _translate_jobs(pending, cache, parallel or translation_parallelism(), log)
    stage = Path(stage)
    for job in jobs:
        _link_tree(cache / job["name"], stage if job["key"] is None else stage / job["name"])
    names = {job["name"] for job in jobs}
    for leftover in cache.iterdir():  # modules game.toml no longer lists
        if leftover.is_dir() and leftover.name not in names:
            shutil.rmtree(leftover, ignore_errors=True)


def texture_pack(game_dir, build_root):
    """Compile the redistributable material-detail layer when its inputs are newer.
    Original-game replacement textures remain optional, locally prepared pack entries.
    A game without artwork has no texture pack."""
    detail = Path(build_root) / "texture-pack/terrain-detail.popt"
    artwork = Path(game_dir) / "assets/terrain/materials-v1.png"
    compiler = ROOT / "tools/recomp/terrain_detail.py"
    if not artwork.is_file():
        return
    if (not detail.is_file() or not (detail.parent / "manifest.json").is_file()
            or detail.stat().st_mtime < max(artwork.stat().st_mtime, compiler.stat().st_mtime)):
        subprocess.run([sys.executable, str(compiler), "--source", str(artwork),
                        "--output", str(detail.parent)], cwd=ROOT, check=True)


def web_site(game_dir, build_root, preset, cfg):
    """The launcher page with this game's web build beside it, ready to serve."""
    site = Path(build_root) / (preset + "-site")
    web_launcher.build([game_dir], site)
    web_launcher.copy_web_build(cfg["game"]["id"], Path(build_root) / preset / "recomp", site)
    return site


def parse_args(argv, system=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regenerate", action="store_true", help="Regenerate and compile translated C")
    parser.add_argument("--translate-only", action="store_true",
                        help="With --regenerate: publish the translation and stop before CMake")
    parser.add_argument("--allow-table-gaps", metavar="REASON", default=None,
                        help="Accept jump-table sites the translator cannot decode (passed to translate.py)")
    parser.add_argument("--forget", metavar="ADDR[,ADDR...]", default=None,
                        help="translate as if the listing had never named these functions "
                             "(tools/recomp/translate.py --forget): an experiment, not a build")
    parser.add_argument("--discovered", metavar="FILE", default=None, type=Path,
                        help="a file a run wrote with RECOMP_DISCOVERY: the addresses it "
                             "reached that the translation did not carry become entry points")
    parser.add_argument("--allow-unmodelled", metavar="REASON", default=None,
                        help="Translate instructions the translator cannot model into a trap at "
                             "their own address (passed to translate.py)")
    parser.add_argument("--version", default=None,
                        help="Version baked into the hosts (POP_RECOMP_VERSION; default: git describe, else unknown). "
                        "A game's Setup passes its release version: a source archive has no git history")
    parser.add_argument("--target", choices=sorted(TARGETS), default="app")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument("--translate-jobs", type=int, default=None,
                        help="Translations run at once by --regenerate (default: one per core, "
                             "at most one per 2.5 GB of memory)")
    parser.add_argument("--preset", default=default_preset(system), help="CMake configure preset")
    parser.add_argument("--config", choices=("Release", "Debug"), default="Release")
    parser.add_argument("--game-dir", type=Path, default=ROOT / "games/stub",
                        help="Absolute directory holding the game.toml this build is for (default: the kit's stub game)")
    parser.add_argument("--stub", action="store_true",
                        help="Link the hosts against a stub translation (no game code; CI's build)")
    parser.add_argument("--device", default=None, help="devicectl identifier (iOS) or adb serial (Android)")
    parser.add_argument("--team", default=os.environ.get("RECOMP_IOS_TEAM", ""),
                        help="Apple team id for automatic signing (ios target; default $RECOMP_IOS_TEAM)")
    parser.add_argument("--no-install", action="store_true", help="Build the mobile app without installing it")
    parser.add_argument("--push-game", action="store_true",
                        help="Android: push the configured game install minus [bundle].exclude before launch")
    parser.add_argument("--console", action="store_true", help="After launching on the device, stream its console")
    args = parser.parse_args(argv)
    if args.push_game and (args.target != "android" or args.no_install):
        parser.error("--push-game requires --target android without --no-install")
    if args.target == "ios" and not args.stub and not args.team:
        parser.error("--target ios needs --team or RECOMP_IOS_TEAM")
    if args.translate_only and not args.regenerate:
        parser.error("--translate-only needs --regenerate")
    if args.stub and (args.config == "Debug" or args.regenerate):
        parser.error("--stub cannot be combined with --config Debug or --regenerate")
    if not args.game_dir.is_absolute():
        parser.error("--game-dir must be absolute: %s" % args.game_dir)
    if not (args.game_dir / "game.toml").is_file():
        parser.error("No game config at %s/game.toml" % args.game_dir)
    if args.target == "plugins" and not (args.game_dir / "mods/CMakeLists.txt").is_file():
        parser.error("%s has no mods/CMakeLists.txt; nothing to build for --target plugins" % args.game_dir)
    if args.target in MACOS_ONLY and (system or platform.system()) != "Darwin":
        parser.error("The iOS packager runs on macOS")
    if args.target == "web" and not os.environ.get("EMSDK"):
        parser.error("--target web needs the Emscripten SDK's environment (source emsdk_env.sh)")
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")
    if args.translate_jobs is not None and args.translate_jobs < 1:
        parser.error("--translate-jobs must be at least 1")
    args.build_root = build_root_for(args.game_dir)
    return args, parser


def main():
    """Check inputs, translate under the build lock when needed, then configure and build."""
    args, parser = parse_args(sys.argv[1:])
    # A kill by PID unwinds like Ctrl-C: running translations are stopped,
    # finished ones stay in recomp/modules.
    signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit(128 + signum))
    cfg = game_config.load(args.game_dir)
    # Regenerating needs the game and its listings.
    if args.regenerate and not cfg["developer_exe_path"].is_file():
        parser.error("Prepare your own game installation with tools/setup.py first")
    preset = preset_name(args.preset, args.config, stub=args.stub, target=args.target)
    build_dir = build_dir_for(args.build_root, preset)
    defines = game_defines(args.game_dir, args.build_root, args.version)
    try:
        # The lock lives at <build root>/recomp/.lock: BuildLock joins build/recomp/.lock onto its argument.
        with buildlock.BuildLock(args.build_root.parent, "tools/build.py"):
            if args.target in NEEDS_GEN and args.regenerate:
                if not (cfg["listings_path"] / "functions.tsv").is_file():
                    parser.error("Translation listings are missing; run tools/setup.py without --link-only")
                publish_generated(args.build_root,
                                  lambda stage: run_translator(stage, args.game_dir, args.build_root,
                                                               args.allow_table_gaps,
                                                               [m["key"] for m in cfg["aux_modules"]],
                                                               args.allow_unmodelled,
                                                               args.discovered, args.forget,
                                                               args.translate_jobs))
            # Generated sources include the adjacent runtime header. Refresh
            # it under the same lock even when their translation is unchanged.
            header = args.build_root / "recomp/gen/x86.h"
            if header.parent.is_dir():
                current = (ROOT / "runtime/x86.h").read_bytes()
                if not header.is_file() or header.read_bytes() != current:
                    temporary = header.with_suffix(".h.new")
                    temporary.write_bytes(current)
                    temporary.replace(header)
            if args.translate_only:
                print("Translation published in %s" % header.parent)
            elif args.target == "ios":
                if not args.stub and not (args.build_root / "recomp/gen/table.c").is_file():
                    parser.error("No translation in %s/recomp/gen; run tools/build.py --regenerate on macOS first"
                                 % args.build_root)
                configure(preset, defines + ["-DRECOMP_IOS_TEAM=" + args.team], build_dir=build_dir)
                extra = ["--", "CODE_SIGNING_ALLOWED=NO"] if args.stub else ["--", "-allowProvisioningUpdates"]
                build(preset, TARGETS["ios"], args.jobs, extra, build_dir=build_dir, config="Release")
                if not args.stub and not args.no_install:
                    app = ios_app_bundle(cfg["game"]["app_name"], args.build_root)
                    device = args.device or pick_device(devicectl_list())
                    install_and_launch(app, cfg["game"]["bundle_id"], device, args.console)
            else:
                # A link-only build ships no texture pack, and its CI has no numpy.
                if args.target == "app" and not args.stub:
                    texture_pack(args.game_dir, args.build_root)
                configure(preset, defines, build_dir=build_dir)
                build(preset, TARGETS[args.target], args.jobs, build_dir=build_dir, config=args.config)
                if args.target == "web":
                    site = web_site(args.game_dir, args.build_root, preset, cfg)
                    print("Web site in %s (serve it with tools/web_launcher.py --serve)" % site)
                if args.target == "android":
                    apk = android_apk(args.build_root, cfg, gen_dir=build_dir, game_dir=args.game_dir)
                    if not args.no_install:
                        android_install_and_launch(apk, cfg["game"]["bundle_id"], args.device, args.console,
                                                   game_cfg=cfg if args.push_game else None,
                                                   build_root=args.build_root)
                system = "Windows" if preset.startswith("windows-cross") else platform.system()
                if args.target == "app" and not args.stub and system in {"Linux", "Windows"}:
                    # The desktop Ninja presets write OUTPUT_NAME into POP_OUT.
                    desktop_root = args.build_root / "windows" if preset.startswith("windows-cross") else args.build_root
                    suffix = ".exe" if system == "Windows" else ""
                    binary = desktop_root / "recomp" / (cfg["game"]["app_name"] + suffix)
                    if not binary.is_file():
                        parser.exit(1, "No desktop app binary at %s after the build\n" % binary)
                    packaged = package_desktop.stage(binary, cfg, desktop_root / "package",
                                                     system=system, build_dir=build_dir, game_dir=args.game_dir)
                    print("Packaged %s" % packaged)
    except subprocess.CalledProcessError as error:
        parser.exit(error.returncode or 1, "Build failed; see the compiler output above.\n")
    except TimeoutError as error:
        parser.exit(1, "%s\n" % error)
    except (OSError, ValueError) as error:
        parser.exit(1, "%s\n" % error)


if __name__ == "__main__":
    main()
