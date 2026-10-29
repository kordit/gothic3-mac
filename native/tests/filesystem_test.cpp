// native/tests/filesystem_test.cpp - native/filesystem_core.cpp without the game (docs/PLAN.md step 1.3).
//
//   sh native/tests/filesystem.sh [game folder]     (default: stage/; builds this, compares with tools/g3pak.py)
//
// index <game> <extended 0|1>: mounts the game folder the way Engine's SetRootPath and MountDir do (Ini/mountlist.ini,
// "projects" -> Data/Projects_compiled as -compiledprojects does, .pak .pNN [.cpt .cNN] .mod .mNN [.nod .nNN]),
// walks every mounted folder the way AddDirToCache does (FindFirstFileA "<folder>/*.*" and down every directory) and
// prints one line per file: the virtual path, the size, the attributes and where an open gets it from (an archive's
// file name, or "disk"). tools/g3pak.py index computes the same lines on its own from the archives' trees.
// read <archive>...: every entry read and inflated, its length checked; a stored entry also read through
// g3fs::EntryBytes from the archive's mapping (whole, then in pieces) and compared with the read, and its first write
// checked to give it its own bytes while the mapping keeps the archive's.
// scratch <dir>: create, write-time, delete and attribute calls on a temporary folder, checked against the contract.
// names <game> <extended 0|1>: for the names of every mounted folder (up to 1500 a folder), finds without wildcards
// (the name, other case, a '.' after it, without its extension, one that is not there) answered by the archives' maps
// and by the walk over every name (g3fs::linear_names), compared result by result.
#include "../filesystem_core.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// POSIX in place of Win32: paths are host paths, the attributes Win32's (0x10 directory, 0x80 normal, 0x01 read-only).
class PosixDisk : public g3fs::Disk {
  public:
    uint32_t attributes(const std::string &path) override {
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return 0xffffffffu;
        uint32_t a = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
        if (!(st.st_mode & S_IWUSR))
            a |= 1u;
        return a;
    }
    bool set_attributes(const std::string &path, uint32_t a) override {
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return false;
        return chmod(path.c_str(), (a & 1u) ? (st.st_mode & ~0222u) : (st.st_mode | S_IWUSR)) == 0;
    }
    bool find(const std::string &pattern, std::vector<g3fs::FindData> *out) override {
        const size_t slash = pattern.rfind('/');
        const std::string dir = slash == std::string::npos ? "." : pattern.substr(0, slash);
        const std::string leaf = slash == std::string::npos ? pattern : pattern.substr(slash + 1);
        DIR *d = opendir(dir.c_str());
        if (!d)
            return false;
        std::vector<std::string> names;
        while (dirent *e = readdir(d))
            names.push_back(e->d_name);
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const std::string &n : names) {
            if (!g3fs::name_fits(leaf, n))
                continue;
            g3fs::FindData r;
            const std::string full = dir + "/" + n;
            struct stat st;
            if (stat(full.c_str(), &st) != 0)
                continue;
            r.w[10] = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
            r.w[6] = (uint32_t)((uint64_t)st.st_size >> 32);
            r.w[7] = (uint32_t)st.st_size;
            r.name = n;
            out->push_back(r);
        }
        return !out->empty();
    }
    bool create_directory(const std::string &path) override { return mkdir(path.c_str(), 0755) == 0; }
    bool remove_directory(const std::string &path) override { return rmdir(path.c_str()) == 0; }
    bool delete_file(const std::string &path) override { return unlink(path.c_str()) == 0; }
    bool copy_file(const std::string &from, const std::string &to, bool fail_if_exists) override {
        if (fail_if_exists && access(to.c_str(), F_OK) == 0)
            return false;
        std::ifstream in(from, std::ios::binary);
        std::ofstream o(to, std::ios::binary);
        o << in.rdbuf();
        return in.good() && o.good();
    }
    bool move_file(const std::string &from, const std::string &to) override {
        return rename(from.c_str(), to.c_str()) == 0;
    }
    std::string full_path(const std::string &path) override { return path; }
    std::string host_path(const std::string &path) override { return path; }
    uint64_t now() override { return ++clock_; }
    uint64_t clock_ = 0x01d9000000000000ull;
};

std::string lowered(std::string s) {
    for (char &c : s)
        c = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    return s;
}

bool exists(const std::string &p) {
    return access(p.c_str(), F_OK) == 0;
}

// SetRootPath and MountDir for one game folder, as the port runs them (-compiledprojects on).
int mount(g3fs::FileSystem &fs, const std::string &root, bool extended, std::vector<std::string> *virtuals) {
    fs.create_volume("G3");
    std::ifstream list(root + "/Ini/mountlist.ini");
    if (!list) {
        fprintf(stderr, "no %s/Ini/mountlist.ini\n", root.c_str());
        return 2;
    }
    fs.create_directory("#G3:/Data");
    std::string line;
    bool in_list = false;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == ';')
            continue;
        if (line[0] == '[') {
            in_list = lowered(line) == "[list]";
            continue;
        }
        const size_t eq = line.find('=');
        if (!in_list || eq == std::string::npos)
            continue;
        std::string v = line.substr(0, eq), p = line.substr(eq + 1);
        if (lowered(v).find("projects") != std::string::npos)
            p = "Data/Projects_compiled";
        const std::string vv = "#G3:/" + v, pp = root + "/" + p;
        fs.create_directory(vv);
        fs.unmount_directory(vv);
        fs.unmount_packs(vv);
        fs.mount_directory(vv, pp);
        auto series = [&](const char *first, char letter) {
            if (!fs.mount_pack(vv, pp + first))
                return;
            for (int i = 0;; ++i) {
                char name[32];
                snprintf(name, sizeof name, i < 10 ? ".%c0%d" : ".%c%d", letter, i);
                if (!exists(pp + name))
                    break;
                fs.mount_pack(vv, pp + name);
            }
        };
        series(".pak", 'p');
        if (extended)
            series(".cpt", 'c');
        series(".mod", 'm');
        if (extended)
            series(".nod", 'n');
        virtuals->push_back(vv);
    }
    return 0;
}

void walk(g3fs::FileSystem &fs, const std::string &dir, std::vector<std::string> *lines) {
    std::vector<g3fs::FindData> found;
    if (!fs.find(dir + "/*.*", &found))
        return;
    for (const auto &d : found) {
        if (d.name == "." || d.name == "..")
            continue;
        const std::string path = dir + "/" + d.name;
        if (d.w[10] & 0x10u) {
            walk(fs, path, lines);
            continue;
        }
        std::string rest, from = "-";
        g3fs::Folder *f = fs.resolve(path, &rest);
        if (f && f->mounted && fs.disk().attributes(f->physical + rest) != 0xffffffffu)
            from = "disk";
        else if (f) {
            g3fs::PackHit hit = fs.pack_open(f, rest);
            if (hit.pack)
                from = hit.pack->path.substr(hit.pack->path.rfind('/') + 1);
        }
        const uint64_t size = (uint64_t)d.w[6] << 32 | d.w[7];
        char buf[64];
        snprintf(buf, sizeof buf, "\t%llu\t%#x\t", (unsigned long long)size, d.w[10]);
        lines->push_back(lowered(path) + buf + from);
    }
}

// Every folder's entries as a find of "<dir>/*.*" lists them: the folder and the names, folders included.
void entries(g3fs::FileSystem &fs, const std::string &dir, std::vector<std::pair<std::string, std::string>> *out) {
    std::vector<g3fs::FindData> found;
    if (!fs.find(dir + "/*.*", &found))
        return;
    for (const auto &d : found) {
        if (d.name == "." || d.name == "..")
            continue;
        out->push_back({dir, d.name});
        if (d.w[10] & 0x10u)
            entries(fs, dir + "/" + d.name, out);
    }
}

bool same(const std::vector<g3fs::FindData> &a, const std::vector<g3fs::FindData> &b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].name != b[i].name || a[i].physical != b[i].physical || a[i].filled != b[i].filled ||
            memcmp(a[i].w, b[i].w, sizeof a[i].w) != 0)
            return false;
    return true;
}

int run_names(const std::string &root, bool extended) {
    PosixDisk disk;
    g3fs::FileSystem fs(disk);
    std::vector<std::string> virtuals;
    if (int rc = mount(fs, root, extended, &virtuals))
        return rc;
    std::vector<std::pair<std::string, std::string>> all;
    for (const auto &v : virtuals)
        entries(fs, v, &all);
    std::map<std::string, unsigned> per_dir;
    unsigned long finds = 0, hits = 0, bad = 0;
    double t_maps = 0, t_walk = 0;
    std::vector<g3fs::FindData> fast, walk;
    auto timed = [](g3fs::FileSystem &fs, const std::string &p, std::vector<g3fs::FindData> *r, double *t) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = fs.find(p, r);
        *t += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return ok;
    };
    for (const auto &e : all) {
        if (per_dir[e.first]++ >= 1500)
            continue;
        const std::string &n = e.second;
        std::string upper(n), base = n.substr(0, n.find('.'));
        for (char &c : upper)
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
        for (const std::string &q : {n, upper, n + ".", base, base + ".", n + "q", lowered(n)}) {
            const std::string pattern = e.first + "/" + q;
            g3fs::linear_names = false;
            const bool f1 = timed(fs, pattern, &fast, &t_maps);
            g3fs::linear_names = true;
            const bool f2 = timed(fs, pattern, &walk, &t_walk);
            ++finds;
            hits += f1;
            if (f1 != f2 || !same(fast, walk)) {
                if (++bad <= 20)
                    printf("DIFF %s: maps %zu, walk %zu\n", pattern.c_str(), fast.size(), walk.size());
            }
        }
    }
    g3fs::linear_names = false;
    printf("names, ExtendedContent %d: %zu entries, %lu finds (%lu with results), %lu differ; maps %.1f s, walk %.1f s\n",
           extended ? 1 : 0, all.size(), finds, hits, bad, t_maps, t_walk);
    return bad ? 1 : 0;
}

int run_index(const std::string &root, bool extended) {
    PosixDisk disk;
    g3fs::FileSystem fs(disk);
    std::vector<std::string> virtuals, lines;
    if (int rc = mount(fs, root, extended, &virtuals))
        return rc;
    for (const auto &v : virtuals)
        walk(fs, v, &lines);
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    for (const auto &l : lines)
        printf("%s\n", l.c_str());
    return 0;
}

int failures;
void check(bool ok, const char *what) {
    if (!ok) {
        printf("FAIL %s\n", what);
        ++failures;
    }
}

// The write side on a temporary folder: the calls a save makes, and the original's refusals.
int scratch(const std::string &dir) {
    PosixDisk disk;
    g3fs::FileSystem fs(disk);
    mkdir(dir.c_str(), 0755);
    mkdir((dir + "/SaveGame").c_str(), 0755);
    check(fs.create_volume("G3"), "create volume");
    check(!fs.create_volume("g3"), "a second volume of the same name");
    check(!fs.create_volume("a/b"), "a volume name with a separator");
    check(fs.create_directory("#G3:/SaveGame"), "virtual folder");
    check(!fs.create_directory("#G3:/a/b"), "two levels at once");
    check(fs.mount_directory("#G3:/SaveGame", dir + "/SaveGame"), "mount");
    check(!fs.mount_directory("#G3:/SaveGame", dir + "/SaveGame"), "mount twice");
    check(!fs.mount_directory("#G3:/Missing", dir + "/SaveGame"), "mount on a missing folder");
    check(fs.create_directory("#G3:/SaveGame/slot1"), "a directory on disk");
    check(disk.attributes(dir + "/SaveGame/slot1") == 0x10u, "it is on disk");
    std::string rest;
    g3fs::Folder *f = fs.resolve("#G3:/SaveGame/slot1/save.dat", &rest);
    check(f && rest == "slot1/save.dat", "resolve");
    const uint32_t written = f ? f->data[4] : 0;
    { std::ofstream(dir + "/SaveGame/slot1/save.dat") << "x"; }
    check(fs.attributes("#G3:/SaveGame/slot1/save.dat") == 0x80u, "attributes from disk");
    check(fs.attributes("#G3:/SaveGame/slot1/none.dat") == 0xffffffffu, "attributes of nothing");
    check(fs.set_attributes("#G3:/SaveGame/slot1/save.dat", 1u), "read-only");
    check(fs.attributes("#G3:/SaveGame/slot1/save.dat") == 0x81u, "read-only seen");
    check(fs.set_attributes("#G3:/SaveGame/slot1/save.dat", 0x80u), "writable again");
    check(fs.copy_file("#G3:/SaveGame/slot1/save.dat", "#G3:/SaveGame/slot1/copy.dat", true), "copy");
    check(!fs.copy_file("#G3:/SaveGame/slot1/save.dat", "#G3:/SaveGame/slot1/copy.dat", true), "copy over");
    check(fs.move_file("#G3:/SaveGame/slot1/copy.dat", "#G3:/SaveGame/slot1/moved.dat"), "move");
    std::vector<g3fs::FindData> found;
    check(fs.find("#G3:/SaveGame/slot1/*.dat", &found) && found.size() == 2, "find two files");
    check(fs.delete_file("#G3:/SaveGame/slot1/moved.dat"), "delete");
    check(!fs.delete_file("#G3:/Data/x"), "delete outside a mount");
    check(fs.delete_file("#G3:/SaveGame/slot1/save.dat"), "delete the other");
    check(fs.remove_directory("#G3:/SaveGame/slot1"), "remove the directory");
    check(f && f->data[4] != written, "the folder's write time moved");
    check(!fs.remove_directory("#G3:/SaveGame"), "a mounted virtual folder stays");
    check(fs.create_directory("#G3:/Tmp") && fs.remove_directory("#G3:/Tmp"), "virtual create and remove");
    found.clear();
    check(fs.find("#G3:/*.*", &found) && found.size() == 1 && found[0].name == "SaveGame", "root lists its folder");
    rmdir((dir + "/SaveGame").c_str());
    rmdir(dir.c_str());
    printf("scratch: %d failures\n", failures);
    return failures ? 1 : 0;
}

// A stored entry through EntryBytes over the mapping, against `data` (pack_read_entry's bytes): the size, the whole
// read, reads in uneven pieces and past the end, then a write in the middle and one past the end, after which the
// entry holds its own bytes and the mapping still the archive's. Returns what failed, "" for nothing.
std::string check_mapped(const std::shared_ptr<g3fs::Pack> &pack, const g3fs::Node &node,
                         const std::vector<uint8_t> &data) {
    g3fs::EntryBytes e;
    if (!e.map(pack, node))
        return "does not map";
    const uint8_t *view = e.view();
    if (!view || view != g3fs::pack_entry_view(*pack, node) || e.size() != data.size())
        return "view or size";
    std::vector<uint8_t> got(data.size() + 7, 0xcd);
    if (e.read(0, got.data(), got.size()) != data.size() || std::memcmp(got.data(), data.data(), data.size()))
        return "whole read";
    static const size_t pieces[] = {1, 3, 4096, 65537};
    size_t at = 0;
    for (int i = 0; at < data.size(); ++i) {
        const size_t want = pieces[i % 4];
        const size_t k = e.read(at, got.data(), want);
        if (k != std::min(want, data.size() - at) || std::memcmp(got.data(), data.data() + at, k))
            return "piece at " + std::to_string(at);
        at += k;
    }
    if (e.read(data.size(), got.data(), 1) != 0 || e.read(data.size() + 5, got.data(), 1) != 0)
        return "read past the end";
    const size_t mid = data.size() / 2;
    const uint8_t mark[2] = {(uint8_t)~data[mid], 0x5a};
    e.write(mid, mark, 1);
    if (e.view() || e.size() != data.size())
        return "first write keeps the view";
    if (std::memcmp(view, data.data(), data.size()))
        return "write reached the mapping";
    std::vector<uint8_t> want = data;
    want[mid] = mark[0];
    e.write(data.size() + 3, mark + 1, 1); // grows; the gap is zero as the memory stream's is
    want.resize(data.size() + 4, 0);
    want.back() = mark[1];
    got.assign(want.size(), 0xcd);
    if (e.size() != want.size() || e.read(0, got.data(), got.size()) != want.size() || got != want)
        return "own bytes after the writes";
    return std::string();
}

// Every entry of the archives read as an open reads it (stored copied, zlib inflated), its length checked; a stored
// one also through the mapping (check_mapped).
int read_all(int n, char **paths) {
    unsigned long long entries = 0, bytes = 0, bad = 0, mapped = 0;
    double t_read = 0, t_map = 0;
    for (int i = 0; i < n; ++i) {
        auto pack = std::make_shared<g3fs::Pack>();
        pack->path = pack->host_path = paths[i];
        std::string error;
        if (!g3fs::pack_load(pack.get(), &error)) {
            printf("FAIL %s: %s\n", paths[i], error.c_str());
            ++bad;
            continue;
        }
        if (!pack->map) {
            printf("FAIL %s: not mapped\n", paths[i]);
            ++bad;
        }
        std::vector<uint8_t> data;
        for (const g3fs::Node &node : pack->nodes) {
            if (node.folder)
                continue;
            ++entries;
            const auto t0 = std::chrono::steady_clock::now();
            if (!g3fs::pack_read_entry(*pack, node, &data) || data.size() != node.size) {
                if (bad < 20)
                    printf("FAIL %s: %s\n", paths[i], node.path.c_str());
                ++bad;
                continue;
            }
            const auto t1 = std::chrono::steady_clock::now();
            t_read += std::chrono::duration<double>(t1 - t0).count();
            bytes += data.size();
            const bool stored = node.compression == 0 && node.encryption == 0 && node.size != 0;
            if (stored != (g3fs::pack_entry_view(*pack, node) != nullptr)) {
                if (bad < 20)
                    printf("FAIL %s: %s: view for a %s entry\n", paths[i], node.path.c_str(),
                           stored ? "stored" : "zlib or empty");
                ++bad;
                continue;
            }
            if (!stored)
                continue;
            ++mapped;
            const auto t2 = std::chrono::steady_clock::now();
            const std::string why = check_mapped(pack, node, data);
            t_map += std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();
            if (!why.empty()) {
                if (bad < 20)
                    printf("FAIL %s: %s: mapped, %s\n", paths[i], node.path.c_str(), why.c_str());
                ++bad;
            }
        }
    }
    printf("read: %llu entries (%llu stored, through the mapping too), %llu MB, %llu failures; "
           "read %.2f s, mapped checks %.2f s\n",
           entries, mapped, bytes >> 20, bad, t_read, t_map);
    return bad ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "read"))
        return read_all(argc - 2, argv + 2);
    if (argc == 4 && !strcmp(argv[1], "index"))
        return run_index(argv[2], argv[3][0] == '1');
    if (argc == 4 && !strcmp(argv[1], "names"))
        return run_names(argv[2], argv[3][0] == '1');
    if (argc == 3 && !strcmp(argv[1], "scratch"))
        return scratch(argv[2]);
    fprintf(stderr, "usage: filesystem_test index|names <game folder> <extended 0|1> | scratch <new folder>\n");
    return 2;
}
