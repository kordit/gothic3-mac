#!/usr/bin/env python3
"""The mods a player picked, as a view of files the game reads: links in the profile, nothing copied.

    tools/g3mods.py build <profile>          # <profile>/active-mods.json -> links in <profile>/Data
    tools/g3mods.py plan <profile>           # what build would do, nothing touched
    tools/g3mods.py clear <profile>          # remove the links the last build made
    tools/g3mods.py selftest                 # the naming rules on a temporary tree

The kit reads the profile (RECOMP_PROFILE_DIR) before the game directory, path for path
(kit/mods/overlay.cpp), so <profile>/Data/_compiledImage.mod is, to the game, a file in its Data
folder. Each one is a symbolic link to the file in the mod's own folder: a 1.9 GB texture archive
costs no disk. The kit refuses to write or delete through a link in the profile (resolve_in rejects
symlinks for mutations), so the game cannot damage a mod folder.

active-mods.json (written by the launcher in the profile root):
    {"version": 1, "preset": "<name or null>", "static": "<absolute folder, optional>",
     "mods": [{"id": "<mod.json id>", "path": "<absolute mod folder>"}, ...]}
in load order: a later mod sits higher and wins over an earlier one. "static" is the Sefaris
catalog's Static/ folder; without it, the Static/ beside the first mod's parent folder
(G3_Mods/Mods/<mod> -> G3_Mods/Static) when it has Projects_compiled.m0x.

How the game takes archives (docs/mods/g3-mod-loading.md): for every folder of mountlist.ini,
Engine eCVirtualFileSystem::MountDir mounts X.pak, X.p00, X.p01 ... (stops at the first gap), with
Alternative Balancing X.cpt, X.c00 ..., then X.mod and only if X.mod mounted X.m00, X.m01 ...,
then (AB) X.nod, X.n00 ... A later generation wins. So the mods of one base name get, in load order,
X.mod, X.m00, X.m01 ... (the .nod chain the same way); when the game folder already has X.mod and
some X.mNN, the numbering continues after them.

What a mod folder may hold (Sefaris Mod Manager layout, docs/mods/sefaris.md on the branch of the
agent that read the manager): Mod.ini and ModFiles/. Files in ModFiles (or in ModFiles/Data) named
<base>.m0x/.mod/.mNN go to the .mod chain of <base>, <base>.n0x/.nod/.nNN to the .nod chain, with
the stem in lower case as the manager writes it (x.mod, x.m00). Other *.ini go to ini/ (a later
mod's wins; ge3.ini never). Everything else is left out and listed in the record with the reason:
program files (scripts/Script_*.dll need translating like every other module), stringtable
variants other than stringtable.ini (the manager ignores them too), .pak/.cpt generations,
anything unknown. The order of the mods is the launcher's (active-mods.json), not decided here.

Built from the mods, as Sefaris' installMods does (docs/mods/sefaris.md, section 1), whenever any
mod is picked:
- strings.mod: stringtable.ini of the first Strings* archive of the game's Data (names in upper
  case order, as NTFS lists them) without its [LocAdmin_Revisions] header, then each mod's
  stringtable.ini without its first [LocAdmin_Strings], in load order, then [LocAdmin_Revisions].
  Unlike the manager, a mod's BOM is dropped and a missing last line end is added, so the last
  value of one mod never runs into the next;
- with Static/: G3_World_01/G3_World_01.wrldatasc (Static's, plus everything after the first
  [Sector.List] of each mod's G3_World_01.wrldatasc), packed as the next projects_compiled, and
  Static/Projects_compiled.m0x/.n0x last on the projects_compiled .mod/.nod chains. Those two
  are archives of deleted entries (tools/g3pak.py) that hide the game's compiled infos and
  string table caches.
They are written to <profile>/mods-generated/ (rewritten only when their bytes change) and linked
like the mods' own files.

The record <profile>/mods-view.json lists every link made and every file generated; the next
build or clear removes exactly those links, and only when they are still links. A regular file in
the way is never touched: it is reported and that one link is not made.
"""

import json
import os
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import g3pak  # noqa: E402

RECORD = "mods-view.json"
CONTRACT = "active-mods.json"
GENERATED = "mods-generated"
STRINGTABLE = "stringtable.ini"
WRLDATASC = "G3_World_01.wrldatasc"
# A mod archive's extension. classify() orders a mod's own files: the .mod/.nod head (-1), the
# numbers it asked for, then the manager's placeholder ".m0x" (the next free slot).
ARCHIVE = re.compile(r"^(?P<base>.+)\.(?:(?P<head>mod|nod)|(?P<chain>[mn])(?:(?P<num>\d\d+)|0x))$", re.I)
PROGRAM = {".dll", ".exe", ".asi", ".com", ".bat", ".cmd", ".scr", ".lnk", ".dylib", ".so"}
BASE_GAME = re.compile(r"\.(pak|cpt|p\d\d+|c\d\d+)$", re.I)


def classify(name):
    """(chain, order key) for an archive a mod ships, or (None, reason) for a file left out."""
    m = ARCHIVE.match(name)
    if m:
        if m.group("head"):
            chain, order = m.group("head").lower()[0], -1
        else:
            chain = m.group("chain").lower()
            order = int(m.group("num")) if m.group("num") else 10 ** 6
        return chain, (m.group("base"), order)
    ext = os.path.splitext(name)[1].lower()
    if ext in PROGRAM:
        return None, "program file: needs translating like the game's own modules, not loaded"
    if name.lower() == STRINGTABLE:
        return "strings", (name, 0)
    if re.match(r"stringtable.*\.ini$", name, re.I):
        return None, "a stringtable variant: Sefaris merges only stringtable.ini and ignores this"
    if name.lower() == WRLDATASC.lower():
        return "sectors", (name, 0)
    if name.lower() == "ge3.ini":
        return None, "ge3.ini: the manager never copies it"
    if ext == ".ini":
        return "ini", (name, 0)
    if BASE_GAME.search(name):
        return None, "a .pak/.cpt generation: would replace the game's own archive by name"
    return None, "not an archive the game mounts from Data"


def mod_payload(folder):
    """The files of one mod folder: (relative name, absolute path, subfolder) for each."""
    root = folder / "ModFiles"
    if not root.is_dir():
        root = folder
    out = []
    for entry in sorted(root.iterdir(), key=lambda p: p.name.lower()):
        if entry.is_dir():
            sub = entry.name.lower()
            if sub == "data":
                out += [(f.name, f, "") for f in sorted(entry.iterdir(), key=lambda p: p.name.lower())
                        if f.is_file()]
            else:
                for f in sorted(entry.rglob("*")):
                    if f.is_file():
                        out.append((f.name, f, entry.name))
        elif entry.is_file():
            out.append((entry.name, entry, ""))
    return out


def game_chain(game_data, base, chain):
    """How many slots of `base`'s chain the game folder already fills: -1 none, 0 the head only,
    n the head and generations 00..n-1 (the engine stops at the first gap, so do we)."""
    if game_data is None or not game_data.is_dir():
        return -1
    names = {p.name.lower() for p in game_data.iterdir()}
    head = "%s.%sod" % (base.lower(), chain)
    if head not in names:
        return -1
    n = 0
    while ("%s.%s%02d" % (base.lower(), chain, n)) in names:
        n += 1
    return n


def slot_name(base, chain, slot):
    """Slot -1 is the head (.mod/.nod), then .m00 .. .m09, .m10 ... as MountDir formats them."""
    if slot < 0:
        return "%s.%sod" % (base, chain)
    return "%s.%s%02d" % (base, chain, slot)


def strings_base(game_data):
    """The archive Sefaris takes the game's string table from: the first Strings* of Data in NTFS
    order (names compared in upper case), or None."""
    if game_data is None or not game_data.is_dir():
        return None
    for p in sorted(game_data.iterdir(), key=lambda p: p.name.upper()):
        if p.name.lower().startswith("strings") and p.is_file():
            return p
    return None


def static_dir(spec):
    """The Sefaris catalog's Static/ for this contract, or None."""
    candidates = []
    if isinstance(spec.get("static"), str) and os.path.isabs(spec["static"]):
        candidates.append(Path(spec["static"]))
    for mod in spec["mods"]:
        if isinstance(mod, dict) and isinstance(mod.get("path"), str) and os.path.isabs(mod["path"]):
            candidates.append(Path(mod["path"]).parent.parent / "Static")
            break
    for c in candidates:
        if (c / "Projects_compiled.m0x").is_file():
            return c
    return None


def merge_strings(base, tables):
    """strings.mod's stringtable.ini: the game's (from the archive `base`) and the mods' tables."""
    text = g3pak.Pak(base).read(STRINGTABLE).decode("utf-16-le").replace("[LocAdmin_Revisions]", "", 1)
    for table in tables:
        part = table.read_bytes().decode("utf-16").replace("[LocAdmin_Strings]", "", 1)
        if not text.endswith("\n"):
            text += "\r\n"
        text += part
    if not text.endswith("\n"):
        text += "\r\n"
    return (text + "[LocAdmin_Revisions]\r\n").encode("utf-16-le")


def merge_sectors(base, lists):
    """G3_World_01.wrldatasc: Static's sector list and every mod's sectors after it."""
    text = base.read_bytes().decode("utf-8")
    for sectors in lists:
        part = sectors.read_bytes().decode("utf-8")
        at = part.find("[Sector.List]")
        part = part[at + len("[Sector.List]"):] if at >= 0 else part
        if not text.endswith("\n"):
            text += "\r\n"
        text += part.lstrip("\r\n")
    return text.encode("utf-8")


def plan(profile, game_data):
    """The view for <profile>/active-mods.json: ([(guest name, target, mod id)], [skipped], [errors],
    [files to generate]). A generated file is a dict with its name in <profile>/mods-generated, its
    kind ("strings" or "sectors"), the base it starts from and the mods' inputs in load order."""
    contract = profile / CONTRACT
    if not contract.exists():
        return [], [], [], []
    try:
        spec = json.loads(contract.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return [], [], ["%s: %s" % (contract, e)], []
    if not isinstance(spec, dict) or spec.get("version") != 1 or not isinstance(spec.get("mods"), list):
        return [], [], ["%s: not version 1 with a mods list" % contract], []
    inis = {}  # name lower -> (name, path, mod id), the last mod's
    chains = {}  # (chain, base lower) -> [(base spelling, path, mod id)] in load order
    tables, sectors = [], []  # [(mod id, path)] in load order
    skipped, errors = [], []
    used = 0
    for i, mod in enumerate(spec["mods"]):
        mid = str(mod.get("id") or "#%d" % i) if isinstance(mod, dict) else "#%d" % i
        path = mod.get("path") if isinstance(mod, dict) else None
        folder = Path(path) if isinstance(path, str) and os.path.isabs(path) else None
        if folder is None or not folder.is_dir():
            errors.append("%s: folder %r not found, mod left out" % (mid, path))
            continue
        used += 1
        files = []
        for name, full, sub in mod_payload(folder):
            mapped = not sub or sub.lower() == "data" or (sub.lower() == "static" and name.lower() == WRLDATASC.lower())
            chain, info = classify(name) if mapped else (None, None)
            if not mapped:
                if sub.lower() == "scripts" or os.path.splitext(name)[1].lower() in PROGRAM:
                    reason = "script plugin (%s/%s): needs translating, not loaded" % (sub, name)
                else:
                    reason = "in %s/: not a folder the view maps" % sub
                skipped.append({"mod": mid, "file": str(full), "reason": reason})
                continue
            if chain is None:
                skipped.append({"mod": mid, "file": str(full), "reason": info})
                continue
            if chain == "ini":
                inis[info[0].lower()] = (info[0], full, mid)  # a later mod's ini wins
                continue
            if chain == "strings":
                tables.append((mid, full))
                continue
            if chain == "sectors":
                sectors.append((mid, full))
                continue
            files.append((chain, info[0], info[1], full))
        # Within one mod, its own order: head, then the numbers it asked for, placeholders last.
        for chain, base, _, full in sorted(files, key=lambda f: (f[0], f[1].lower(), f[2], f[3].name)):
            chains.setdefault((chain, base.lower()), []).append((base, full, mid))
    generated = []
    if used:
        gen = profile / GENERATED
        base = strings_base(game_data)
        if base is None:
            errors.append("no Strings* archive in %s: strings.mod not built, the mods' string tables "
                          "left out" % game_data)
        else:
            chains.setdefault(("m", "strings"), []).append(("strings", gen / "strings.mod", "(merged)"))
            generated.append({"file": "strings.mod", "kind": "strings", "base": str(base),
                              "inputs": [str(p) for _, p in tables]})
        static = static_dir(spec)
        if static is None:
            for mid, p in sectors:
                skipped.append({"mod": mid, "file": str(p), "reason": "no Static/ folder to merge world sectors into"})
            skipped.append({"mod": "(static)", "file": "Static/", "reason": (
                "no Sefaris Static/ folder (\"static\" in %s or beside the mods' folder): the world "
                "sector list and the archives that hide the compiled caches left out" % CONTRACT)})
        else:
            pc = chains.setdefault(("m", "projects_compiled"), [])
            pc.append(("Projects_compiled", gen / "projects_compiled.mod", "(merged)"))
            pc.append(("Projects_compiled", static / "Projects_compiled.m0x", "(static)"))
            if (static / "Projects_compiled.n0x").is_file():
                chains.setdefault(("n", "projects_compiled"), []).append(
                    ("Projects_compiled", static / "Projects_compiled.n0x", "(static)"))
            generated.append({"file": "projects_compiled.mod", "kind": "sectors",
                              "base": str(static / WRLDATASC), "inputs": [str(p) for _, p in sectors]})
    links = []
    for (chain, base_lower), entries in sorted(chains.items()):
        slot = game_chain(game_data, entries[0][0], chain)
        for base, full, mid in entries:
            # Sefaris writes the stem in lower case (x.mod, x.m00); the guest does not care.
            links.append(("Data/" + slot_name(base.lower(), chain, slot), str(full.resolve()), mid))
            slot += 1
    for name, full, mid in sorted(inis.values()):
        links.append(("ini/" + name, str(full.resolve()), mid))
    return links, skipped, errors, generated


def read_record(profile):
    try:
        return json.loads((profile / RECORD).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def clear(profile, generated=True):
    """Remove the links the last build recorded (only while they are still links) and, unless
    generated is False, the files it generated."""
    record = read_record(profile)
    removed = 0
    for link in record.get("links", []):
        p = profile / link["path"]
        if p.is_symlink():
            p.unlink()
            removed += 1
    if generated:
        gen = profile / GENERATED
        for g in record.get("generated", []):
            p = gen / Path(g["file"]).name
            if p.is_file() and not p.is_symlink():
                p.unlink()
        if gen.is_dir() and not any(gen.iterdir()):
            gen.rmdir()
    rec = profile / RECORD
    if rec.exists():
        rec.unlink()
    return removed


def inputs_signature(g):
    """What a generated file depends on: its kind, then base and inputs with their sizes and times
    in whole seconds, one per line (the launcher writes the same text, launcher_mods.cpp)."""
    parts = [g["kind"]]
    for p in [g["base"]] + g["inputs"]:
        st = os.stat(p)
        parts.append("%s %d %d" % (p, st.st_size, int(st.st_mtime)))
    return "\n".join(parts)


def generate(profile, g, previous):
    """Write one generated file unless the last build made it from the same inputs. Returns the
    record entry."""
    out = profile / GENERATED / g["file"]
    sig = inputs_signature(g)
    old = previous.get(g["file"])
    if old and old.get("signature") == sig and out.is_file() and not out.is_symlink():
        return old
    paths = [Path(g["base"])] + [Path(p) for p in g["inputs"]]
    newest = max(int(p.stat().st_mtime) for p in paths)
    filetime = newest * 10000000 + 116444736000000000
    if g["kind"] == "strings":
        entries = {STRINGTABLE: merge_strings(paths[0], paths[1:])}
    else:
        entries = {"G3_World_01/" + WRLDATASC: merge_sectors(paths[0], paths[1:])}
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".tmp")
    g3pak.write(tmp, entries, filetime)
    os.replace(tmp, out)
    return {"file": g["file"], "kind": g["kind"], "signature": sig, "inputs": g["inputs"]}


def build(profile, game_data):
    if not (profile / CONTRACT).exists():
        # No mods picked: take away what an earlier build made, and write nothing into a profile
        # that never had a view (the player's own profile stays as the game left it).
        clear(profile)
        return {"version": 1, "links": [], "skipped": [], "errors": [], "generated": []}
    links, skipped, errors, todo = plan(profile, game_data)
    previous = {g["file"]: g for g in read_record(profile).get("generated", [])}
    clear(profile, generated=False)
    made_files = []
    for g in todo:
        try:
            made_files.append(generate(profile, g, previous))
        except (OSError, ValueError, UnicodeError, KeyError, SystemExit) as e:
            errors.append("%s not generated: %s" % (g["file"], e))
            gen_target = str((profile / GENERATED / g["file"]).resolve())
            links = [l for l in links if l[1] != gen_target]
    for name in set(previous) - {g["file"] for g in made_files}:
        stale = profile / GENERATED / name
        if stale.is_file() and not stale.is_symlink():
            stale.unlink()
    made = []
    for guest, target, mid in links:
        p = profile / guest
        p.parent.mkdir(parents=True, exist_ok=True)
        if p.is_symlink():
            p.unlink()
        elif p.exists():
            errors.append("%s exists in the profile and is not a link from a build: left alone, "
                          "mod %s not linked there" % (p, mid))
            continue
        os.symlink(target, p)
        made.append({"path": guest, "target": target, "mod": mid})
    record = {"version": 1, "links": made, "generated": made_files, "skipped": skipped, "errors": errors}
    (profile / RECORD).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return record


def game_data_dir(arg=None):
    if arg:
        return Path(arg)
    root = os.environ.get("G3_ROOT") or Path(__file__).resolve().parents[1]
    return Path(root) / "stage" / "Data"


def selftest():
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        game = t / "game" / "Data"
        game.mkdir(parents=True)
        for n in ("_compiledImage.pak", "_compiledMesh.pak", "_compiledMesh.p00", "Infos.pak",
                  "Infos.mod", "Infos.m00"):
            (game / n).write_bytes(b"")
        # The game's string table as CP 1.75's Strings.pak holds it: BOM, revisions header last.
        g3pak.write(game / "Strings.pak", {STRINGTABLE: "﻿[LocAdmin_Strings]\r\nK=g\r\n[LocAdmin_Revisions]\r\n"
                                           .encode("utf-16-le")})
        a, b = t / "A" / "ModFiles", t / "B" / "ModFiles"
        (a / "scripts").mkdir(parents=True)
        (b).mkdir(parents=True)
        for n in ("_compiledImage.m0x", "_compiledMesh.m0x", "Infos.m0x", "readme.txt",
                  "Projects_compiled.p0x", "ge3.ini", "Camera.ini", "stringtableExpress.ini"):
            (a / n).write_bytes(b"a")
        (a / "stringtable.ini").write_bytes("[LocAdmin_Strings]\r\nA=a".encode("utf-16"))  # no last line end
        (a / "scripts" / "Script_X.dll").write_bytes(b"MZ")
        (b / "Static").mkdir()
        (b / "Static" / WRLDATASC).write_bytes(b"[Sector.List]\r\nSector=b\r\n")
        (b / "StringTable.ini").write_bytes("[LocAdmin_Strings]\r\nB=b\r\n".encode("utf-16"))
        for n in ("_compiledImage.m0x", "_CompiledImage.m01", "Templates.nod", "camera.ini"):
            (b / n).write_bytes(b"b")
        prof = t / "profile"
        prof.mkdir()
        (prof / CONTRACT).write_text(json.dumps({"version": 1, "preset": None, "mods": [
            {"id": "a", "path": str(t / "A")}, {"id": "missing", "path": str(t / "nope")},
            {"id": "b", "path": str(t / "B")}]}))
        rec = build(prof, game)
        got = {l["path"]: (l["mod"], Path(l["target"]).name) for l in rec["links"]}
        want = {
            "Data/_compiledimage.mod": ("a", "_compiledImage.m0x"),
            "Data/_compiledimage.m00": ("b", "_CompiledImage.m01"),
            "Data/_compiledimage.m01": ("b", "_compiledImage.m0x"),
            "Data/_compiledmesh.mod": ("a", "_compiledMesh.m0x"),
            "Data/infos.m01": ("a", "Infos.m0x"),
            "Data/templates.nod": ("b", "Templates.nod"),
            "Data/strings.mod": ("(merged)", "strings.mod"),
            "ini/camera.ini": ("b", "camera.ini"),
        }
        assert got == want, got
        assert all((prof / p).is_symlink() for p in want)
        reasons = sorted(Path(s["file"]).name for s in rec["skipped"])
        assert reasons == ["G3_World_01.wrldatasc", "Projects_compiled.p0x", "Script_X.dll", "Static", "ge3.ini",
                           "readme.txt", "stringtableExpress.ini"], reasons
        assert len(rec["errors"]) == 1 and "missing" in rec["errors"][0], rec["errors"]
        merged = g3pak.Pak(prof / "Data/strings.mod").read("stringtable.ini").decode("utf-16-le")
        assert merged == "﻿[LocAdmin_Strings]\r\nK=g\r\n\r\n\r\nA=a\r\n\r\nB=b\r\n[LocAdmin_Revisions]\r\n", repr(merged)
        # The same inputs again: strings.mod is not rewritten.
        stamp = (prof / GENERATED / "strings.mod").stat().st_mtime_ns
        build(prof, game)
        assert (prof / GENERATED / "strings.mod").stat().st_mtime_ns == stamp
        # With the catalog's Static/: sectors merged and packed, the cache-hiding archives last.
        static = t / "Static"
        static.mkdir()
        (static / WRLDATASC).write_bytes(b"[Sector.List]\r\nSector=base\r\n")
        g3pak.write(static / "Projects_compiled.m0x", {"stringtable.bin": None})
        g3pak.write(static / "Projects_compiled.n0x", {"stringtable.bin": None})
        (b / "Projects_compiled.m0x").write_bytes(b"b")
        spec = json.loads((prof / CONTRACT).read_text())
        spec["static"] = str(static)
        (prof / CONTRACT).write_text(json.dumps(spec))
        rec = build(prof, game)
        got = {l["path"]: (l["mod"], Path(l["target"]).name) for l in rec["links"] if "projects" in l["path"]}
        assert got == {"Data/projects_compiled.mod": ("b", "Projects_compiled.m0x"),
                       "Data/projects_compiled.m00": ("(merged)", "projects_compiled.mod"),
                       "Data/projects_compiled.m01": ("(static)", "Projects_compiled.m0x"),
                       "Data/projects_compiled.nod": ("(static)", "Projects_compiled.n0x")}, got
        sectors = g3pak.Pak(prof / "Data/projects_compiled.m00").read("g3_world_01/g3_world_01.wrldatasc")
        assert sectors == b"[Sector.List]\r\nSector=base\r\nSector=b\r\n", sectors
        assert not rec["errors"][1:], rec["errors"]
        (b / "Projects_compiled.m0x").unlink()
        spec["static"] = None
        (prof / CONTRACT).write_text(json.dumps(spec))
        # A regular file in the way stays and is reported; a rebuild replaces only links.
        (prof / "Data/_compiledimage.mod").unlink()
        (prof / "Data/_compiledimage.mod").write_bytes(b"player")
        rec = build(prof, game)
        assert (prof / "Data/_compiledimage.mod").read_bytes() == b"player"
        assert any("not a link" in e for e in rec["errors"])
        # No contract: the view goes away, the player's file stays.
        (prof / CONTRACT).unlink()
        build(prof, game)
        assert not (prof / "Data/_compiledmesh.mod").exists()
        assert (prof / "Data/_compiledimage.mod").read_bytes() == b"player"
        assert not (prof / RECORD).exists() and not (prof / GENERATED).exists()
        # A profile that never had a view gets no record either.
        fresh = t / "fresh"
        fresh.mkdir()
        build(fresh, game)
        assert list(fresh.iterdir()) == []
    print("g3mods selftest: ok")


def main(argv):
    if len(argv) >= 1 and argv[0] == "selftest":
        selftest()
        return 0
    if len(argv) < 2 or argv[0] not in ("build", "plan", "clear"):
        print(__doc__, file=sys.stderr)
        return 2
    profile = Path(argv[1]).resolve()
    game = game_data_dir(argv[2] if len(argv) > 2 else None)
    if argv[0] == "clear":
        print("g3mods: removed %d links" % clear(profile))
        return 0
    if argv[0] == "plan":
        links, skipped, errors, generated = plan(profile, game)
        print(json.dumps({"links": [{"path": g, "target": t, "mod": m} for g, t, m in links],
                          "generated": generated, "skipped": skipped, "errors": errors}, indent=2))
        return 0
    rec = build(profile, game)
    for g in rec["generated"]:
        print("g3mods: generated %s/%s from %d mod files" % (GENERATED, g["file"], len(g["inputs"])))
    for l in rec["links"]:
        print("g3mods: %s -> %s (%s)" % (l["path"], l["target"], l["mod"]))
    for s in rec["skipped"]:
        print("g3mods: left out %s (%s): %s" % (s["file"], s["mod"], s["reason"]))
    for e in rec["errors"]:
        print("g3mods: error: %s" % e, file=sys.stderr)
    return 1 if rec["errors"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
