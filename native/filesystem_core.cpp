// native/filesystem_core.cpp - see filesystem_core.h. The addresses name the FileSystem.dll function each piece
// answers as (analysis/FileSystem.dll); docs/perf/ladowanie-natywne.md part 1 has the walk through them.
#include "filesystem_core.h"

#include "third_party/libdeflate.h"

extern "C" int g3fs_name_fits(const char *pattern, const char *name); // filesystem_match.c

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace g3fs {

bool linear_names = false;

namespace {

char fold(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}
bool ieq(const std::string &a, const std::string &b) { // bCString::CompareNoCase == 0
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (fold(a[i]) != fold(b[i]))
            return false;
    return true;
}
bool iprefix(const std::string &s, const std::string &p) { // FindNoCase(s, p) == 0
    if (p.size() > s.size())
        return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (fold(s[i]) != fold(p[i]))
            return false;
    return true;
}
bool icontains(const std::string &s, const std::string &p) { // FindNoCase(s, p) >= 0
    if (p.empty())
        return true;
    for (size_t at = 0; at + p.size() <= s.size(); ++at) {
        size_t i = 0;
        while (i < p.size() && fold(s[at + i]) == fold(p[i]))
            ++i;
        if (i == p.size())
            return true;
    }
    return false;
}
bool any_of(const std::string &s, const char *set) { // bCString::ContainsOneOf
    return s.find_first_of(set) != std::string::npos;
}
std::string slashes(std::string s) { // Replace('\\', '/')
    for (char &c : s)
        if (c == '\\')
            c = '/';
    return s;
}
std::string with_slash(std::string s) { // the '/' every folder lookup appends
    s = slashes(std::move(s));
    if (!s.empty() && s.back() != '/')
        s += '/';
    return s;
}
std::string strip_slash(std::string s) { // 10da2820
    s = slashes(std::move(s));
    if (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}
std::string tail(const std::string &s, const std::string &head) { // bCString::Delete(0, head's length)
    return head.size() < s.size() ? s.substr(head.size()) : std::string();
}

// The name GetName gives: a file's after its last '/' (10da2a40), a folder's last component (10da2c60).
std::string file_name(const std::string &path) {
    const size_t at = path.rfind('/');
    return at == std::string::npos ? path : path.substr(at + 1);
}
std::string folder_name(const std::string &path) {
    const long last = (long)path.rfind('/');
    long prev = -1, at = -1;
    if (last > -1)
        do {
            prev = at;
            const size_t f = path.find('/', (size_t)(at + 1));
            at = f == std::string::npos ? -1 : (long)f;
        } while (at != -1 && at < last);
    const long from = prev + 1, count = last - prev - 1;
    if (from >= (long)path.size() || count <= 0)
        return count < 0 && from < (long)path.size() ? path.substr((size_t)from) : std::string();
    return path.substr((size_t)from, (size_t)count);
}

void set_now(uint32_t *data, uint64_t now) { // a new folder: created = accessed = written = now
    data[0] = data[2] = data[4] = (uint32_t)now;
    data[1] = data[3] = data[5] = (uint32_t)(now >> 32);
}

// ---- the virtual tree ----

Folder *contain(Folder *f, const std::string &p) { // 10da8ae0
    if (!iprefix(p, f->path))
        return nullptr;
    if (ieq(p, f->path))
        return f->parent;
    for (auto &k : f->kids)
        if (Folder *r = contain(k.get(), p))
            return r;
    return f;
}
Folder *exact_rec(Folder *f, const std::string &p) {
    if (!icontains(p, f->path))
        return nullptr;
    if (ieq(p, f->path))
        return f;
    for (auto &k : f->kids)
        if (Folder *r = exact_rec(k.get(), p))
            return r;
    return nullptr;
}
Folder *exact(Folder *f, const std::string &p) { // 10da8b50
    return exact_rec(f, with_slash(p));
}

void fill(const Folder &f, FindData *d) { // 10da2bc0 on a virtual folder
    std::memcpy(d->w, f.data, sizeof f.data);
    d->w[8] = d->w[9] = 0;
    d->w[10] = f.attributes;
    d->name = folder_name(f.path);
    d->physical.clear();
    d->filled = true;
}

// ---- an archive's tree ----

Node *deep(Node *n, const std::string &p) { // 10da41c0
    if (!n->path.empty() && !iprefix(p, n->path))
        return nullptr;
    if (ieq(p, n->path))
        return n->parent;
    for (Node *d : n->dirs)
        if (Node *r = deep(d, p))
            return r;
    return n;
}
Node *exact_dir_rec(Node *n, const std::string &p) {
    if (!n->path.empty() && !icontains(p, n->path))
        return nullptr;
    if (ieq(p, n->path))
        return n;
    for (Node *d : n->dirs)
        if (Node *r = exact_dir_rec(d, p))
            return r;
    return nullptr;
}
Node *exact_dir(Node *n, const std::string &p) { // 10da4240
    return exact_dir_rec(n, with_slash(p));
}
Node *file_in(Node *n, const std::string &path) { // 10da4140
    auto it = n->file_at.find(lower(strip_slash(path)));
    return it == n->file_at.end() ? nullptr : n->files[it->second];
}
Node *dir_in(Node *n, const std::string &path) { // 10da4080
    auto it = n->dir_at.find(lower(with_slash(path)));
    return it == n->dir_at.end() ? nullptr : n->dirs[it->second];
}
Node *entry_in(Node *f, const std::string &rest) { // 10da4380 and 10da4420: a folder by that path, else a file
    const std::string p = f->path + rest;
    if (Node *d = exact_dir(f, p))
        return d;
    Node *g = deep(f, p);
    return g ? file_in(g, p) : nullptr;
}
bool exists(Node *g, const std::string &path) { // 10da3f60
    const std::string p = with_slash(path);
    if (ieq(p, g->path + ".") || ieq(p, g->path + ".."))
        return true;
    if (dir_in(g, p))
        return true;
    return file_in(g, p) != nullptr;
}

// 10da2bc0 and 10da3040 on an archive's node. A node without a path fills nothing (its GetName fails).
void fill(const Node &n, FindData *d) {
    if (n.path.empty()) {
        d->filled = false;
        return;
    }
    d->name = n.name;
    std::memcpy(d->w, n.data, sizeof n.data);
    d->w[8] = d->w[9] = 0;
    d->w[10] = n.attributes;
    d->physical.clear();
    d->filled = true;
    if (!n.folder) {
        d->w[6] = (uint32_t)(n.size >> 32);
        d->w[7] = (uint32_t)n.size;
        d->physical = n.from;
    }
}

// The names a pattern without '*' and '?' fits (10da9d30 then compares the halves character by character, folded): its
// base and extension both whole, so the pattern itself, and with no extension also the name with a '.' after it. False
// where the walk has to answer: a wildcard, a half of 255 bytes or more (the original cuts it), an empty pattern.
bool plain_names(const std::string &pat, std::string names[2], int *count) {
    if (pat.find_first_of("*?") != std::string::npos)
        return false;
    const size_t dot = pat.find('.');
    const size_t base = dot == std::string::npos ? pat.size() : dot;
    const size_t ext = dot == std::string::npos ? 0 : pat.size() - dot - 1;
    if (base >= 255 || ext >= 255 || (base == 0 && ext == 0))
        return false;
    if (ext) {
        names[0] = pat;
        *count = 1;
    } else {
        names[0] = pat.substr(0, base);
        names[1] = names[0] + ".";
        *count = 2;
    }
    return true;
}

// One archive's part of a find (10da5770, 10da46f0, 10da4800): the results in the folder the rest names.
void pack_results(Pack &pack, const std::string &rest, std::vector<std::pair<FindData, std::string>> *out) {
    const std::string q = strip_slash(rest);
    Node *f = deep(pack.root(), q);
    if (!f)
        return;
    const std::string pat = tail(q, f->path);
    if (any_of(pat, "\\/:\"<>|"))
        return;
    const bool dots = f->parent && pat == "*.*";
    if (dots) {
        FindData d;
        fill(*f, &d);
        d.name = ".";
        out->push_back({d, f->path + d.name});
        FindData u;
        fill(*f->parent, &u);
        u.name = "..";
        out->push_back({u, f->path + u.name});
    }
    std::string names[2];
    int count = 0;
    if (!linear_names && f->exact_names && plain_names(pat, names, &count)) { // the walk's results, by key
        std::vector<size_t> at;
        for (int i = 0; i < count; ++i) {
            auto it = f->dir_at.find(lower(f->path + names[i] + "/"));
            if (it != f->dir_at.end())
                at.push_back(it->second);
        }
        std::sort(at.begin(), at.end());
        for (size_t k : at) {
            FindData r;
            fill(*f->dirs[k], &r);
            out->push_back({r, f->path + r.name});
        }
        at.clear();
        for (int i = 0; i < count; ++i) {
            auto it = f->file_at.find(lower(f->path + names[i]));
            if (it != f->file_at.end())
                at.push_back(it->second);
        }
        std::sort(at.begin(), at.end());
        for (size_t k : at) {
            FindData r;
            fill(*f->files[k], &r);
            out->push_back({r, f->path + r.name});
        }
        return;
    }
    for (Node *d : f->dirs)
        if (name_fits(pat, d->name)) {
            FindData r;
            fill(*d, &r);
            out->push_back({r, f->path + r.name});
        }
    for (Node *n : f->files)
        if (name_fits(pat, n->name)) {
            FindData r;
            fill(*n, &r);
            out->push_back({r, f->path + r.name});
        }
}

} // namespace

std::string lower(const std::string &s) {
    std::string r(s);
    for (char &c : r)
        c = fold(c);
    return r;
}

bool name_fits(const std::string &pattern, const std::string &name) {
    return g3fs_name_fits(pattern.c_str(), name.c_str()) != 0;
}

void Folder::touch(uint64_t now) {
    data[4] = (uint32_t)now;
    data[5] = (uint32_t)(now >> 32);
    data[2] = data[4];
    data[3] = data[5];
}

Pack::~Pack() {
    if (fd >= 0)
        close(fd);
}

// ---- archives ----

namespace {

struct Reader { // the tree in memory (10da5cd0) and the cursor of its stream
    const uint8_t *p;
    size_t n, at = 0;
    bool u32(uint32_t *v) {
        if (at + 4 > n)
            return false;
        std::memcpy(v, p + at, 4);
        at += 4;
        return true;
    }
    bool bytes(void *v, size_t k) {
        if (at + k > n)
            return false;
        std::memcpy(v, p + at, k);
        at += k;
        return true;
    }
    bool str(std::string *s) { // 10da2230: u32 length, then length + 1 bytes, the string up to its first NUL
        uint32_t len;
        if (!u32(&len))
            return false;
        s->clear();
        if (!len)
            return true;
        if (at + len + 1 > n)
            return false;
        const char *b = (const char *)p + at;
        s->assign(b, strnlen(b, len + 1));
        at += len + 1;
        return true;
    }
};

void index_child(Node *parent, Node *child) {
    if (child->path != parent->path + child->name + (child->folder ? "/" : ""))
        parent->exact_names = false;
    if (child->folder) {
        if (!parent->dir_at.emplace(lower(child->path), parent->dirs.size()).second)
            parent->exact_names = false;
        parent->dirs.push_back(child);
    } else {
        if (!parent->file_at.emplace(lower(child->path), parent->files.size()).second)
            parent->exact_names = false;
        parent->files.push_back(child);
    }
}

bool read_folder(Pack *pack, Reader &r, Node *f, int depth);

bool read_file(Pack *pack, Reader &r, Node *parent) { // 10da32f0
    pack->nodes.emplace_back();
    Node *n = &pack->nodes.back();
    uint8_t head[0x24], body[0x20];
    if (!r.bytes(head, sizeof head) || !r.bytes(body, sizeof body) || !r.str(&n->path) || !r.str(&n->from))
        return false;
    std::memcpy(n->data, head, 0x20);
    std::memcpy(&n->attributes, head + 0x20, 4);
    std::memcpy(&n->offset, body, 8);
    std::memcpy(&n->stored, body + 8, 8);
    std::memcpy(&n->size, body + 16, 8);
    std::memcpy(&n->encryption, body + 24, 4);
    std::memcpy(&n->compression, body + 28, 4);
    n->name = file_name(n->path);
    n->parent = parent;
    index_child(parent, n);
    return true;
}

bool read_folder(Pack *pack, Reader &r, Node *f, int depth) { // 10da49b0, 10da4be0, 10da4d80
    uint8_t head[0x24];
    if (depth > 256 || !r.bytes(head, sizeof head) || !r.str(&f->path))
        return false;
    f->folder = true;
    std::memcpy(f->data, head, 0x20);
    std::memcpy(&f->attributes, head + 0x20, 4);
    f->name = folder_name(f->path);
    uint32_t count;
    if (!r.u32(&count))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        pack->nodes.emplace_back();
        Node *d = &pack->nodes.back();
        d->parent = f;
        if (!read_folder(pack, r, d, depth + 1)) // what 10da3dd0 set before is read over
            return false;
        index_child(f, d);
    }
    if (!r.u32(&count))
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!read_file(pack, r, f))
            return false;
    return true;
}

} // namespace

bool pack_load(Pack *pack, std::string *error) { // 10da5830, 10da5a10, 10da5cd0
    pack->fd = pack->host_path.empty() ? -1 : open(pack->host_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (pack->fd < 0) {
        *error = "cannot open";
        return false;
    }
    struct stat st;
    if (fstat(pack->fd, &st) != 0) {
        *error = "cannot stat";
        return false;
    }
    pack->file_size = (uint64_t)st.st_size;
    uint8_t h[0x30];
    if (pread(pack->fd, h, sizeof h, 0) != (ssize_t)sizeof h) {
        *error = "short header";
        return false;
    }
    uint32_t scrambled;
    uint64_t tree, end;
    std::memcpy(&scrambled, h + 12, 4);
    std::memcpy(&tree, h + 32, 8);
    std::memcpy(&end, h + 40, 8);
    // the comment after the tree must read (10da5a10, 10da2230), then the tree [tree, end) goes into memory
    uint32_t comment = 0;
    if (end < tree || end > pack->file_size || end - tree > (1u << 30) ||
        pread(pack->fd, &comment, 4, (off_t)end) != 4) {
        *error = "bad header";
        return false;
    }
    if (comment > pack->file_size - end) { // a damaged or hostile archive: no allocation from its numbers
        *error = "bad comment";
        return false;
    }
    if (comment) {
        std::vector<uint8_t> c((size_t)comment + 1);
        if (pread(pack->fd, c.data(), c.size(), (off_t)(end + 4)) != (ssize_t)c.size()) {
            *error = "short comment";
            return false;
        }
    }
    std::vector<uint8_t> buf((size_t)(end - tree));
    if (!buf.empty() && pread(pack->fd, buf.data(), buf.size(), (off_t)tree) != (ssize_t)buf.size()) {
        *error = "short tree";
        return false;
    }
    if (scrambled == 1)
        for (size_t i = buf.size(); i != 0; --i) {
            uint8_t b = buf[i - 1];
            const uint8_t k = (uint8_t)i, s = (uint8_t)(i & 7);
            b = (uint8_t)(b - k);
            b = (uint8_t)((b >> s) | (b << ((8 - s) & 7)));
            buf[i - 1] = (uint8_t)(b ^ k);
        }
    pack->nodes.clear();
    pack->nodes.emplace_back();
    Reader r{buf.data(), buf.size()};
    if (!read_folder(pack, r, pack->root(), 0)) {
        *error = "bad tree";
        return false;
    }
    return true;
}

int64_t pack_pread(const Pack &pack, uint64_t at, void *buf, size_t n) {
    const ssize_t got = pread(pack.fd, buf, n, (off_t)at);
    return got < 0 ? -1 : (int64_t)got;
}

bool pack_read_entry(const Pack &pack, const Node &node, Bytes *out) { // 10da3230, 10da33c0
    out->clear();
    const uint32_t size = (uint32_t)node.size, stored = (uint32_t)node.stored;
    if (size == 0 && (node.size >> 32) == 0)
        return true;
    if (node.encryption != 0)
        return false;
    // the archive's numbers are checked before anything is allocated from them: the packed bytes must lie inside the
    // file, and an entry inflates to at most 1 GB (the guest has 4 GB in all)
    if ((node.size >> 32) || (node.stored >> 32) || node.offset > pack.file_size ||
        stored > pack.file_size - node.offset || size > (1u << 30))
        return false;
    Bytes packed(stored);
    if (pack_pread(pack, node.offset, packed.data(), stored) != (int64_t)stored)
        return false;
    if (node.compression == 0) {
        if (stored != size)
            return false;
        out->swap(packed);
        return true;
    }
    static thread_local libdeflate_decompressor *dec = libdeflate_alloc_decompressor();
    out->resize(size);
    if (!dec || libdeflate_zlib_decompress(dec, packed.data(), stored, out->data(), size, nullptr) !=
                    LIBDEFLATE_SUCCESS) {
        out->clear();
        return false;
    }
    return true;
}

bool pack_entry_stored(const Pack &pack, const Node &node) {
    return pack.fd >= 0 && node.encryption == 0 && node.compression == 0 && node.size != 0 &&
           node.size == node.stored && node.offset <= pack.file_size && node.stored <= pack.file_size - node.offset &&
           node.size <= (1u << 30);
}

bool EntryBytes::refer(const std::shared_ptr<Pack> &pack, const Node &node) {
    if (!pack_entry_stored(*pack, node))
        return false;
    pack_ = pack;
    node_ = &node;
    size_ = node.size;
    own_.clear();
    return true;
}

bool EntryBytes::load(const Pack &pack, const Node &node) {
    pack_.reset();
    node_ = nullptr;
    return pack_read_entry(pack, node, &own_);
}

bool EntryBytes::direct(uint64_t at, size_t n) const {
    if (!pack_ || at >= size_)
        return pack_ != nullptr; // past the end: nothing to read, nothing to read in
    const uint64_t k = std::min<uint64_t>(n, size_ - at);
    return k >= DIRECT || (at == 0 && k == size_);
}

size_t EntryBytes::read_archive(const Pack &pack, const Node &node, uint64_t at, void *dst, size_t n) {
    const size_t k = at < node.size ? (size_t)std::min<uint64_t>(n, node.size - at) : 0u;
    if (!k)
        return 0;
    const int64_t got = pack_pread(pack, node.offset + at, dst, k);
    return got > 0 ? (size_t)got : 0u;
}

void EntryBytes::adopt(Bytes &&whole) {
    if (!pack_ || whole.size() != size_)
        return;
    own_ = std::move(whole);
    pack_.reset();
    node_ = nullptr;
}

size_t EntryBytes::read(uint64_t at, void *dst, size_t n) const {
    if (pack_)
        return read_archive(*pack_, *node_, at, dst, n);
    const size_t k = at < own_.size() ? (size_t)std::min<uint64_t>(n, own_.size() - at) : 0u;
    if (k)
        std::memcpy(dst, own_.data() + at, k);
    return k;
}

bool EntryBytes::write(uint64_t at, const void *src, size_t n) {
    if (pack_) { // the first write: the entry's own bytes from here on, the archive left as it is
        if (!pack_read_entry(*pack_, *node_, &own_))
            return false;
        pack_.reset();
        node_ = nullptr;
    }
    const size_t old = own_.size();
    if (old < at + n) {
        own_.resize(at + n);
        if (at > old)
            std::memset(own_.data() + old, 0, at - old);
    }
    if (n)
        std::memcpy(own_.data() + at, src, n);
    return true;
}

// ---- the module ----

Volume *FileSystem::volume_of(const std::string &path) { // 10da1130
    const std::string p = slashes(path);
    for (auto &v : volumes_)
        if (iprefix(p, v->root->path))
            return v.get();
    return nullptr;
}

Folder *FileSystem::resolve(const std::string &path, std::string *rest) { // 10da9880
    Volume *v = volume_of(path);
    if (!v)
        return nullptr;
    const std::string p = slashes(path);
    Folder *f = contain(v->root.get(), p);
    if (f)
        *rest = tail(p, f->path);
    return f;
}

bool FileSystem::create_volume(const std::string &name) { // 10da1060, 10da97d0, 10da88c0
    if (name.empty())
        return false;
    for (auto &v : volumes_)
        if (ieq(v->name, name))
            return false;
    if (any_of(name, "\\/:*?\"<>|"))
        return false;
    auto v = std::make_unique<Volume>();
    v->name = name;
    v->root = std::make_unique<Folder>();
    v->root->path = with_slash("#" + name + ":");
    set_now(v->root->data, disk_.now());
    volumes_.push_back(std::move(v));
    return true;
}

bool FileSystem::create_directory(const std::string &path) { // 10da1a10, 10da9990, 10da8e30
    std::string rest;
    Folder *f = resolve(path, &rest);
    if (!f)
        return false;
    const uint64_t now = disk_.now();
    if (f->mounted) {
        if (!disk_.create_directory(f->physical + rest))
            return false;
        f->touch(now);
        return true;
    }
    if (!f->packs.empty() || exact(f, f->path + rest))
        return false;
    // 10da2890: one component, a '/' or '\' only at its end
    std::string name = rest;
    if (name.find(':') != std::string::npos)
        return false;
    const size_t sep = name.find_first_of("\\/");
    if (sep != std::string::npos) {
        if (sep != name.size() - 1)
            return false;
        name.pop_back();
    }
    if (any_of(name, "\\/:*?\"<>|") || name == "." || name == "..")
        return false;
    auto k = std::make_unique<Folder>(); // 10da8980
    k->path = with_slash(f->path + name);
    k->parent = f;
    set_now(k->data, now);
    f->kids.push_back(std::move(k));
    f->touch(now);
    return true;
}

bool FileSystem::remove_directory(const std::string &path) { // 10da1a40, 10da99f0, 10da8f70
    std::string rest;
    Folder *f = resolve(path, &rest);
    if (!f)
        return false;
    if (f->mounted) {
        if (!disk_.remove_directory(f->physical + rest))
            return false;
        f->touch(disk_.now());
        return true;
    }
    const std::string target = with_slash(f->path + rest);
    for (size_t i = 0; i < f->kids.size(); ++i) {
        if (f->kids[i]->path != target) // bCString::operator!=, case-sensitive
            continue;
        if (!f->kids[i]->kids.empty() || f->kids[i]->mounted)
            return false; // "Cannot delete folder containing children." / "Cannot delete mounted folder."
        f->kids.erase(f->kids.begin() + (long)i);
        f->touch(disk_.now());
        return true;
    }
    return false;
}

bool FileSystem::mount_directory(const std::string &path, const std::string &physical) { // 10da1a70, 10da8c50
    Volume *v = volume_of(path);
    Folder *f = v ? exact(v->root.get(), path) : nullptr;
    if (!f || !f->kids.empty() || f->mounted)
        return false;
    const std::string full = with_slash(disk_.full_path(physical)); // 10da8140
    const uint32_t a = disk_.attributes(full + "."); // 10da8380
    if (a == 0xffffffffu || !(a & 0x10))
        return false;
    f->mounted = true;
    f->physical = full;
    return true;
}

bool FileSystem::unmount_directory(const std::string &path) { // 10da1ab0
    Volume *v = volume_of(path);
    Folder *f = v ? exact(v->root.get(), path) : nullptr;
    if (!f)
        return false;
    f->mounted = false;
    f->physical.clear();
    return true;
}

// The archive's copy with every zlib entry unpacked (tools/g3stored.py), when there is one for this very file: the
// same tree, so a read of an entry there is a read, not a read and an inflate (session 22: 30-57 ms for a World
// Texture Mix texture inside the frame). The key is FNV-1a 64 of the real path, the size and the modification time in
// nanoseconds, each with a NUL after it, as g3stored.py computes it; a changed archive has another key.
static std::string stored_copy(const std::string &host_path) {
    char real[PATH_MAX];
    struct stat st;
    const char *home = getenv("HOME");
    if (!home || !realpath(host_path.c_str(), real) || stat(real, &st) != 0)
        return std::string();
    uint64_t h = 0xcbf29ce484222325ull;
    const std::string parts[3] = {real, std::to_string((long long)st.st_size),
                                  std::to_string((long long)st.st_mtimespec.tv_sec * 1000000000ll +
                                                 st.st_mtimespec.tv_nsec)};
    for (const std::string &part : parts) {
        for (unsigned char b : part)
            h = (h ^ b) * 0x100000001b3ull;
        h *= 0x100000001b3ull; // the NUL
    }
    char name[96];
    snprintf(name, sizeof name, "/Library/Caches/Gothic3Recomp/stored/%016llx.g3s", (unsigned long long)h);
    const std::string copy = std::string(home) + name;
    return access(copy.c_str(), R_OK) == 0 ? copy : std::string();
}

bool FileSystem::mount_pack(const std::string &path, const std::string &pack) { // 10da1b70, 10da63d0
    Volume *v = volume_of(path);
    Folder *f = v ? exact(v->root.get(), path) : nullptr;
    if (!f || !f->kids.empty())
        return false;
    auto p = std::make_shared<Pack>();
    p->path = pack;
    p->host_path = disk_.host_path(pack);
    std::string error;
    const std::string copy = stored_copy(p->host_path);
    if (!copy.empty()) {
        const std::string original = p->host_path;
        p->host_path = copy;
        if (pack_load(p.get(), &error)) {
            fprintf(stderr, "[g3fs] %s read from its unpacked copy %s\n", pack.c_str(), copy.c_str());
        } else {
            fprintf(stderr, "[g3fs] the unpacked copy %s of %s does not load (%s), the archive is read\n", copy.c_str(),
                    pack.c_str(), error.c_str());
            if (p->fd >= 0)
                close(p->fd);
            p->fd = -1;
            p->host_path = original;
        }
    }
    if (p->fd < 0 && !pack_load(p.get(), &error))
        return false;
    f->packs.push_back(std::move(p));
    return true;
}

bool FileSystem::unmount_packs(const std::string &path) { // 10da1bb0
    Volume *v = volume_of(path);
    Folder *f = v ? exact(v->root.get(), path) : nullptr;
    if (!f)
        return false;
    f->packs.clear();
    return true;
}

uint32_t FileSystem::attributes(const std::string &path) { // 10da1bf0, 10da98f0, 10da6530
    std::string rest;
    Folder *f = resolve(path, &rest);
    if (!f)
        return 0xffffffffu;
    if (f->mounted) {
        const uint32_t a = disk_.attributes(f->physical + rest);
        if (a != 0xffffffffu)
            return a;
    }
    const std::string p = slashes(rest);
    for (size_t i = f->packs.size(); i-- > 0;) {
        Node *g = deep(f->packs[i]->root(), p);
        if (!g)
            return 0; // the original breaks out with 0 here
        if (Node *n = entry_in(g, tail(p, g->path)))
            return n->attributes;
    }
    return 0xffffffffu;
}

bool FileSystem::set_attributes(const std::string &path, uint32_t attributes) { // 10da1240, 10da8dd0, 10da6600
    std::string rest;
    Folder *f = resolve(path, &rest);
    if (!f)
        return false;
    if (f->mounted && disk_.set_attributes(f->physical + rest, attributes))
        return true;
    const std::string p = slashes(rest);
    for (size_t i = f->packs.size(); i-- > 0;) {
        Node *g = deep(f->packs[i]->root(), p);
        if (!g)
            continue;
        if (Node *n = entry_in(g, tail(p, g->path))) {
            n->attributes = attributes;
            return true;
        }
    }
    return false;
}

bool FileSystem::copy_file(const std::string &from, const std::string &to, bool fail_if_exists) { // 10da12b0
    Volume *a = volume_of(from), *b = volume_of(to);
    if (!a || !b || a != b)
        return false; // "Cannot move files/folders accross volumes."
    std::string r1, r2;
    Folder *f = resolve(from, &r1), *g = resolve(to, &r2); // 10da9ab0
    if (!f || !g || f != g || !f->mounted)                 // 10da9260: "Cannot copy virtual folder."
        return false;
    if (!disk_.copy_file(f->physical + r1, f->physical + r2, fail_if_exists))
        return false;
    f->touch(disk_.now());
    return true;
}

bool FileSystem::move_file(const std::string &from, const std::string &to) { // 10da1320, 10da9bc0, 10da92e0
    Volume *a = volume_of(from), *b = volume_of(to);
    if (!a || !b || a != b)
        return false;
    std::string r1, r2;
    Folder *f = resolve(from, &r1), *g = resolve(to, &r2);
    if (!f || !g || f != g)
        return false;
    if (f->mounted) {
        if (!disk_.move_file(f->physical + r1, f->physical + r2))
            return false;
        f->touch(disk_.now());
        return true;
    }
    Folder *src = exact(f, f->path + r1);
    if (!src || exact(f, f->path + r2)) // "Virtual source folder does not exist." / "... target ... already exist."
        return false;
    // 10da8cf0: the folder and everything under it renamed, the folder's times = now
    if (!src->parent || any_of(r2, "\\/:*?\"<>|"))
        return false;
    struct Rename {
        static void run(Folder *k, const std::string &name, uint64_t now) {
            k->path = with_slash(k->parent->path + name);
            k->touch(now);
            for (auto &c : k->kids)
                run(c.get(), folder_name(c->path), now);
        }
    };
    Rename::run(src, r2, disk_.now());
    return true;
}

bool FileSystem::delete_file(const std::string &path) { // 10da1380, 10da91d0
    std::string rest;
    Folder *f = resolve(path, &rest);
    if (!f || !f->mounted) // "Cannot delete file within unmounted folder. (%s)"
        return false;
    if (!disk_.delete_file(f->physical + rest))
        return false;
    f->touch(disk_.now());
    return true;
}

PackHit FileSystem::pack_open(Folder *f, const std::string &rest) { // 10da66c0
    const std::string p = slashes(rest);
    for (size_t i = f->packs.size(); i-- > 0;) {
        Node *g = deep(f->packs[i]->root(), p);
        if (!g)
            continue;
        if (Node *n = file_in(g, g->path + tail(p, g->path)))
            return {f->packs[i], n};
    }
    return {};
}

bool FileSystem::find(const std::string &pattern, std::vector<FindData> *out) { // 10da9cd0, 10da93f0, 10da94e0
    out->clear();
    Volume *v = volume_of(pattern);
    if (!v)
        return false;
    const std::string p = strip_slash(pattern);
    Folder *f = contain(v->root.get(), p);
    if (!f)
        return false;
    const std::string rest = tail(p, f->path);
    // archives first (10da67f0, 10da6870): each result unless a later archive has the same path (10da64b0), or the
    // mounted directory has it on disk (10da8440)
    for (size_t i = 0; i < f->packs.size(); ++i) {
        std::vector<std::pair<FindData, std::string>> results;
        pack_results(*f->packs[i], rest, &results);
        for (auto &r : results) {
            bool later = false;
            for (size_t j = i + 1; j < f->packs.size() && !later; ++j) {
                Node *g = deep(f->packs[j]->root(), r.second);
                later = g && exists(g, r.second);
            }
            if (later)
                continue;
            if (f->mounted) {
                std::vector<FindData> on_disk;
                if (disk_.find(f->physical + r.second, &on_disk))
                    continue;
            }
            out->push_back(std::move(r.first));
        }
    }
    if (f->mounted) { // the disk; its end is the find's end (10da8680, 10da8700)
        std::vector<FindData> on_disk;
        if (disk_.find(f->physical + rest, &on_disk))
            for (size_t i = 0; i < on_disk.size(); ++i) {
                on_disk[i].physical = i == 0 ? f->physical + rest : f->physical;
                out->push_back(std::move(on_disk[i]));
            }
        return !out->empty();
    }
    if (f->parent) { // the virtual subfolders, "." and ".." first
        FindData d;
        fill(*f, &d);
        d.name = ".";
        out->push_back(d);
        FindData u;
        fill(*f->parent, &u);
        u.name = "..";
        out->push_back(u);
    }
    for (auto &k : f->kids)
        if (name_fits(rest, folder_name(k->path))) {
            FindData d;
            fill(*k, &d);
            out->push_back(d);
        }
    return !out->empty();
}

} // namespace g3fs
