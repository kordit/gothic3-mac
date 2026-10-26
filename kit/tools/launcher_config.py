"""game.toml keys only the launcher reads: the game downloaded from the
player's Steam account and the modules made from the player's files (Gothic 3,
D-037). Kept out of game_config.py, which the translation's fingerprint
digests: a change here must not translate every module again. build.py drops
these keys from game.toml's digest for the same reason (LAUNCHER_ONLY_KEYS)."""


def load_steam(launcher, source):
    """[launcher.steam]: DepotDownloader beside the app's executable. tool is
    its file name ("" = no Steam download); app the Steam app id; each
    [[depots]] entry an id, a manifest id (a string: some TOML readers check
    integers against 63 bits), the branch it belongs to ("" = public), files
    (DepotDownloader -filelist lines, "regex:" ones too; [] = all), mb (its
    size, for the progress bar) and label: a depot the player can leave out,
    named so ("" = always downloaded). Returns the table with defaults."""
    steam = dict(launcher.get("steam", {}))
    tool = steam.setdefault("tool", "")
    if not isinstance(tool, str) or "/" in tool:
        raise ValueError("%s: [launcher.steam] tool must be a file name, not %r" % (source, tool))
    # Where the app's build gets the tool: a release archive, its pin, its source (finish_bundle.py).
    for key in ("tool_url", "tool_sha256", "tool_source"):
        if not isinstance(steam.setdefault(key, ""), str):
            raise ValueError("%s: [launcher.steam] %s must be a string" % (source, key))
    if tool and (not steam["tool_url"] or len(steam["tool_sha256"]) != 64):
        raise ValueError("%s: [launcher.steam] tool needs tool_url and tool_sha256" % source)
    app = steam.setdefault("app", 0)
    if type(app) is not int or app < 0 or (tool and not app):
        raise ValueError("%s: [launcher.steam] app must be a Steam app id" % source)
    depots = [dict(d) if isinstance(d, dict) else d for d in steam.get("depots", [])]
    if tool and not depots:
        raise ValueError("%s: [launcher.steam] depots must list what to download" % source)
    for d in depots:
        if (not isinstance(d, dict) or type(d.get("id")) is not int or d["id"] <= 0
                or not isinstance(d.setdefault("manifest", ""), str) or not d["manifest"].isdigit()):
            raise ValueError("%s: [[launcher.steam.depots]] need an id and a manifest id string" % source)
        for key in ("branch", "label"):
            if not isinstance(d.setdefault(key, ""), str):
                raise ValueError("%s: [[launcher.steam.depots]] %s must be a string" % (source, key))
        for key in ("files", "ini_on", "ini_off"):
            value = d.setdefault(key, [])
            if not isinstance(value, list) or not all(isinstance(f, str) and "\n" not in f for f in value):
                raise ValueError("%s: [[launcher.steam.depots]] %s must be a list of strings" % (source, key))
            if key != "files":
                check_ini_edits(value, source)
        if type(d.setdefault("mb", 0)) is not int or d["mb"] < 0:
            raise ValueError("%s: [[launcher.steam.depots]] mb must be a non-negative integer" % source)
    steam["depots"] = depots
    ini = steam.setdefault("ini", [])
    if not isinstance(ini, list) or not all(isinstance(e, str) and "\n" not in e for e in ini):
        raise ValueError("%s: [launcher.steam] ini must be a list of strings" % source)
    check_ini_edits(ini, source)
    return steam


def check_ini_edits(edits, source):
    """launcher_stage.h apply_ini_edits: "file|section|key=value", "file||+line" or "file||-line"."""
    for e in edits:
        parts = e.split("|", 2)
        if (len(parts) != 3 or not parts[0] or parts[0].startswith("/") or ".." in parts[0].split("/")
                or (parts[1] and "=" not in parts[2][1:]) or (not parts[1] and parts[2][:1] not in ("+", "-"))):
            raise ValueError('%s: [launcher.steam] ini edit %r is not "file|section|key=value" or "file||+line"'
                             % (source, e))


def load_profile_seed(launcher, source):
    """[launcher] profile_seed: "game file|profile file" pairs the launcher copies into the player's
    profile after preparing the game, when the profile has none yet (Gothic 3: the install's
    Shader.Cache, without which the first start compiles thousands of shaders)."""
    seeds = launcher.get("profile_seed", [])
    if not isinstance(seeds, list):
        raise ValueError("%s: [launcher] profile_seed must be a list" % source)
    for s in seeds:
        parts = s.split("|") if isinstance(s, str) else []
        if (len(parts) != 2 or not all(parts) or any(p.startswith("/") or ".." in p.split("/") for p in parts)):
            raise ValueError('%s: [launcher] profile_seed entry %r is not "game file|profile file"' % (source, s))
    return seeds


def aux_stage(entry, source, key):
    """How the launcher makes a [modules.aux.<key>] module from the player's
    files: from (the install's file, '/' separated), from_sha256 (its pin) and
    prepare ("" or "cab:<member>", a file of a cabinet). The result is that
    file rebased to the module's base by its own .reloc, and must hash to the
    module's sha256. None when the table has no from: the player's file is
    used as it is."""
    if "from" not in entry:
        return None
    stage = {"from": entry["from"], "from_sha256": entry.get("from_sha256", ""),
             "prepare": entry.get("prepare", "")}
    if (not isinstance(stage["from"], str) or not stage["from"] or stage["from"].startswith("/")
            or ".." in stage["from"].split("/") or not isinstance(stage["from_sha256"], str)
            or len(stage["from_sha256"]) != 64 or not isinstance(stage["prepare"], str)
            or (stage["prepare"] and not stage["prepare"].startswith("cab:"))):
        raise ValueError('%s: [modules.aux.%s] from is a relative path with a from_sha256, prepare "" or '
                         '"cab:<member>"' % (source, key))
    return stage
