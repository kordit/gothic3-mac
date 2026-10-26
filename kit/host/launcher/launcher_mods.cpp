// launcher_mods.cpp - see launcher_mods.h.
#include "launcher_mods.h"

#include "launcher.h"

#include "../../platform/os.h"
#include "../../third_party/miniz/miniz.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace launcher {
namespace {

// ---------------------------------------------------------------------------
// A JSON reader for mod.json and preset.json: objects, arrays and strings are
// kept, numbers, booleans and null are read and dropped. Enough for the two
// files and the launcher's own; not a general parser.
// ---------------------------------------------------------------------------
struct JValue {
    enum Type { Null, Other, String, Array, Object } type = Null;
    std::string s;
    std::vector<JValue> a;
    std::vector<std::pair<std::string, JValue>> o;
    const JValue *get(const char *key) const {
        if (type != Object)
            return nullptr;
        for (const auto &m : o)
            if (m.first == key)
                return &m.second;
        return nullptr;
    }
    std::string str(const char *key) const {
        const JValue *v = get(key);
        return v && v->type == String ? v->s : std::string();
    }
    std::vector<std::string> strings(const char *key) const {
        std::vector<std::string> out;
        const JValue *v = get(key);
        if (v && v->type == Array)
            for (const JValue &e : v->a)
                if (e.type == String)
                    out.push_back(e.s);
        return out;
    }
};

struct JReader {
    const std::string &t;
    size_t i = 0;
    int depth = 0;
    explicit JReader(const std::string &text) : t(text) {}
    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r'))
            ++i;
    }
    static void utf8(std::string *out, uint32_t cp) {
        if (cp < 0x80)
            out->push_back(char(cp));
        else if (cp < 0x800) {
            out->push_back(char(0xC0 | (cp >> 6)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(char(0xE0 | (cp >> 12)));
            out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(char(0xF0 | (cp >> 18)));
            out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        }
    }
    bool hex4(uint32_t *v) {
        if (i + 4 > t.size())
            return false;
        *v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t[i++];
            *v <<= 4;
            if (c >= '0' && c <= '9')
                *v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f')
                *v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                *v |= uint32_t(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }
    bool string(std::string *out) {
        if (i >= t.size() || t[i] != '"')
            return false;
        ++i;
        while (i < t.size()) {
            const char c = t[i++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (i >= t.size())
                return false;
            const char e = t[i++];
            switch (e) {
            case 'n':
                out->push_back('\n');
                break;
            case 't':
                out->push_back('\t');
                break;
            case 'r':
                out->push_back('\r');
                break;
            case 'b':
                out->push_back('\b');
                break;
            case 'f':
                out->push_back('\f');
                break;
            case 'u': {
                uint32_t cp;
                if (!hex4(&cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < t.size() && t[i] == '\\' &&
                    t[i + 1] == 'u') {
                    i += 2;
                    uint32_t lo;
                    if (!hex4(&lo))
                        return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default:
                out->push_back(e);
                break;
            }
        }
        return false;
    }
    bool value(JValue *v) {
        ws();
        if (i >= t.size() || ++depth > 64)
            return false;
        bool ok = true;
        const char c = t[i];
        if (c == '"') {
            v->type = JValue::String;
            ok = string(&v->s);
        } else if (c == '[') {
            v->type = JValue::Array;
            ++i;
            ws();
            if (i < t.size() && t[i] == ']')
                ++i;
            else
                for (;;) {
                    v->a.emplace_back();
                    if (!value(&v->a.back())) {
                        ok = false;
                        break;
                    }
                    ws();
                    if (i < t.size() && t[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < t.size() && t[i] == ']') {
                        ++i;
                        break;
                    }
                    ok = false;
                    break;
                }
        } else if (c == '{') {
            v->type = JValue::Object;
            ++i;
            ws();
            if (i < t.size() && t[i] == '}')
                ++i;
            else
                for (;;) {
                    ws();
                    std::string key;
                    if (!string(&key)) {
                        ok = false;
                        break;
                    }
                    ws();
                    if (i >= t.size() || t[i] != ':') {
                        ok = false;
                        break;
                    }
                    ++i;
                    v->o.emplace_back(key, JValue{});
                    if (!value(&v->o.back().second)) {
                        ok = false;
                        break;
                    }
                    ws();
                    if (i < t.size() && t[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < t.size() && t[i] == '}') {
                        ++i;
                        break;
                    }
                    ok = false;
                    break;
                }
        } else {
            // A number, true, false or null: kept as Other.
            const size_t start = i;
            while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '-' ||
                                    t[i] == '+' || t[i] == '.'))
                ++i;
            ok = i > start;
            const std::string word = t.substr(start, i - start);
            v->type = word == "null" ? JValue::Null : JValue::Other;
            v->s = word; // a number's text, true or false
        }
        --depth;
        return ok;
    }
};

bool parse_json(std::string text, JValue *out) {
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) // a BOM, as Windows editors write
        text.erase(0, 3);
    JReader r(text);
    if (!r.value(out))
        return false;
    r.ws();
    return r.i == text.size();
}

void json_string(std::string *out, const std::string &s) {
    out->push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            *out += "\\\"";
            break;
        case '\\':
            *out += "\\\\";
            break;
        case '\n':
            *out += "\\n";
            break;
        case '\r':
            *out += "\\r";
            break;
        case '\t':
            *out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", c);
                *out += buf;
            } else
                out->push_back(char(c));
        }
    }
    out->push_back('"');
}

std::string read_text(const std::string &path) {
    std::string out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return out;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    fclose(f);
    return out;
}

bool write_text(const std::string &path, const std::string &text) {
    const std::string part = path + ".part";
    FILE *f = fopen(part.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f) != 0 || !ok) {
        os_unlink(part.c_str());
        return false;
    }
    return os_rename(part.c_str(), path.c_str()) == 0;
}

std::vector<std::string> list_dir(const std::string &dir) {
    std::vector<std::string> names;
    os_listdir(
        dir.c_str(),
        [](const char *name, void *user) -> int {
            if (strcmp(name, ".") && strcmp(name, ".."))
                static_cast<std::vector<std::string> *>(user)->push_back(name);
            return 0;
        },
        &names);
    return names;
}

bool is_dir(const std::string &path) {
    OsStat st{};
    return os_stat(path.c_str(), &st) == 0 && st.is_dir;
}

std::string lower(std::string s) {
    for (char &c : s)
        c = char(std::tolower((unsigned char)c));
    return s;
}

// The first Script_*.dll under `dir` (relative to `base`), searched depth first.
std::string find_script_dll(const std::string &dir, const std::string &rel, int depth) {
    if (depth > 8)
        return "";
    std::vector<std::string> names = list_dir(dir);
    std::sort(names.begin(), names.end());
    for (const std::string &n : names) {
        const std::string path = dir + "/" + n;
        const std::string relative = rel.empty() ? n : rel + "/" + n;
        if (is_dir(path)) {
            const std::string found = find_script_dll(path, relative, depth + 1);
            if (!found.empty())
                return found;
        } else if (wildcard_match("script_*.dll", n.c_str())) {
            return relative;
        }
    }
    return "";
}

std::string join(const std::vector<std::string> &v) {
    std::string out;
    for (const std::string &s : v)
        out += (out.empty() ? "" : ", ") + s;
    return out;
}

bool contains_id(const std::vector<std::string> &v, const std::string &id) {
    for (const std::string &s : v)
        if (same_id(s, id))
            return true;
    return false;
}

} // namespace

bool same_id(const std::string &a, const std::string &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

int ModCatalog::find(const std::string &id) const {
    for (size_t i = 0; i < mods.size(); ++i)
        if (same_id(mods[i].id, id))
            return int(i);
    return -1;
}

std::vector<std::string> ModCatalog::categories() const {
    std::vector<std::string> out;
    bool uncategorized = false;
    for (const ModInfo &m : mods) {
        if (m.category.empty()) {
            uncategorized = true;
            continue;
        }
        if (std::find(out.begin(), out.end(), m.category) == out.end())
            out.push_back(m.category);
    }
    std::sort(out.begin(), out.end(),
              [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
    if (uncategorized)
        out.push_back("Uncategorized");
    return out;
}

std::string default_mod_root(const std::string &configured) {
    if (const char *env = recomp_env("SEFARIS_DIR"); env && *env)
        return env;
    const char *home = getenv("HOME");
    if (configured.compare(0, 2, "~/") == 0 && home && *home)
        return std::string(home) + configured.substr(1);
    return configured;
}

bool mod_dir_before(const std::string &a, const std::string &b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char x = (unsigned char)std::toupper((unsigned char)a[i]);
        const unsigned char y = (unsigned char)std::toupper((unsigned char)b[i]);
        if (x != y)
            return x < y;
    }
    if (a.size() != b.size())
        return a.size() < b.size();
    return a < b; // names equal but for case: any fixed order
}

void mod_load_order(std::vector<std::string> *dirs) {
    std::sort(dirs->begin(), dirs->end(), mod_dir_before);
}

ModCatalog load_mod_catalog(const std::string &root) {
    ModCatalog cat;
    cat.root = root;
    const std::string mods_dir = root + "/Mods";
    cat.found = is_dir(mods_dir);
    if (!cat.found)
        return cat;
    std::vector<std::string> dirs;
    for (const std::string &n : list_dir(mods_dir))
        if (is_dir(mods_dir + "/" + n))
            dirs.push_back(n);
    mod_load_order(&dirs);
    for (const std::string &d : dirs) {
        ModInfo m;
        m.dir = d;
        m.path = mods_dir + "/" + d;
        const std::string json_name = find_name(m.path, "mod.json");
        JValue j;
        if (json_name.empty() || !parse_json(read_text(m.path + "/" + json_name), &j) ||
            j.type != JValue::Object) {
            cat.errors.push_back(d + ": " + (json_name.empty() ? "no mod.json" : "mod.json does not parse"));
            continue;
        }
        m.id = j.str("id");
        if (m.id.empty()) {
            cat.errors.push_back(d + ": mod.json has no id");
            continue;
        }
        if (cat.find(m.id) >= 0) {
            cat.errors.push_back(d + ": id " + m.id + " is used by an earlier folder");
            continue;
        }
        m.title = j.str("title");
        if (m.title.empty())
            m.title = m.id;
        m.category = j.str("category");
        m.dependencies = j.strings("dependencies");
        m.incompatibles = j.strings("incompatibles");
        m.authors = j.strings("authors");
        const std::string images_name = find_name(m.path, "images");
        if (!images_name.empty()) {
            const std::string images = m.path + "/" + images_name;
            const std::string named = find_name(images, "315x312.jpg");
            if (!named.empty())
                m.cover = images + "/" + named;
            else {
                std::vector<std::string> names = list_dir(images);
                std::sort(names.begin(), names.end());
                for (const std::string &n : names)
                    if (wildcard_match("*.jpg", n.c_str()) || wildcard_match("*.jpeg", n.c_str())) {
                        m.cover = images + "/" + n;
                        break;
                    }
            }
        }
        const std::string readme_name = find_name(m.path, "Readme");
        if (!readme_name.empty()) {
            const std::string dir = m.path + "/" + readme_name;
            for (const char *lang : {"readme_pl.md", "readme_gb.md", "readme_de.md"}) {
                const std::string n = find_name(dir, lang);
                if (!n.empty()) {
                    m.readme = dir + "/" + n;
                    break;
                }
            }
        }
        m.script_dll = find_script_dll(m.path, "", 0);
        cat.mods.push_back(std::move(m));
    }
    const std::string presets_name = find_name(root, "presets");
    if (!presets_name.empty()) {
        const std::string presets = root + "/" + presets_name;
        std::vector<std::string> names = list_dir(presets);
        std::sort(names.begin(), names.end(),
                  [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
        for (const std::string &n : names) {
            const std::string dir = presets + "/" + n;
            const std::string file = is_dir(dir) ? find_name(dir, "preset.json") : std::string();
            if (file.empty())
                continue;
            JValue j;
            if (!parse_json(read_text(dir + "/" + file), &j) || j.type != JValue::Object) {
                cat.errors.push_back("presets/" + n + ": preset.json does not parse");
                continue;
            }
            ModPreset p;
            p.dir = dir;
            p.name = j.str("name");
            if (p.name.empty())
                p.name = n;
            p.mod_ids = j.strings("modIds");
            cat.presets.push_back(std::move(p));
        }
    }
    return cat;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------
bool ModSelection::enabled(const std::string &id) const {
    return contains_id(ids, id);
}

std::string mod_blocked(const ModCatalog &cat, const ModSelection &sel, const std::string &id) {
    const int i = cat.find(id);
    if (i < 0)
        return "nie ma go w katalogu";
    const ModInfo &m = cat.mods[size_t(i)];
    if (!m.supported())
        return "jeszcze nieobsługiwany (" + m.script_dll + ")";
    std::vector<std::string> by;
    for (const std::string &other : sel.ids) {
        const int j = cat.find(other);
        if (j < 0 || same_id(other, id))
            continue;
        const ModInfo &o = cat.mods[size_t(j)];
        if (contains_id(m.incompatibles, o.id) || contains_id(o.incompatibles, m.id))
            by.push_back(o.title);
    }
    if (!by.empty())
        return "zablokowany przez: " + join(by);
    return "";
}

std::string mod_required_by(const ModCatalog &cat, const ModSelection &sel, const std::string &id) {
    std::vector<std::string> by;
    for (const std::string &other : sel.ids) {
        const int j = cat.find(other);
        if (j < 0 || same_id(other, id))
            continue;
        if (contains_id(cat.mods[size_t(j)].dependencies, id))
            by.push_back(cat.mods[size_t(j)].title);
    }
    return by.empty() ? "" : "zależne od niego: " + join(by);
}

std::string mod_toggle(const ModCatalog &cat, ModSelection *sel, const std::string &id) {
    if (sel->enabled(id)) {
        const std::string why = mod_required_by(cat, *sel, id);
        if (!why.empty())
            return "Nie można wyłączyć, " + why;
        sel->ids.erase(std::remove_if(sel->ids.begin(), sel->ids.end(),
                                      [&](const std::string &s) { return same_id(s, id); }),
                       sel->ids.end());
        return "";
    }
    // The mod and every dependency it pulls in, each checked against what is
    // enabled and what this click enables; nothing changes when one fails.
    ModSelection next = *sel;
    std::vector<std::string> todo{id};
    while (!todo.empty()) {
        const std::string cur = todo.back();
        todo.pop_back();
        if (next.enabled(cur))
            continue;
        const int i = cat.find(cur);
        if (i < 0)
            return "Nie znaleziono zależności: " + cur;
        const std::string why = mod_blocked(cat, next, cur);
        if (!why.empty())
            return same_id(cur, id) ? "Nie można włączyć, " + why
                                    : "Zależność " + cat.mods[size_t(i)].title + ": " + why;
        next.ids.push_back(cat.mods[size_t(i)].id);
        for (const std::string &d : cat.mods[size_t(i)].dependencies)
            todo.push_back(d);
    }
    sel->ids = next.ids;
    return "";
}

void mod_apply_preset(const ModCatalog &cat, ModSelection *sel, const ModPreset &preset,
                      std::vector<std::string> *missing, std::vector<std::string> *unsupported) {
    sel->ids.clear();
    sel->preset = preset.name;
    for (const std::string &id : preset.mod_ids) {
        const int i = cat.find(id);
        if (i < 0) {
            if (missing)
                missing->push_back(id);
            continue;
        }
        if (!cat.mods[size_t(i)].supported()) {
            if (unsupported)
                unsupported->push_back(cat.mods[size_t(i)].title);
            continue;
        }
        if (!sel->enabled(id))
            sel->ids.push_back(cat.mods[size_t(i)].id);
    }
}

std::vector<ModProblem> mod_problems(const ModCatalog &cat, const ModSelection &sel) {
    std::vector<ModProblem> out;
    for (const std::string &id : sel.ids) {
        const int i = cat.find(id);
        if (i < 0) {
            out.push_back({id, id + ": nie ma go w katalogu", false});
            continue;
        }
        const ModInfo &m = cat.mods[size_t(i)];
        if (!m.supported())
            out.push_back({m.id, m.title + ": jeszcze nieobsługiwany", false});
        std::vector<std::string> missing;
        for (const std::string &d : m.dependencies)
            if (!sel.enabled(d))
                missing.push_back(cat.find(d) >= 0 ? cat.mods[size_t(cat.find(d))].title
                                                   : d + " (brak w katalogu)");
        if (!missing.empty())
            out.push_back({m.id, m.title + " wymaga: " + join(missing), false});
        for (const std::string &other : sel.ids) {
            const int j = cat.find(other);
            if (j <= i) // each pair once
                continue;
            const ModInfo &o = cat.mods[size_t(j)];
            if (contains_id(m.incompatibles, o.id) || contains_id(o.incompatibles, m.id))
                out.push_back({m.id, m.title + " gryzie się z " + o.title, true});
        }
    }
    return out;
}

std::vector<const ModInfo *> mod_active(const ModCatalog &cat, const ModSelection &sel) {
    std::vector<const ModInfo *> out;
    for (const ModInfo &m : cat.mods) // already in load order
        if (sel.enabled(m.id) && m.supported())
            out.push_back(&m);
    return out;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
bool load_mod_choice(const std::string &file, std::string *root, ModSelection *sel) {
    const std::string text = read_text(file);
    JValue j;
    if (text.empty() || !parse_json(text, &j) || j.type != JValue::Object)
        return false;
    if (root)
        *root = j.str("root");
    sel->preset = j.str("preset");
    sel->ids = j.strings("mods");
    return true;
}

bool save_mod_choice(const std::string &file, const std::string &root, const ModSelection &sel) {
    std::string out = "{\n  \"version\": 1,\n  \"root\": ";
    json_string(&out, root);
    out += ",\n  \"preset\": ";
    if (sel.preset.empty())
        out += "null";
    else
        json_string(&out, sel.preset);
    out += ",\n  \"mods\": [";
    for (size_t i = 0; i < sel.ids.size(); ++i) {
        out += i ? ", " : "";
        json_string(&out, sel.ids[i]);
    }
    out += "]\n}\n";
    return write_text(file, out);
}

std::string active_mods_json(const ModCatalog &cat, const ModSelection &sel) {
    std::string out = "{\n  \"version\": 1,\n  \"preset\": ";
    if (sel.preset.empty())
        out += "null";
    else
        json_string(&out, sel.preset);
    OsStat st{};
    const std::string stat = cat.root + "/Static";
    if (!cat.root.empty() && os_stat((stat + "/Projects_compiled.m0x").c_str(), &st) == 0) {
        out += ",\n  \"static\": ";
        json_string(&out, stat);
    }
    out += ",\n  \"mods\": [";
    const std::vector<const ModInfo *> active = mod_active(cat, sel);
    for (size_t i = 0; i < active.size(); ++i) {
        out += i ? ",\n    {\"id\": " : "\n    {\"id\": ";
        json_string(&out, active[i]->id);
        out += ", \"path\": ";
        json_string(&out, active[i]->path);
        out += "}";
    }
    out += active.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

bool write_active_mods(const std::string &file, const ModCatalog &cat, const ModSelection &sel) {
    return write_text(file, active_mods_json(cat, sel));
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
std::string ascii_text(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out.push_back(c == '\t' ? ' ' : char(c));
            ++i;
            continue;
        }
        uint32_t cp = 0;
        int len = 0;
        if ((c & 0xE0) == 0xC0)
            cp = c & 0x1F, len = 2;
        else if ((c & 0xF0) == 0xE0)
            cp = c & 0x0F, len = 3;
        else if ((c & 0xF8) == 0xF0)
            cp = c & 0x07, len = 4;
        if (!len || i + size_t(len) > s.size()) {
            out.push_back('?');
            ++i;
            continue;
        }
        for (int k = 1; k < len; ++k)
            cp = (cp << 6) | ((unsigned char)s[i + size_t(k)] & 0x3F);
        i += size_t(len);
        static const struct {
            uint32_t cp;
            const char *ascii;
        } kMap[] = {
            {0x104, "A"}, {0x105, "a"}, {0x106, "C"}, {0x107, "c"}, {0x118, "E"}, {0x119, "e"},
            {0x141, "L"}, {0x142, "l"}, {0x143, "N"}, {0x144, "n"}, {0xD3, "O"},  {0xF3, "o"},
            {0x15A, "S"}, {0x15B, "s"}, {0x179, "Z"}, {0x17A, "z"}, {0x17B, "Z"}, {0x17C, "z"},
            {0xC4, "Ae"}, {0xE4, "ae"}, {0xD6, "Oe"}, {0xF6, "oe"}, {0xDC, "Ue"}, {0xFC, "ue"},
            {0xDF, "ss"}, {0x10C, "C"}, {0x10D, "c"}, {0x160, "S"}, {0x161, "s"}, {0x17D, "Z"},
            {0x17E, "z"}, {0x158, "R"}, {0x159, "r"}, {0x11A, "E"}, {0x11B, "e"}, {0x2018, "'"},
            {0x2019, "'"}, {0x201A, ","}, {0x201C, "\""}, {0x201D, "\""}, {0x201E, "\""},
            {0xAB, "\""}, {0xBB, "\""}, {0x2013, "-"}, {0x2014, "-"}, {0x2022, "-"},
            {0x2026, "..."}, {0xA0, " "}, {0xB7, "-"}, {0x2192, "->"}, {0xFEFF, ""},
            {0xD7, "x"}, {0xB0, " deg"}, {0xA9, "(c)"}, {0x2122, "(TM)"},
        };
        const char *mapped = nullptr;
        for (const auto &m : kMap)
            if (m.cp == cp) {
                mapped = m.ascii;
                break;
            }
        if (!mapped && cp >= 0xC0 && cp <= 0xFF) {
            // Latin-1 letters by their base letter.
            static const char kBase[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
            out.push_back(kBase[cp - 0xC0]);
            continue;
        }
        out += mapped ? mapped : "?";
    }
    return out;
}

std::vector<ReadmeLine> simplify_markdown(const std::string &text) {
    std::vector<ReadmeLine> out;
    std::string body = text;
    if (body.compare(0, 3, "\xEF\xBB\xBF") == 0)
        body.erase(0, 3);
    // <br> tags are line breaks; other tags go.
    std::string flat;
    for (size_t i = 0; i < body.size();) {
        if (body[i] == '<') {
            const size_t end = body.find('>', i);
            if (end != std::string::npos && end - i < 40) {
                const std::string tag = lower(body.substr(i + 1, end - i - 1));
                if (tag.compare(0, 2, "br") == 0 || tag == "/p" || tag == "p")
                    flat.push_back('\n');
                i = end + 1;
                continue;
            }
        }
        if (body[i] != '\r')
            flat.push_back(body[i]);
        ++i;
    }
    bool blank = true;
    for (size_t start = 0; start <= flat.size();) {
        size_t nl = flat.find('\n', start);
        if (nl == std::string::npos)
            nl = flat.size();
        std::string line = flat.substr(start, nl - start);
        start = nl + 1;
        // Trim.
        size_t a = 0;
        while (a < line.size() && (line[a] == ' ' || line[a] == '\t'))
            ++a;
        line = line.substr(a);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        ReadmeLine r;
        if (line.empty() || line == "---" || line == "***" || line == "___") {
            if (!blank)
                out.push_back({"", false});
            blank = true;
            if (nl >= flat.size())
                break;
            continue;
        }
        if (line[0] == '#') {
            size_t h = 0;
            while (h < line.size() && line[h] == '#')
                ++h;
            while (h < line.size() && line[h] == ' ')
                ++h;
            line = line.substr(h);
            r.heading = true;
        } else if (line.compare(0, 2, "- ") == 0 || line.compare(0, 2, "* ") == 0 ||
                   line.compare(0, 2, "+ ") == 0) {
            line = "- " + line.substr(2);
        } else if (line.compare(0, 3, "\xE2\x80\xA2") == 0) { // a bullet character
            size_t b = 3;
            while (b < line.size() && (line[b] == ' ' || line[b] == '\t'))
                ++b;
            line = "- " + line.substr(b);
        } else if (line[0] == '>') {
            line = line.substr(line.size() > 1 && line[1] == ' ' ? 2 : 1);
        }
        // Inline: images dropped, links to their text, emphasis and code marks gone.
        std::string clean;
        for (size_t i = 0; i < line.size();) {
            if (line[i] == '!' && i + 1 < line.size() && line[i + 1] == '[') {
                const size_t close = line.find("](", i);
                const size_t end = close == std::string::npos ? close : line.find(')', close);
                if (end != std::string::npos) {
                    i = end + 1;
                    continue;
                }
            }
            if (line[i] == '[') {
                const size_t close = line.find("](", i);
                const size_t end = close == std::string::npos ? close : line.find(')', close);
                if (end != std::string::npos) {
                    clean += line.substr(i + 1, close - i - 1);
                    i = end + 1;
                    continue;
                }
            }
            if (line[i] == '*' || line[i] == '`' ||
                (line[i] == '_' && i + 1 < line.size() && line[i + 1] == '_')) {
                i += line[i] == '_' ? 2 : 1;
                continue;
            }
            clean.push_back(line[i]);
            ++i;
        }
        r.text = clean;
        out.push_back(r);
        blank = false;
        if (nl >= flat.size())
            break;
    }
    while (!out.empty() && out.back().text.empty())
        out.pop_back();
    return out;
}

std::vector<ReadmeLine> load_readme(const std::string &path) {
    return path.empty() ? std::vector<ReadmeLine>() : simplify_markdown(read_text(path));
}

// ---------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------
bool jpeg_size(const uint8_t *d, size_t n, int *w, int *h) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return false;
    size_t i = 2;
    while (i + 4 <= n) {
        if (d[i] != 0xFF) {
            ++i;
            continue;
        }
        const uint8_t marker = d[i + 1];
        if (marker == 0xFF) {
            ++i;
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        const size_t len = size_t(d[i + 2]) << 8 | d[i + 3];
        // SOF0..SOF15 but DHT (C4), JPG (C8) and DAC (CC).
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
            marker != 0xCC) {
            if (i + 9 > n)
                return false;
            *h = int(d[i + 5]) << 8 | d[i + 6];
            *w = int(d[i + 7]) << 8 | d[i + 8];
            return *w > 0 && *h > 0;
        }
        if (marker == 0xD9 || marker == 0xDA)
            return false;
        i += 2 + len;
    }
    return false;
}

Image scale_cover(const Image &src, int w, int h) {
    Image out;
    if (src.w <= 0 || src.h <= 0 || w <= 0 || h <= 0 ||
        src.rgba.size() < size_t(src.w) * size_t(src.h) * 4)
        return out;
    out.w = w;
    out.h = h;
    out.rgba.assign(size_t(w) * size_t(h) * 4, 0);
    // The largest centred part of the source with the target's aspect.
    double cw = src.w, ch = double(src.w) * h / w;
    if (ch > src.h) {
        ch = src.h;
        cw = double(src.h) * w / h;
    }
    const double x0 = (src.w - cw) / 2, y0 = (src.h - ch) / 2;
    const double sx = cw / w, sy = ch / h;
    for (int y = 0; y < h; ++y) {
        const int ya = std::min(src.h - 1, int(y0 + y * sy));
        const int yb = std::max(ya + 1, std::min(src.h, int(y0 + (y + 1) * sy)));
        for (int x = 0; x < w; ++x) {
            const int xa = std::min(src.w - 1, int(x0 + x * sx));
            const int xb = std::max(xa + 1, std::min(src.w, int(x0 + (x + 1) * sx)));
            uint32_t acc[4] = {0, 0, 0, 0}, count = 0;
            for (int yy = ya; yy < yb; ++yy)
                for (int xx = xa; xx < xb; ++xx) {
                    const uint8_t *p = &src.rgba[(size_t(yy) * size_t(src.w) + size_t(xx)) * 4];
                    for (int k = 0; k < 4; ++k)
                        acc[k] += p[k];
                    ++count;
                }
            uint8_t *q = &out.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            for (int k = 0; k < 4; ++k)
                q[k] = uint8_t(acc[k] / count);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The mod view: tools/g3mods.py `build` in C++ (see the header). Keep the two
// in step; the tests run g3mods.py's selftest vectors.
// ---------------------------------------------------------------------------
namespace {

const char *const kProgramExt[] = {".dll", ".exe", ".asi", ".com", ".bat",
                                   ".cmd", ".scr", ".lnk", ".dylib", ".so"};

std::string ext_lower(const std::string &name) {
    // os.path.splitext: the last dot, unless the name only starts with it.
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return "";
    size_t lead = 0;
    while (lead < name.size() && name[lead] == '.')
        ++lead;
    if (dot < lead)
        return "";
    return lower(name.substr(dot));
}

bool is_program(const std::string &name) {
    const std::string ext = ext_lower(name);
    for (const char *p : kProgramExt)
        if (ext == p)
            return true;
    return false;
}

bool all_digits(const std::string &s) {
    if (s.empty())
        return false;
    for (char c : s)
        if (!std::isdigit((unsigned char)c))
            return false;
    return true;
}

// classify(): an archive's chain ('m' or 'n') with its base and order, "ini",
// or "" with the reason the file is left out.
struct Classified {
    std::string chain; // "m", "n", "ini" or ""
    std::string base;  // the archive's stem, or the ini's name
    long order = 0;
    std::string reason;
};

Classified classify(const std::string &name) {
    Classified c;
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        const std::string ext = lower(name.substr(dot + 1));
        if (ext == "mod" || ext == "nod") {
            c.chain = ext.substr(0, 1);
            c.base = name.substr(0, dot);
            c.order = -1;
            return c;
        }
        if (ext.size() >= 2 && (ext[0] == 'm' || ext[0] == 'n')) {
            const std::string rest = ext.substr(1);
            if ((rest.size() >= 2 && all_digits(rest)) || rest == "0x") {
                c.chain = ext.substr(0, 1);
                c.base = name.substr(0, dot);
                c.order = rest == "0x" ? 1000000 : std::strtol(rest.c_str(), nullptr, 10);
                return c;
            }
        }
    }
    const std::string ext = ext_lower(name);
    const std::string low = lower(name);
    if (is_program(name)) {
        c.reason = "program file: needs translating like the game's own modules, not loaded";
        return c;
    }
    if (low == "stringtable.ini") {
        c.chain = "strings";
        c.base = name;
        return c;
    }
    if (low.compare(0, 11, "stringtable") == 0 && low.size() >= 15 &&
        low.compare(low.size() - 4, 4, ".ini") == 0) {
        c.reason = "a stringtable variant: Sefaris merges only stringtable.ini and ignores this";
        return c;
    }
    if (low == "g3_world_01.wrldatasc") {
        c.chain = "sectors";
        c.base = name;
        return c;
    }
    if (low == "ge3.ini") {
        c.reason = "ge3.ini: the manager never copies it";
        return c;
    }
    if (ext == ".ini") {
        c.chain = "ini";
        c.base = name;
        return c;
    }
    // \.(pak|cpt|p\d\d+|c\d\d+)$
    if (dot != std::string::npos) {
        const std::string e = lower(name.substr(dot + 1));
        if (e == "pak" || e == "cpt" ||
            ((e[0] == 'p' || e[0] == 'c') && e.size() >= 3 && all_digits(e.substr(1)))) {
            c.reason = "a .pak/.cpt generation: would replace the game's own archive by name";
            return c;
        }
    }
    c.reason = "not an archive the game mounts from Data";
    return c;
}

struct PayloadFile {
    std::string name, path, sub;
};

void walk_files(const std::string &dir, const std::string &rel, std::vector<std::string> *out,
                int depth) {
    if (depth > 16)
        return;
    for (const std::string &n : list_dir(dir)) {
        const std::string path = dir + "/" + n, r = rel.empty() ? n : rel + "/" + n;
        OsStat st{};
        if (os_lstat(path.c_str(), &st) != 0)
            continue;
        if (st.is_dir && !st.is_symlink)
            walk_files(path, r, out, depth + 1);
        else if (os_stat(path.c_str(), &st) == 0 && st.is_regular)
            out->push_back(r);
    }
}

// Path components, for sorting as pathlib does.
std::vector<std::string> parts(const std::string &rel) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= rel.size(); ++i)
        if (i == rel.size() || rel[i] == '/') {
            out.push_back(rel.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

std::vector<PayloadFile> mod_payload(const std::string &folder) {
    std::string root = folder;
    const std::string mf = find_name(folder, "ModFiles");
    if (!mf.empty() && is_dir(folder + "/" + mf))
        root = folder + "/" + mf;
    std::vector<std::string> names = list_dir(root);
    std::stable_sort(names.begin(), names.end(),
                     [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
    std::vector<PayloadFile> out;
    for (const std::string &n : names) {
        const std::string path = root + "/" + n;
        OsStat st{};
        if (os_stat(path.c_str(), &st) != 0)
            continue;
        if (st.is_dir) {
            if (lower(n) == "data") {
                std::vector<std::string> files = list_dir(path);
                std::stable_sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
                    return lower(a) < lower(b);
                });
                for (const std::string &f : files) {
                    OsStat fs{};
                    if (os_stat((path + "/" + f).c_str(), &fs) == 0 && fs.is_regular)
                        out.push_back({f, path + "/" + f, ""});
                }
            } else {
                std::vector<std::string> files;
                walk_files(path, "", &files, 0);
                std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) {
                    return parts(a) < parts(b);
                });
                for (const std::string &f : files) {
                    const size_t slash = f.find_last_of('/');
                    out.push_back({slash == std::string::npos ? f : f.substr(slash + 1),
                                   path + "/" + f, n});
                }
            }
        } else if (st.is_regular) {
            out.push_back({n, path, ""});
        }
    }
    return out;
}

int game_chain(const std::string &game_data, const std::string &base, const std::string &chain) {
    if (game_data.empty() || !is_dir(game_data))
        return -1;
    std::vector<std::string> names = list_dir(game_data);
    for (std::string &n : names)
        n = lower(n);
    auto has = [&](const std::string &n) {
        return std::find(names.begin(), names.end(), n) != names.end();
    };
    if (!has(lower(base) + "." + chain + "od"))
        return -1;
    int n = 0;
    char buf[32];
    for (;;) {
        snprintf(buf, sizeof buf, ".%s%02d", chain.c_str(), n);
        if (!has(lower(base) + buf))
            break;
        ++n;
    }
    return n;
}

std::string slot_name(const std::string &base, const std::string &chain, int slot) {
    if (slot < 0)
        return base + "." + chain + "od";
    char buf[32];
    snprintf(buf, sizeof buf, ".%s%02d", chain.c_str(), slot);
    return base + buf;
}

std::string resolved(const std::string &path) {
#ifndef _WIN32
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf))
        return buf;
#endif
    return path;
}

std::string python_repr(const JValue *v) {
    if (!v || v->type == JValue::Null)
        return "None";
    if (v->type == JValue::String)
        return "'" + v->s + "'";
    return "?";
}

bool is_link(const std::string &path) {
    OsStat st{};
    return os_lstat(path.c_str(), &st) == 0 && st.is_symlink;
}

bool make_link(const std::string &target, const std::string &path) {
#ifndef _WIN32
    return symlink(target.c_str(), path.c_str()) == 0;
#else
    (void)target;
    (void)path;
    return false;
#endif
}

void mkdirs_for(const std::string &file) {
    for (size_t i = 1; i < file.size(); ++i)
        if (file[i] == '/')
            os_mkdir(file.substr(0, i).c_str());
}

// G3V0 archives as tools/g3pak.py reads and packs them: a 48-byte header, file
// data from 0x30, then a tree of nodes (three FILETIMEs, u64 0, u32 attributes;
// a directory has its name, its subdirectories and its files behind u32
// counts; a file has offset, stored size, size, encryption, compression, its
// path and the path it was packed from).
struct PakReader {
    const std::string &d;
    size_t at = 0;
    bool ok = true;
    std::string want; // lower-case path looked for
    std::string found;
    explicit PakReader(const std::string &data) : d(data) {}
    uint64_t u(int n) {
        if (at + n > d.size()) {
            ok = false;
            return 0;
        }
        uint64_t v = 0;
        for (int i = n - 1; i >= 0; --i)
            v = (v << 8) | (unsigned char)d[at + i];
        at += n;
        return v;
    }
    std::string name() {
        const uint64_t n = u(4);
        if (!n || !ok)
            return "";
        if (at + n + 1 > d.size()) {
            ok = false;
            return "";
        }
        std::string s = d.substr(at, n);
        at += n + 1;
        return s;
    }
    void node(int depth) {
        if (depth > 64) {
            ok = false;
            return;
        }
        at += 32;
        const uint64_t attributes = u(4);
        if (!ok)
            return;
        if (attributes & 0x10) {
            name();
            for (uint64_t i = u(4); ok && i > 0; --i)
                node(depth + 1);
            for (uint64_t i = u(4); ok && i > 0; --i)
                node(depth + 1);
            return;
        }
        const uint64_t offset = u(8), stored = u(8), size = u(8), encryption = u(4), compression = u(4);
        const std::string path = name();
        name();
        if (!ok || lower(path) != want || !found.empty() || encryption || offset + stored > d.size())
            return;
        if (compression == 2) {
            std::string out(size, '\0');
            mz_ulong len = mz_ulong(size);
            if (size && mz_uncompress(reinterpret_cast<unsigned char *>(&out[0]), &len,
                                      reinterpret_cast<const unsigned char *>(d.data() + offset),
                                      mz_ulong(stored)) == MZ_OK &&
                len == size)
                found = out;
        } else if (stored == size) {
            found = d.substr(offset, size);
        }
    }
};

// The unpacked entry `key` (any case) of the archive at `path`; false when the
// file is no archive or has no such entry.
bool pak_read(const std::string &path, const std::string &key, std::string *out) {
    const std::string data = read_text(path);
    if (data.size() < 48 || data.compare(4, 4, "G3V0") != 0)
        return false;
    PakReader r(data);
    r.want = lower(key);
    r.at = 32;
    r.at = size_t(r.u(8));
    r.node(0);
    if (!r.ok || r.found.empty())
        return false;
    *out = r.found;
    return true;
}

void put(std::string *s, uint64_t v, int n) {
    for (int i = 0; i < n; ++i)
        s->push_back(char((v >> (8 * i)) & 0xff));
}

void put_name(std::string *s, const std::string &n) {
    put(s, n.size(), 4);
    if (!n.empty()) {
        s->append(n);
        s->push_back('\0');
    }
}

// What G3PakDir.exe --no-comments writes (tools/g3pak.py `pack`): entries
// stored, case-insensitive name order, subdirectories first; a null entry is a
// deleted one (attributes 0x28000), which hides that name in lower archives.
struct PakDir {
    std::vector<std::pair<std::string, PakDir>> dirs;
    std::vector<std::pair<std::string, const std::string *>> files;
};

bool lower_less(const std::string &a, const std::string &b) { return lower(a) < lower(b); }

void pak_emit(PakDir &dir, const std::string &prefix, const std::string &times, std::string *body,
              std::string *nodes) {
    nodes->append(times);
    put(nodes, 0x20010, 4);
    put_name(nodes, prefix);
    std::stable_sort(dir.dirs.begin(), dir.dirs.end(),
                     [](const auto &a, const auto &b) { return lower_less(a.first, b.first); });
    std::stable_sort(dir.files.begin(), dir.files.end(),
                     [](const auto &a, const auto &b) { return lower_less(a.first, b.first); });
    put(nodes, dir.dirs.size(), 4);
    for (auto &d : dir.dirs)
        pak_emit(d.second, prefix + d.first, times, body, nodes);
    put(nodes, dir.files.size(), 4);
    for (const auto &f : dir.files) {
        nodes->append(times);
        if (!f.second) {
            put(nodes, 0x28000, 4);
            nodes->append(32, '\0');
        } else {
            put(nodes, 0x20020, 4);
            put(nodes, 0x30 + body->size(), 8);
            put(nodes, f.second->size(), 8);
            put(nodes, f.second->size(), 8);
            put(nodes, 0, 8);
            body->append(*f.second);
        }
        put_name(nodes, prefix + f.first);
        put_name(nodes, "");
    }
}

bool pak_write(const std::string &out, const std::vector<std::pair<std::string, const std::string *>> &entries,
               uint64_t filetime) {
    PakDir root;
    for (const auto &e : entries) {
        PakDir *node = &root;
        const std::vector<std::string> p = parts(e.first);
        for (size_t i = 0; i + 1 < p.size(); ++i) {
            const std::string d = p[i] + "/";
            auto it = std::find_if(node->dirs.begin(), node->dirs.end(),
                                   [&](const auto &x) { return x.first == d; });
            if (it == node->dirs.end()) {
                node->dirs.push_back({d, PakDir()});
                it = node->dirs.end() - 1;
            }
            node = &it->second;
        }
        node->files.push_back({p.back(), e.second});
    }
    std::string times, body, nodes;
    put(&times, filetime, 8);
    put(&times, filetime, 8);
    put(&times, filetime, 8);
    put(&times, 0, 8);
    pak_emit(root, "", times, &body, &nodes);
    std::string file;
    put(&file, 0, 4);
    file.append("G3V0");
    put(&file, 0, 8);
    put(&file, 1, 4);
    file.append("G3PD");
    put(&file, 0x30 + body.size(), 8);
    put(&file, 0x30 + body.size(), 8);
    put(&file, 0x30 + body.size() + nodes.size(), 8);
    file += body;
    file += nodes;
    put(&file, 0, 4);
    return write_text(out, file);
}

// UTF-16 text as Python reads it: "utf-16-le" keeps a BOM as a character,
// "utf-16" takes one off (and reads big endian after FE FF).
std::u16string utf16_decode(const std::string &b, bool take_bom) {
    std::u16string s;
    size_t i = 0;
    bool big = false;
    if (take_bom && b.size() >= 2) {
        if ((unsigned char)b[0] == 0xff && (unsigned char)b[1] == 0xfe)
            i = 2;
        else if ((unsigned char)b[0] == 0xfe && (unsigned char)b[1] == 0xff)
            i = 2, big = true;
    }
    for (; i + 1 < b.size(); i += 2) {
        const unsigned lo = (unsigned char)b[i], hi = (unsigned char)b[i + 1];
        s.push_back(char16_t(big ? (lo << 8 | hi) : (hi << 8 | lo)));
    }
    return s;
}

std::string utf16le_encode(const std::u16string &s) {
    std::string b;
    b.reserve(s.size() * 2);
    for (char16_t c : s) {
        b.push_back(char(c & 0xff));
        b.push_back(char(c >> 8));
    }
    return b;
}

void erase_first(std::u16string *s, const std::u16string &what) {
    const size_t at = s->find(what);
    if (at != std::u16string::npos)
        s->erase(at, what.size());
}

// strings.mod's stringtable.ini, as tools/g3mods.py merge_strings.
bool merge_strings(const std::string &base, const std::vector<std::string> &tables, std::string *out) {
    std::string raw;
    if (!pak_read(base, "stringtable.ini", &raw))
        return false;
    std::u16string text = utf16_decode(raw, false);
    erase_first(&text, u"[LocAdmin_Revisions]");
    for (const std::string &t : tables) {
        std::u16string part = utf16_decode(read_text(t), true);
        erase_first(&part, u"[LocAdmin_Strings]");
        if (text.empty() || text.back() != u'\n')
            text += u"\r\n";
        text += part;
    }
    if (text.empty() || text.back() != u'\n')
        text += u"\r\n";
    text += u"[LocAdmin_Revisions]\r\n";
    *out = utf16le_encode(text);
    return true;
}

// G3_World_01.wrldatasc, as tools/g3mods.py merge_sectors.
bool merge_sectors(const std::string &base, const std::vector<std::string> &lists, std::string *out) {
    OsStat st{};
    if (os_stat(base.c_str(), &st) != 0)
        return false;
    std::string text = read_text(base);
    for (const std::string &l : lists) {
        std::string part = read_text(l);
        const size_t at = part.find("[Sector.List]");
        if (at != std::string::npos)
            part = part.substr(at + 13);
        if (text.empty() || text.back() != '\n')
            text += "\r\n";
        const size_t start = part.find_first_not_of("\r\n");
        text += start == std::string::npos ? std::string() : part.substr(start);
    }
    *out = text;
    return true;
}

// tools/g3mods.py inputs_signature: the kind, then each input's path, size and
// time in whole seconds, a line each. Empty when one cannot be read.
std::string inputs_signature(const ModViewGen &g, int64_t *newest) {
    std::string sig = g.kind;
    *newest = 0;
    std::vector<std::string> all{g.base};
    all.insert(all.end(), g.inputs.begin(), g.inputs.end());
    for (const std::string &p : all) {
        OsStat st{};
        if (os_stat(p.c_str(), &st) != 0)
            return "";
        sig += "\n" + p + " " + std::to_string(st.size) + " " + std::to_string(st.mtime);
        *newest = std::max(*newest, st.mtime);
    }
    return sig;
}

std::string parent_dir(std::string p) {
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string() : slash == 0 ? std::string("/") : p.substr(0, slash);
}

// The archive Sefaris takes the game's string table from: the first Strings*
// of Data in NTFS order (upper case names).
std::string strings_base(const std::string &game_data) {
    if (game_data.empty() || !is_dir(game_data))
        return "";
    std::vector<std::string> names = list_dir(game_data);
    std::stable_sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
        std::string ua = a, ub = b;
        for (char &c : ua)
            c = char(std::toupper((unsigned char)c));
        for (char &c : ub)
            c = char(std::toupper((unsigned char)c));
        return ua < ub;
    });
    for (const std::string &n : names) {
        OsStat st{};
        if (lower(n).compare(0, 7, "strings") == 0 && os_stat((game_data + "/" + n).c_str(), &st) == 0 &&
            st.is_regular)
            return game_data + "/" + n;
    }
    return "";
}

} // namespace

bool g3pak_read(const std::string &archive, const std::string &key, std::string *out) {
    return pak_read(archive, key, out);
}

bool g3pak_write(const std::string &archive,
                 const std::vector<std::pair<std::string, const std::string *>> &entries, uint64_t filetime) {
    return pak_write(archive, entries, filetime);
}

ModView plan_mod_view(const std::string &profile, const std::string &game_data) {
    ModView view;
    const std::string contract = profile + "/active-mods.json";
    OsStat st{};
    if (os_stat(contract.c_str(), &st) != 0)
        return view;
    JValue spec;
    const JValue *mods = nullptr;
    if (!parse_json(read_text(contract), &spec)) {
        view.errors.push_back(contract + ": not valid JSON");
        return view;
    }
    const JValue *version = spec.get("version");
    mods = spec.get("mods");
    if (spec.type != JValue::Object || !version || version->type != JValue::Other ||
        (version->s != "1" && version->s != "1.0") ||
        !mods || mods->type != JValue::Array) {
        view.errors.push_back(contract + ": not version 1 with a mods list");
        return view;
    }
    struct Ini {
        std::string name, path, mod;
    };
    std::vector<std::pair<std::string, Ini>> inis; // by lower name, the last mod's
    struct Entry {
        std::string base, path, mod;
    };
    std::vector<std::pair<std::pair<std::string, std::string>, std::vector<Entry>>> chains;
    auto chain_of = [&](const std::string &chain, const std::string &base) -> std::vector<Entry> & {
        const std::pair<std::string, std::string> key{chain, lower(base)};
        auto it = std::find_if(chains.begin(), chains.end(), [&](const auto &e) { return e.first == key; });
        if (it == chains.end()) {
            chains.push_back({key, {}});
            it = chains.end() - 1;
        }
        return it->second;
    };
    std::vector<std::pair<std::string, std::string>> tables, sectors; // (mod id, path) in load order
    int used = 0;
    for (size_t i = 0; i < mods->a.size(); ++i) {
        const JValue &mod = mods->a[i];
        std::string mid = mod.type == JValue::Object ? mod.str("id") : std::string();
        if (mid.empty())
            mid = "#" + std::to_string(i);
        const JValue *pv = mod.type == JValue::Object ? mod.get("path") : nullptr;
        const std::string path = pv && pv->type == JValue::String ? pv->s : std::string();
        if (path.empty() || path[0] != '/' || !is_dir(path)) {
            view.errors.push_back(mid + ": folder " + python_repr(pv) + " not found, mod left out");
            continue;
        }
        ++used;
        struct File {
            std::string chain, base;
            long order;
            std::string name, path;
        };
        std::vector<File> files;
        for (const PayloadFile &f : mod_payload(path)) {
            const std::string sub = lower(f.sub);
            if (!f.sub.empty() && sub != "data" &&
                !(sub == "static" && lower(f.name) == "g3_world_01.wrldatasc")) {
                std::string reason;
                if (sub == "scripts" || is_program(f.name))
                    reason = "script plugin (" + f.sub + "/" + f.name + "): needs translating, not loaded";
                else
                    reason = "in " + f.sub + "/: not a folder the view maps";
                view.skipped.push_back({mid, f.path, reason});
                continue;
            }
            const Classified c = classify(f.name);
            if (c.chain.empty()) {
                view.skipped.push_back({mid, f.path, c.reason});
                continue;
            }
            if (c.chain == "strings" || c.chain == "sectors") {
                (c.chain == "strings" ? tables : sectors).push_back({mid, f.path});
                continue;
            }
            if (c.chain == "ini") {
                const std::string key = lower(c.base);
                auto it = std::find_if(inis.begin(), inis.end(),
                                       [&](const auto &e) { return e.first == key; });
                if (it != inis.end())
                    it->second = {c.base, f.path, mid};
                else
                    inis.push_back({key, {c.base, f.path, mid}});
                continue;
            }
            files.push_back({c.chain, c.base, c.order, f.name, f.path});
        }
        // Within one mod: the head, the numbers it asked for, placeholders last.
        std::stable_sort(files.begin(), files.end(), [](const File &a, const File &b) {
            if (a.chain != b.chain)
                return a.chain < b.chain;
            if (lower(a.base) != lower(b.base))
                return lower(a.base) < lower(b.base);
            if (a.order != b.order)
                return a.order < b.order;
            return a.name < b.name;
        });
        for (const File &f : files)
            chain_of(f.chain, f.base).push_back({f.base, f.path, mid});
    }
    if (used) {
        const std::string gen = profile + "/mods-generated";
        const std::string base = strings_base(game_data);
        if (base.empty()) {
            view.errors.push_back("no Strings* archive in " + game_data +
                                  ": strings.mod not built, the mods' string tables left out");
        } else {
            chain_of("m", "strings").push_back({"strings", gen + "/strings.mod", "(merged)"});
            ModViewGen g{"strings.mod", "strings", base, "", {}};
            for (const auto &t : tables)
                g.inputs.push_back(t.second);
            view.generated.push_back(g);
        }
        // The catalog's Static/: "static" in the contract, else beside the
        // first mod's parent folder (G3_Mods/Mods/<mod> -> G3_Mods/Static).
        std::vector<std::string> candidates;
        if (const JValue *sv = spec.get("static"); sv && sv->type == JValue::String && !sv->s.empty() &&
                                                   sv->s[0] == '/')
            candidates.push_back(sv->s);
        for (const JValue &mod : mods->a) {
            const std::string p = mod.type == JValue::Object ? mod.str("path") : std::string();
            if (!p.empty() && p[0] == '/') {
                candidates.push_back(parent_dir(parent_dir(p)) + "/Static");
                break;
            }
        }
        std::string stat;
        for (const std::string &c : candidates) {
            OsStat st{};
            if (os_stat((c + "/Projects_compiled.m0x").c_str(), &st) == 0 && st.is_regular) {
                stat = c;
                break;
            }
        }
        if (stat.empty()) {
            for (const auto &s : sectors)
                view.skipped.push_back({s.first, s.second, "no Static/ folder to merge world sectors into"});
            view.skipped.push_back({"(static)", "Static/",
                                    "no Sefaris Static/ folder (\"static\" in active-mods.json or beside "
                                    "the mods' folder): the world sector list and the archives that hide "
                                    "the compiled caches left out"});
        } else {
            std::vector<Entry> &pc = chain_of("m", "projects_compiled");
            pc.push_back({"Projects_compiled", gen + "/projects_compiled.mod", "(merged)"});
            pc.push_back({"Projects_compiled", stat + "/Projects_compiled.m0x", "(static)"});
            OsStat st{};
            if (os_stat((stat + "/Projects_compiled.n0x").c_str(), &st) == 0 && st.is_regular)
                chain_of("n", "projects_compiled")
                    .push_back({"Projects_compiled", stat + "/Projects_compiled.n0x", "(static)"});
            ModViewGen g{"projects_compiled.mod", "sectors", stat + "/G3_World_01.wrldatasc", "", {}};
            for (const auto &s : sectors)
                g.inputs.push_back(s.second);
            view.generated.push_back(g);
        }
    }
    std::sort(chains.begin(), chains.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &ch : chains) {
        int slot = game_chain(game_data, ch.second.front().base, ch.first.first);
        for (const Entry &e : ch.second) {
            view.links.push_back({"Data/" + slot_name(lower(e.base), ch.first.first, slot),
                                  resolved(e.path), e.mod});
            ++slot;
        }
    }
    std::vector<Ini> sorted;
    for (const auto &e : inis)
        sorted.push_back(e.second);
    std::sort(sorted.begin(), sorted.end(), [](const Ini &a, const Ini &b) {
        if (a.name != b.name)
            return a.name < b.name;
        if (a.path != b.path)
            return a.path < b.path;
        return a.mod < b.mod;
    });
    for (const Ini &e : sorted)
        view.links.push_back({"ini/" + e.name, resolved(e.path), e.mod});
    return view;
}

namespace {
// The generated files the last build recorded: file name -> signature.
std::vector<std::pair<std::string, std::string>> recorded_generated(const std::string &profile) {
    std::vector<std::pair<std::string, std::string>> out;
    JValue rec;
    if (parse_json(read_text(profile + "/mods-view.json"), &rec) && rec.type == JValue::Object)
        if (const JValue *gen = rec.get("generated"); gen && gen->type == JValue::Array)
            for (const JValue &g : gen->a) {
                std::string f = g.str("file");
                f = f.substr(f.find_last_of('/') == std::string::npos ? 0 : f.find_last_of('/') + 1);
                if (!f.empty() && f != "." && f != "..")
                    out.push_back({f, g.str("signature")});
            }
    return out;
}

void remove_generated(const std::string &profile, const std::string &file) {
    const std::string p = profile + "/mods-generated/" + file;
    OsStat st{};
    if (os_lstat(p.c_str(), &st) == 0 && st.is_regular)
        os_unlink(p.c_str());
}
} // namespace

int clear_mod_view(const std::string &profile, bool generated) {
    int removed = 0;
    const std::string record = profile + "/mods-view.json";
    JValue rec;
    if (parse_json(read_text(record), &rec) && rec.type == JValue::Object)
        if (const JValue *links = rec.get("links"); links && links->type == JValue::Array)
            for (const JValue &l : links->a) {
                const std::string p = l.str("path");
                if (p.empty() || p.find("..") != std::string::npos)
                    continue;
                if (is_link(profile + "/" + p) && os_unlink((profile + "/" + p).c_str()) == 0)
                    ++removed;
            }
    if (generated) {
        for (const auto &g : recorded_generated(profile))
            remove_generated(profile, g.first);
        os_rmdir((profile + "/mods-generated").c_str()); // only when empty
    }
    OsStat st{};
    if (os_lstat(record.c_str(), &st) == 0)
        os_unlink(record.c_str());
    return removed;
}

std::string mod_view_json(const ModView &v) {
    std::string out = "{\n  \"version\": 1,\n  \"links\": [";
    for (size_t i = 0; i < v.links.size(); ++i) {
        out += i ? ",\n    {\n      \"path\": " : "\n    {\n      \"path\": ";
        json_string(&out, v.links[i].path);
        out += ",\n      \"target\": ";
        json_string(&out, v.links[i].target);
        out += ",\n      \"mod\": ";
        json_string(&out, v.links[i].mod);
        out += "\n    }";
    }
    out += v.links.empty() ? "],\n  \"generated\": [" : "\n  ],\n  \"generated\": [";
    for (size_t i = 0; i < v.generated.size(); ++i) {
        const ModViewGen &g = v.generated[i];
        out += i ? ",\n    {\n      \"file\": " : "\n    {\n      \"file\": ";
        json_string(&out, g.file);
        out += ",\n      \"kind\": ";
        json_string(&out, g.kind);
        out += ",\n      \"signature\": ";
        json_string(&out, g.signature);
        out += ",\n      \"inputs\": [";
        for (size_t j = 0; j < g.inputs.size(); ++j) {
            out += j ? ", " : "";
            json_string(&out, g.inputs[j]);
        }
        out += "]\n    }";
    }
    out += v.generated.empty() ? "],\n  \"skipped\": [" : "\n  ],\n  \"skipped\": [";
    for (size_t i = 0; i < v.skipped.size(); ++i) {
        out += i ? ",\n    {\n      \"mod\": " : "\n    {\n      \"mod\": ";
        json_string(&out, v.skipped[i].mod);
        out += ",\n      \"file\": ";
        json_string(&out, v.skipped[i].file);
        out += ",\n      \"reason\": ";
        json_string(&out, v.skipped[i].reason);
        out += "\n    }";
    }
    out += v.skipped.empty() ? "],\n  \"errors\": [" : "\n  ],\n  \"errors\": [";
    for (size_t i = 0; i < v.errors.size(); ++i) {
        out += i ? ",\n    " : "\n    ";
        json_string(&out, v.errors[i]);
    }
    out += v.errors.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

ModView build_mod_view(const std::string &profile, const std::string &game_data) {
    OsStat st{};
    if (os_stat((profile + "/active-mods.json").c_str(), &st) != 0) {
        // No mods picked: what an earlier build made goes, and a profile that
        // never had a view gets nothing written.
        clear_mod_view(profile);
        return {};
    }
    ModView planned = plan_mod_view(profile, game_data);
    const auto previous = recorded_generated(profile);
    clear_mod_view(profile, false);
    ModView made;
    made.skipped = planned.skipped;
    made.errors = planned.errors;
    // The merged archives, rewritten only when an input changed (the
    // signature tools/g3mods.py records too). A failed one is not linked.
    std::vector<std::string> dropped;
    for (ModViewGen g : planned.generated) {
        const std::string out = profile + "/mods-generated/" + g.file;
        int64_t newest = 0;
        g.signature = inputs_signature(g, &newest);
        auto old = std::find_if(previous.begin(), previous.end(), [&](const auto &p) { return p.first == g.file; });
        OsStat ost{};
        bool ok = !g.signature.empty();
        if (ok && !(old != previous.end() && old->second == g.signature && os_lstat(out.c_str(), &ost) == 0 &&
                    ost.is_regular)) {
            std::string data;
            const std::string inner = g.kind == "strings" ? "stringtable.ini" : "G3_World_01/G3_World_01.wrldatasc";
            ok = g.kind == "strings" ? merge_strings(g.base, g.inputs, &data) : merge_sectors(g.base, g.inputs, &data);
            mkdirs_for(out);
            const uint64_t filetime = uint64_t(newest) * 10000000ull + 116444736000000000ull;
            ok = ok && pak_write(out, {{inner, &data}}, filetime);
        }
        if (!ok) {
            made.errors.push_back(g.file + " not generated: an input could not be read");
            dropped.push_back(out);
            dropped.push_back(resolved(out));
            continue;
        }
        made.generated.push_back(g);
    }
    for (const auto &p : previous)
        if (std::none_of(made.generated.begin(), made.generated.end(),
                         [&](const ModViewGen &g) { return g.file == p.first; }))
            remove_generated(profile, p.first);
    for (const ModViewLink &l : planned.links) {
        if (std::find(dropped.begin(), dropped.end(), l.target) != dropped.end())
            continue;
        const std::string p = profile + "/" + l.path;
        mkdirs_for(p);
        if (is_link(p))
            os_unlink(p.c_str());
        else if (os_lstat(p.c_str(), &st) == 0) {
            made.errors.push_back(p + " exists in the profile and is not a link from a build: left "
                                      "alone, mod " +
                                  l.mod + " not linked there");
            continue;
        }
        if (!make_link(l.target, p)) {
            made.errors.push_back(p + ": could not make the link to " + l.target);
            continue;
        }
        made.links.push_back(l);
    }
    write_text(profile + "/mods-view.json", mod_view_json(made));
    return made;
}

} // namespace launcher
