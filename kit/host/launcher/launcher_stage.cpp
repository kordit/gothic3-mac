// launcher_stage.cpp - see launcher_stage.h.
#include "launcher_stage.h"

#include "launcher.h"

#include "../../platform/os.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace launcher {

namespace {

uint16_t rd16(const std::vector<uint8_t> &b, size_t at) {
    return uint16_t(b[at] | b[at + 1] << 8);
}
uint32_t rd32(const std::vector<uint8_t> &b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 |
           uint32_t(b[at + 3]) << 24;
}
void wr32(std::vector<uint8_t> &b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b[at + size_t(i)] = uint8_t(v >> (8 * i));
}

bool read_all(const std::string &path, std::vector<uint8_t> *out) {
    const int fd = os_fd_open(path.c_str(), OS_O_RDONLY);
    if (fd < 0)
        return false;
    out->clear();
    uint8_t buf[1 << 16];
    for (int64_t n; (n = os_fd_read(fd, buf, sizeof buf)) != 0;) {
        if (n < 0) {
            os_fd_close(fd);
            return false;
        }
        out->insert(out->end(), buf, buf + n);
    }
    os_fd_close(fd);
    return true;
}

bool write_all(const std::string &path, const std::vector<uint8_t> &bytes) {
    const int fd = os_fd_open(path.c_str(), OS_O_WRONLY | OS_O_CREAT | OS_O_TRUNC);
    if (fd < 0)
        return false;
    size_t at = 0;
    while (at < bytes.size()) {
        const int64_t n = os_fd_write(fd, bytes.data() + at, bytes.size() - at);
        if (n <= 0) {
            os_fd_close(fd);
            return false;
        }
        at += size_t(n);
    }
    return os_fd_close(fd) == 0;
}

std::string join(const std::string &a, const std::string &b) {
    return a.empty() || a.back() == '/' ? a + b : a + "/" + b;
}

// The file `relative` names under root, each component found without case
// (an install copied through a case-insensitive volume keeps any spelling).
std::string resolve(const std::string &root, const std::string &relative) {
    std::string at = root;
    size_t from = 0;
    while (from <= relative.size()) {
        size_t slash = relative.find('/', from);
        if (slash == std::string::npos)
            slash = relative.size();
        const std::string part = relative.substr(from, slash - from);
        const std::string found = find_name(at, part);
        at = join(at, found.empty() ? part : found);
        from = slash + 1;
    }
    return at;
}

// A cabinet's member, through the system's bsdtar (macOS, Windows 10 and
// later, most Linux distributions), into `dir`.
bool extract_cab(const std::string &cab, const std::string &member, const std::string &dir,
                 std::string *error) {
#ifdef _WIN32
    const char *tar = "C:\\Windows\\System32\\tar.exe";
#else
    const char *tar = "/usr/bin/tar";
#endif
    const char *argv[] = {tar, "-xf", cab.c_str(), "-C", dir.c_str(), member.c_str(), nullptr};
    int64_t pid = 0;
    int code = -1;
    if (os_spawn(argv, &pid) != 0 || os_wait(pid, &code) != 0 || code != 0) {
        *error = std::string(tar) + " could not take " + member + " out of " + cab;
        return false;
    }
    return true;
}

} // namespace

bool rebase_pe(std::vector<uint8_t> *file, uint32_t base, std::string *error) {
    std::vector<uint8_t> &b = *file;
    auto fail = [&](const std::string &why) {
        *error = why;
        return false;
    };
    if (b.size() < 0x40 || rd16(b, 0) != 0x5a4d)
        return fail("not an MZ image");
    const size_t pe = rd32(b, 0x3c);
    if (pe + 24 > b.size() || rd32(b, pe) != 0x00004550)
        return fail("not a PE image");
    const uint16_t nsections = rd16(b, pe + 6);
    const uint16_t opt_size = rd16(b, pe + 20);
    const size_t opt = pe + 24;
    if (opt + opt_size > b.size() || opt_size < 96 + 6 * 8 || rd16(b, opt) != 0x10b)
        return fail("not a PE32 image");
    const uint32_t old = rd32(b, opt + 28);
    const uint32_t headers = rd32(b, opt + 60);
    if (rd32(b, opt + 92) < 6)
        return fail("no base relocation directory");
    const uint32_t reloc_rva = rd32(b, opt + 96 + 5 * 8),
                   reloc_size = rd32(b, opt + 96 + 5 * 8 + 4);
    if (!reloc_rva || !reloc_size)
        return fail("no relocations to rebase with");
    const size_t sections = opt + opt_size;
    if (sections + size_t(nsections) * 40 > b.size())
        return fail("section table past the end of the file");
    // pefile's get_offset_from_rva: inside a section's raw data, or in the headers.
    auto offset = [&](uint32_t rva, size_t *out) {
        for (uint16_t i = 0; i < nsections; ++i) {
            const size_t s = sections + size_t(i) * 40;
            const uint32_t va = rd32(b, s + 12), vsize = rd32(b, s + 8), raw = rd32(b, s + 16),
                           ptr = rd32(b, s + 20);
            const uint32_t span = vsize > raw ? vsize : raw;
            if (rva >= va && rva - va < span) {
                if (rva - va >= raw)
                    return false;
                *out = size_t(ptr) + (rva - va);
                return true;
            }
        }
        if (rva < headers) {
            *out = rva;
            return true;
        }
        return false;
    };
    const uint32_t delta = base - old;
    size_t at = 0;
    if (!offset(reloc_rva, &at))
        return fail("relocation directory outside the file");
    const size_t end = at + reloc_size;
    if (end > b.size())
        return fail("relocation directory past the end of the file");
    while (at + 8 <= end) {
        const uint32_t page = rd32(b, at), block = rd32(b, at + 4);
        if (block < 8 || at + block > end)
            break;
        for (size_t e = at + 8; e + 2 <= at + block; e += 2) {
            const uint16_t entry = rd16(b, e);
            const int type = entry >> 12;
            const uint32_t rva = page + (entry & 0xfff);
            if (type == 0)
                continue;
            if (type != 3) {
                char why[96];
                snprintf(why, sizeof why, "relocation type %d at rva %#x is not supported", type,
                         rva);
                return fail(why);
            }
            size_t fix = 0;
            if (!offset(rva, &fix) || fix + 4 > b.size()) {
                char why[96];
                snprintf(why, sizeof why, "fixup at rva %#x has no file bytes", rva);
                return fail(why);
            }
            wr32(b, fix, rd32(b, fix) + delta);
        }
        at += block;
    }
    wr32(b, opt + 28, base);
    return true;
}

bool modules_staged(const std::string &root, const std::vector<StageModule> &modules) {
    for (const StageModule &m : modules)
        if (sha256_file(resolve(root, m.dest)) != m.sha256)
            return false;
    return true;
}

StageOutcome
stage_modules(const std::string &root, const std::vector<StageModule> &modules,
              const std::function<void(size_t, size_t, const std::string &)> &progress) {
    StageOutcome out;
    std::string tmp;
    for (size_t i = 0; i < modules.size(); ++i) {
        const StageModule &m = modules[i];
        if (progress)
            progress(i, modules.size(), m.name);
        const std::string dest = resolve(root, m.dest);
        if (sha256_file(dest) == m.sha256) {
            ++out.kept;
            continue;
        }
        std::string src = resolve(root, m.from);
        const std::string digest = sha256_file(src);
        if (digest.empty()) {
            out.error = m.from + " is missing";
            break;
        }
        if (digest != m.from_sha256) {
            out.error = m.from + " has SHA-256 " + digest + ", not the supported " + m.from_sha256;
            break;
        }
        if (m.prepare.compare(0, 4, "cab:") == 0) {
            if (tmp.empty()) {
                char dir[4096];
                snprintf(dir, sizeof dir, "%s/.stage-XXXXXX", root.c_str());
                if (os_mkdtemp(dir) != 0) {
                    out.error = "cannot make a folder in " + root;
                    break;
                }
                tmp = dir;
            }
            const std::string member = m.prepare.substr(4);
            if (!extract_cab(src, member, tmp, &out.error))
                break;
            src = join(tmp, member);
        } else if (!m.prepare.empty()) {
            out.error = m.name + ": unknown prepare step " + m.prepare;
            break;
        }
        std::vector<uint8_t> image;
        if (!read_all(src, &image)) {
            out.error = "cannot read " + src;
            break;
        }
        std::string why;
        if (!rebase_pe(&image, m.base, &why)) {
            out.error = m.name + ": " + why;
            break;
        }
        const std::string part = dest + ".part";
        if (!write_all(part, image)) {
            out.error = "cannot write " + part;
            break;
        }
        const std::string made = sha256_file(part);
        if (made != m.sha256) {
            os_unlink(part.c_str());
            out.error = m.name + " rebased to SHA-256 " + made + ", not the expected " + m.sha256;
            break;
        }
        if (os_rename(part.c_str(), dest.c_str()) != 0) {
            os_unlink(part.c_str());
            out.error = "cannot replace " + dest;
            break;
        }
        ++out.made;
    }
    if (!tmp.empty())
        remove_tree(tmp);
    out.ok = out.error.empty();
    if (out.ok && progress)
        progress(modules.size(), modules.size(), "");
    return out;
}

namespace {

// One edit applied to a file's text; false when the edit is malformed.
bool edit_text(std::string *text, const std::string &section, const std::string &edit) {
    const std::string eol = text->find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    for (size_t at = 0; at < text->size();) {
        size_t nl = text->find('\n', at);
        const size_t end = nl == std::string::npos ? text->size() : nl;
        std::string line = text->substr(at, end - at);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
        at = nl == std::string::npos ? text->size() : nl + 1;
    }
    const bool final_eol = !text->empty() && text->back() == '\n';
    if (final_eol)
        lines.push_back(""); // what follows the last newline, so joining puts it back
    auto same = [](const std::string &a, const std::string &b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return tolower((unsigned char)x) == tolower((unsigned char)y);
               });
    };
    if (section.empty()) {
        if (edit.size() < 2 || (edit[0] != '+' && edit[0] != '-'))
            return false;
        const std::string want = edit.substr(1);
        auto it = std::find_if(lines.begin(), lines.end(),
                               [&](const std::string &l) { return same(l, want); });
        if (edit[0] == '+' && it == lines.end()) {
            // After the last line with text, so a file without a final newline stays one.
            if (!lines.empty() && lines.back().empty() && final_eol)
                lines.back() = want, lines.push_back("");
            else
                lines.push_back(want);
        } else if (edit[0] == '-')
            while (it != lines.end()) {
                lines.erase(it);
                it = std::find_if(lines.begin(), lines.end(),
                                  [&](const std::string &l) { return same(l, want); });
            }
    } else {
        const size_t eq = edit.find('=');
        if (eq == std::string::npos || eq == 0)
            return false;
        const std::string key = edit.substr(0, eq);
        size_t i = 0;
        while (i < lines.size() && !same(lines[i], "[" + section + "]"))
            ++i;
        if (i == lines.size())
            return false;
        size_t last = i; // the section's last line with text
        bool done = false;
        for (size_t j = i + 1; j < lines.size() && !(lines[j].size() && lines[j][0] == '['); ++j) {
            const size_t e = lines[j].find('=');
            if (e != std::string::npos && same(lines[j].substr(0, e), key)) {
                lines[j] = edit;
                done = true;
                break;
            }
            if (!lines[j].empty() && lines[j][0] != ';')
                last = j;
        }
        if (!done)
            lines.insert(lines.begin() + long(last + 1), edit);
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size())
            out += eol;
    }
    *text = out;
    return true;
}

} // namespace

bool apply_ini_edits(const std::string &root, const std::vector<std::string> &edits,
                     std::string *error) {
    for (const std::string &e : edits) {
        const size_t a = e.find('|'), b = a == std::string::npos ? a : e.find('|', a + 1);
        if (b == std::string::npos) {
            *error = "malformed ini edit " + e;
            return false;
        }
        const std::string path = resolve(root, e.substr(0, a));
        std::vector<uint8_t> bytes;
        if (!read_all(path, &bytes)) {
            *error = "cannot read " + path;
            return false;
        }
        std::string text(bytes.begin(), bytes.end());
        if (!edit_text(&text, e.substr(a + 1, b - a - 1), e.substr(b + 1))) {
            *error = "cannot apply " + e;
            return false;
        }
        if (text == std::string(bytes.begin(), bytes.end()))
            continue;
        if (!write_all(path + ".part", std::vector<uint8_t>(text.begin(), text.end())) ||
            os_rename((path + ".part").c_str(), path.c_str()) != 0) {
            *error = "cannot write " + path;
            return false;
        }
    }
    return true;
}

} // namespace launcher
