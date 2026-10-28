// native/filesystem.cpp - FileSystem.dll replaced at its boundary (D-043; docs/perf/ladowanie-natywne.md part 1).
//
// Engine imports one function from FileSystem.dll, ?GetInstance@CFFFileSystemModule@@SGAAV1@XZ, and reaches
// everything else through the vtables of the objects it hands back: the module (IFFModuleFileSystem, 22 methods, and
// GetLastError on its primary vtable) and the files CreateFileA opens (IFFVirtualFile with its base and stream
// parts), which only Engine's eCVirtualFile touches. This file is the guest side of that boundary: the objects in
// guest memory with vtables of import trampolines, their arguments read and their results written, bCStrings set
// through SharedBase's exports. What the module answers comes from filesystem_core.cpp, on the host.
//
// Guest objects, laid out the way Engine computes its way through them:
//   module M: +0x00 primary vtable, +0x04 -> {0, 0x1c, 0x20}, so M + [[M+4]+8] + 4 = M+0x24, +0x24 the module vtable
//   file F:   +0x00 file vtable, +0x04 -> {0, 4, 8} (base F+8, stream F+12), +0x08 base vtable, +0x0c stream vtable
//
// Threads: fmodex runs as a free thread and reads its streams through these objects beside the game thread, so every
// entry point may run at once with another. The core sits behind g_lock; nothing under it calls guest code (SharedBase
// takes guest critical sections) or lends the baton. The physical side calls the kit's Win32 through the trampolines
// FileSystem.dll's own imports got, so the profile tiers, the last error and the lister's order stay what they were.
#include "filesystem_core.h"

#include "runtime/imports.h"
#include "runtime/loader.h"
#include "runtime/memory.h"
#include "runtime/win32.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

extern "C" bool recomp_host_work_begin();
extern "C" void recomp_host_work_end();
extern "C" bool recomp_cur_is_main_thread();

namespace {

const char *const FS_DLL = "FileSystem.dll";
// An archive read or inflate this big off the main thread lends the baton. The image cache's loader thread held it
// 44-66 ms a texture while the game thread waited (player's game 28.10, 23 slow frames in wait yield), and textures
// are mostly under 1 MB, k_ReadFile's bound. Taking the baton back can cost the loader up to a frame, so the smallest
// entries (an inflate of about 0.1 ms) keep it.
constexpr uint32_t LEND = 64u << 10;

thread_local X86 *t_c; // the guest context of the call in progress on this thread

// ---- guest helpers ----

// Guest memory below ESP for the duration of one call: strings and out-parameters for Win32 and SharedBase.
struct Scratch {
    X86 *c;
    uint32_t esp;
    explicit Scratch(X86 *cx) : c(cx), esp(cx->r[R_ESP]) {}
    ~Scratch() { c->r[R_ESP] = esp; }
    uint32_t take(uint32_t n) {
        c->r[R_ESP] = (c->r[R_ESP] - n) & ~15u;
        return c->r[R_ESP];
    }
    uint32_t str(const std::string &s) {
        const uint32_t a = take((uint32_t)s.size() + 1u);
        std::memcpy(g_mem + a, s.c_str(), s.size() + 1);
        return a;
    }
};

uint32_t k32(const char *name) {
    static std::mutex m;
    static std::unordered_map<std::string, uint32_t> cache;
    std::lock_guard<std::mutex> g(m);
    auto it = cache.find(name);
    if (it != cache.end())
        return it->second;
    const uint32_t t = imports_resolve("KERNEL32.dll", name);
    cache.emplace(name, t);
    return t;
}
uint32_t win(X86 *c, const char *name, std::initializer_list<uint32_t> args) {
    return guest_call(c, k32(name), args.begin(), (int)args.size());
}

uint32_t sharedbase(const char *name) {
    LoaderModule *m = loader_module_named("SharedBase.dll");
    return m ? loader_module_export(*m, name) : 0;
}

// A bCString is a pointer to its characters, the length at -8; 0 is the empty string.
std::string read_str(uint32_t obj) {
    if (!gm_valid(obj, 4))
        return std::string();
    const uint32_t d = rd32(obj);
    if (!d || !gm_valid(d - 8u, 8))
        return std::string();
    const uint32_t n = rd32(d - 8u);
    return gm_valid(d, n) ? std::string((const char *)g_mem + d, n) : std::string();
}
void set_str(X86 *c, uint32_t obj, const std::string &s) { // bCString::operator=(char const *)
    static const uint32_t assign = sharedbase("??4bCString@@QAEAAV0@PBD@Z");
    Scratch sc(c);
    const uint32_t text = sc.str(s);
    c->r[R_ECX] = obj;
    guest_call(c, assign, &text, 1);
}
void clear_str(X86 *c, uint32_t obj) {
    static const uint32_t clear = sharedbase("?Clear@bCString@@QAEXXZ");
    c->r[R_ECX] = obj;
    guest_call(c, clear, nullptr, 0);
}

void ret(X86 *c, uint32_t v) {
    c->r[R_EAX] = v;
}
void ret64(X86 *c, uint64_t v) {
    c->r[R_EAX] = (uint32_t)v;
    c->r[R_EDX] = (uint32_t)(v >> 32);
}

// ---- the physical side: the kit's Win32, as FileSystem.dll called it ----

class GuestDisk : public g3fs::Disk {
  public:
    uint32_t attributes(const std::string &path) override {
        Scratch s(t_c);
        return win(t_c, "GetFileAttributesA", {s.str(path)});
    }
    bool set_attributes(const std::string &path, uint32_t a) override {
        Scratch s(t_c);
        return win(t_c, "SetFileAttributesA", {s.str(path), a}) == 1;
    }
    bool find(const std::string &pattern, std::vector<g3fs::FindData> *out) override { // 10da9f00, 10da9f90
        Scratch s(t_c);
        const uint32_t data = s.take(0x140);
        const uint32_t h = win(t_c, "FindFirstFileA", {s.str(pattern), data});
        if (h == 0xffffffffu)
            return false;
        do {
            g3fs::FindData d;
            d.w[10] = rd32(data);
            for (int i = 0; i < 10; ++i)
                d.w[i] = rd32(data + 4u + 4u * (uint32_t)i);
            d.name = gm_str(data + 0x2cu, 260);
            out->push_back(std::move(d));
        } while (win(t_c, "FindNextFileA", {h, data}) == 1);
        win(t_c, "FindClose", {h});
        return true;
    }
    bool create_directory(const std::string &path) override {
        Scratch s(t_c);
        return win(t_c, "CreateDirectoryA", {s.str(path), 0}) == 1;
    }
    bool remove_directory(const std::string &path) override {
        Scratch s(t_c);
        return win(t_c, "RemoveDirectoryA", {s.str(path)}) == 1;
    }
    bool delete_file(const std::string &path) override {
        Scratch s(t_c);
        return win(t_c, "DeleteFileA", {s.str(path)}) == 1;
    }
    bool copy_file(const std::string &from, const std::string &to, bool fail_if_exists) override {
        Scratch s(t_c);
        const uint32_t a = s.str(from), b = s.str(to);
        return win(t_c, "CopyFileA", {a, b, fail_if_exists ? 1u : 0u}) == 1;
    }
    bool move_file(const std::string &from, const std::string &to) override {
        Scratch s(t_c);
        const uint32_t a = s.str(from), b = s.str(to);
        return win(t_c, "MoveFileExA", {a, b, 2}) == 1;
    }
    std::string full_path(const std::string &path) override { // the CRT's _fullpath: GetFullPathNameA, 260 bytes
        Scratch s(t_c);
        const uint32_t in = s.str(path), buf = s.take(0x108);
        const uint32_t n = win(t_c, "GetFullPathNameA", {in, 0x104, buf, 0});
        return n && n < 0x104 ? gm_str(buf, 0x104) : std::string();
    }
    std::string host_path(const std::string &path) override {
        return win32_host_path_op(path, WIN32_FILE_READ);
    }
    uint64_t now() override { // GetSystemTime + SystemTimeToFileTime: milliseconds since 1601
        using namespace std::chrono;
        const int64_t ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        return (uint64_t)(ms + 11644473600000ll) * 10000u;
    }
};

GuestDisk g_disk;
g3fs::FileSystem g_fs(g_disk);
std::mutex g_lock; // the core, the find handles, the buffered entries

// ---- files ----

struct Shared { // a buffered entry's bytes, shared by its open streams as the entry's memory stream is
    std::mutex m;
    std::vector<uint8_t> bytes;
};
std::unordered_map<const g3fs::Node *, std::weak_ptr<Shared>> g_buffered;

struct File {
    std::atomic<uint32_t> refs{1};
    enum Kind { PHYSICAL, BUFFERED, DIRECT } kind = PHYSICAL;
    uint32_t access = 0;
    // physical: the Win32 handle, the path it was opened by and the virtual one
    uint32_t handle = 0xffffffffu;
    std::string physical, name;
    // from an archive
    std::shared_ptr<g3fs::Pack> pack;
    g3fs::Node *node = nullptr;
    std::shared_ptr<Shared> mem;
    std::mutex pos_m;
    uint64_t pos = 0;
};
std::mutex g_files_lock;
std::unordered_map<uint32_t, std::shared_ptr<File>> g_files; // by the file interface's guest address

uint32_t g_vb_file, g_vt_file, g_vt_base, g_vt_stream; // shared by every file object

std::shared_ptr<File> file_at(uint32_t f) {
    std::lock_guard<std::mutex> g(g_files_lock);
    auto it = g_files.find(f);
    return it == g_files.end() ? nullptr : it->second;
}
uint32_t file_new(std::shared_ptr<File> file) {
    const uint32_t f = heap_alloc_kit(16, true);
    if (!f)
        return 0;
    wr32(f + 0u, g_vt_file);
    wr32(f + 4u, g_vb_file);
    wr32(f + 8u, g_vt_base);
    wr32(f + 12u, g_vt_stream);
    std::lock_guard<std::mutex> g(g_files_lock);
    g_files[f] = std::move(file);
    return f;
}

// ECX of each part -> the file interface
uint32_t of_file(X86 *c) {
    return c->r[R_ECX];
}
uint32_t of_base(X86 *c) {
    return c->r[R_ECX] - 8u;
}
uint32_t of_stream(X86 *c) {
    return c->r[R_ECX] - 12u;
}

struct Enter { // a shim's entry: the context the physical side's calls run in
    X86 *prev;
    explicit Enter(X86 *c) : prev(t_c) { t_c = c; }
    ~Enter() { t_c = prev; }
};

// base: AddRef, Release, QueryInterface
void f_addref(X86 *c) {
    auto f = file_at(of_base(c));
    ret(c, f ? ++f->refs : 0);
}
void f_release(X86 *c) {
    Enter e(c);
    const uint32_t addr = of_base(c);
    auto f = file_at(addr);
    if (!f) {
        ret(c, 0);
        return;
    }
    const uint32_t left = --f->refs;
    if (left == 0) {
        {
            std::lock_guard<std::mutex> g(g_files_lock);
            g_files.erase(addr);
        }
        if (f->kind == File::PHYSICAL && f->handle != 0xffffffffu)
            win(c, "CloseHandle", {f->handle});
        heap_free(addr);
    }
    ret(c, left);
}
void f_query(X86 *c) {
    if (gm_valid(arg(c, 1), 4))
        wr32(arg(c, 1), 0);
    ret(c, 0);
}

uint64_t length_of(X86 *c, File &f) {
    if (f.kind == File::PHYSICAL) {
        Scratch s(c);
        const uint32_t li = s.take(8);
        if (f.handle != 0xffffffffu && win(c, "GetFileSizeEx", {f.handle, li}) == 1)
            return rd32(li) | (uint64_t)rd32(li + 4u) << 32;
        return 0;
    }
    if (f.kind == File::BUFFERED) {
        std::lock_guard<std::mutex> g(f.mem->m);
        return f.mem->bytes.size();
    }
    return f.node->stored;
}

// stream: GetLength, GetPosition, Seek, Read, Write
void s_length(X86 *c) {
    Enter e(c);
    auto f = file_at(of_stream(c));
    ret64(c, f ? length_of(c, *f) : 0);
}
void s_position(X86 *c) { // (int64 *)
    Enter e(c);
    auto f = file_at(of_stream(c));
    const uint32_t out = arg(c, 0);
    if (!f) {
        ret(c, 0);
        return;
    }
    if (f->kind == File::PHYSICAL) {
        ret(c, f->handle != 0xffffffffu && win(c, "SetFilePointerEx", {f->handle, 0, 0, out, 1}) == 1);
        return;
    }
    std::lock_guard<std::mutex> g(f->pos_m);
    wr32(out, (uint32_t)f->pos);
    wr32(out + 4u, (uint32_t)(f->pos >> 32));
    ret(c, 1);
}
void s_seek(X86 *c) { // (lo, hi, origin)
    Enter e(c);
    auto f = file_at(of_stream(c));
    const uint32_t lo = arg(c, 0), hi = arg(c, 1), origin = arg(c, 2);
    if (!f) {
        ret(c, 0);
        return;
    }
    if (f->kind == File::PHYSICAL) {
        ret(c, f->handle != 0xffffffffu && origin <= 2 &&
                   win(c, "SetFilePointerEx", {f->handle, lo, hi, 0, origin}) == 1);
        return;
    }
    // 10da7560: origins 0 and 1 both from the start, 2 from the end, any other to 0
    const int64_t len = (int64_t)length_of(c, *f), off = (int64_t)((uint64_t)hi << 32 | lo);
    int64_t to = 0;
    if (origin == 0 || origin == 1)
        to = off;
    else if (origin == 2)
        to = len - off;
    if ((origin <= 2 && to < 0) || to > len) {
        ret(c, 0);
        return;
    }
    std::lock_guard<std::mutex> g(f->pos_m);
    f->pos = (uint64_t)to;
    ret(c, 1);
}
void s_read(X86 *c) { // (buf, n)
    Enter e(c);
    auto f = file_at(of_stream(c));
    const uint32_t buf = arg(c, 0), n = arg(c, 1);
    if (!f || !gm_valid(buf, n)) {
        ret(c, 0);
        return;
    }
    if (f->kind == File::PHYSICAL) {
        Scratch s(c);
        const uint32_t got = s.take(4);
        wr32(got, 0);
        ret(c, f->handle != 0xffffffffu && win(c, "ReadFile", {f->handle, buf, n, got, 0}) == 1 ? rd32(got) : 0);
        return;
    }
    if (f->access == 2) {
        ret(c, 0);
        return;
    }
    std::unique_lock<std::mutex> g(f->pos_m);
    if (f->kind == File::BUFFERED) { // 10da6d90 on the memory stream: up to its end
        std::lock_guard<std::mutex> m(f->mem->m);
        const uint64_t size = f->mem->bytes.size();
        const uint32_t k = f->pos < size ? (uint32_t)std::min<uint64_t>(n, size - f->pos) : 0u;
        std::memcpy(g_mem + buf, f->mem->bytes.data() + f->pos, k);
        f->pos += k;
        ret(c, k);
        return;
    }
    // 10da7150: from the archive at the entry's offset, while the position is inside it, not cut at its end
    if (f->pos >= f->node->stored) {
        ret(c, 0);
        return;
    }
    // A lending read lets the position go first: taking the baton back waits for whoever holds it, and that thread may
    // be waiting for this file.
    const uint64_t at = f->pos;
    bool lend = false;
    if (n >= LEND && !recomp_cur_is_main_thread()) {
        g.unlock();
        lend = recomp_host_work_begin();
        if (!lend)
            g.lock();
    }
    const int64_t got = g3fs::pack_pread(*f->pack, f->node->offset + at, g_mem + buf, n);
    if (lend) {
        recomp_host_work_end();
        g.lock();
    }
    const uint32_t k = got > 0 ? (uint32_t)got : 0u;
    f->pos = at + k;
    ret(c, k);
}
void s_write(X86 *c) { // (buf, n)
    Enter e(c);
    auto f = file_at(of_stream(c));
    const uint32_t buf = arg(c, 0), n = arg(c, 1);
    if (!f || !gm_valid(buf, n)) {
        ret(c, 0);
        return;
    }
    if (f->kind == File::PHYSICAL) {
        Scratch s(c);
        const uint32_t put = s.take(4);
        wr32(put, 0);
        ret(c, f->handle != 0xffffffffu && win(c, "WriteFile", {f->handle, buf, n, put, 0}) == 1 ? rd32(put) : 0);
        return;
    }
    if (f->access == 1 || f->kind == File::DIRECT) { // the direct stream writes the archive, opened for reading
        ret(c, 0);
        return;
    }
    std::lock_guard<std::mutex> g(f->pos_m); // 10da6df0 on the memory stream, which grows
    std::lock_guard<std::mutex> m(f->mem->m);
    if (f->mem->bytes.size() < f->pos + n)
        f->mem->bytes.resize(f->pos + n);
    std::memcpy(f->mem->bytes.data() + f->pos, g_mem + buf, n);
    f->pos += n;
    ret(c, n);
}

// file: GetAttributes, GetFileName, GetPhysicalPath, three times, SetAttributes
void p_attributes(X86 *c) {
    Enter e(c);
    auto f = file_at(of_file(c));
    if (!f) {
        ret(c, 0xffffffffu);
        return;
    }
    if (f->kind == File::PHYSICAL) {
        Scratch s(c);
        ret(c, win(c, "GetFileAttributesA", {s.str(f->physical)}));
        return;
    }
    std::lock_guard<std::mutex> g(g_lock);
    ret(c, f->node->attributes);
}
void p_name(X86 *c, bool physical) { // (bCString &)
    Enter e(c);
    auto f = file_at(of_file(c));
    if (!f) {
        ret(c, 0);
        return;
    }
    if (f->kind != File::PHYSICAL) { // 10da7610: cleared, false
        clear_str(c, arg(c, 0));
        ret(c, 0);
        return;
    }
    set_str(c, arg(c, 0), physical ? f->physical : f->name);
    ret(c, 1);
}
void p_file_name(X86 *c) {
    p_name(c, false);
}
void p_physical_path(X86 *c) {
    p_name(c, true);
}
// which: 0 created, 1 last written, 2 last accessed (the physical file's GetFileTime arguments, the entry's fields)
void p_time(X86 *c, int which) { // (FILETIME *)
    Enter e(c);
    auto f = file_at(of_file(c));
    const uint32_t out = arg(c, 0);
    if (!f) {
        ret(c, 0);
        return;
    }
    if (f->kind == File::PHYSICAL) {
        ret(c, win(c, "GetFileTime", {f->handle, which == 0 ? out : 0u, which == 2 ? out : 0u,
                                      which == 1 ? out : 0u}) == 1);
        return;
    }
    static const int field[3] = {0, 4, 2};
    std::lock_guard<std::mutex> g(g_lock);
    wr32(out, f->node->data[field[which]]);
    wr32(out + 4u, f->node->data[field[which] + 1]);
    ret(c, 1);
}
void p_created(X86 *c) {
    p_time(c, 0);
}
void p_written(X86 *c) {
    p_time(c, 1);
}
void p_accessed(X86 *c) {
    p_time(c, 2);
}
void p_set_attributes(X86 *c) { // (DWORD)
    Enter e(c);
    auto f = file_at(of_file(c));
    if (!f) {
        ret(c, 0);
        return;
    }
    if (f->kind == File::PHYSICAL) {
        Scratch s(c);
        ret(c, win(c, "SetFileAttributesA", {s.str(f->physical), arg(c, 0)}) == 1);
        return;
    }
    std::lock_guard<std::mutex> g(g_lock);
    f->node->attributes = arg(c, 0);
    ret(c, 1);
}

// ---- the module ----

uint32_t g_module;
struct Find {
    std::vector<g3fs::FindData> results;
    size_t next = 0;
};
std::unordered_map<uint32_t, std::unique_ptr<Find>> g_finds;
uint32_t g_next_find = 0x1000;

void put_find(X86 *c, uint32_t out, const g3fs::FindData &d) { // into SFFFileFind
    if (d.filled)
        for (uint32_t i = 0; i < 11; ++i)
            wr32(out + 4u * i, d.w[i]);
    set_str(c, out + 0x2cu, d.name);
    if (d.filled)
        set_str(c, out + 0x30u, d.physical);
}

void m_volume_count(X86 *c) {
    std::lock_guard<std::mutex> g(g_lock);
    ret(c, (uint32_t)g_fs.volume_count());
}
void m_unused(X86 *c, const char *what) {
    log_once(what, "FileSystem: %s called; the game's modules never call it, the native module answers false", what);
    ret(c, 0);
}
void m_volume_root(X86 *c) {
    m_unused(c, "GetVolumeRootPath");
}
void m_mount_info(X86 *c) {
    m_unused(c, "GetMountInformation");
}
void m_create_pack(X86 *c) {
    m_unused(c, "CreatePackFile");
}
void m_release_pack(X86 *c) {
    m_unused(c, "ReleasePackFile");
}
void m_on_create(X86 *c) {
    m_unused(c, "CFFFileSystemModule primary vtable");
}
void m_current_dir(X86 *c) { // 10da19d0, 10da19f0: not implemented in the original either
    m_unused(c, "Get/SetCurrentDirectoryA");
}

// One core call under the lock with the path argument(s) read first.
template <class F> void m_path(X86 *c, F op) {
    Enter e(c);
    const std::string p = read_str(arg(c, 0));
    std::lock_guard<std::mutex> g(g_lock);
    ret(c, op(p));
}
void m_create_volume(X86 *c) {
    m_path(c, [](const std::string &p) { return (uint32_t)g_fs.create_volume(p); });
}
void m_create_directory(X86 *c) {
    m_path(c, [](const std::string &p) { return (uint32_t)g_fs.create_directory(p); });
}
void m_remove_directory(X86 *c) {
    m_path(c, [](const std::string &p) { return (uint32_t)g_fs.remove_directory(p); });
}
void m_unmount_directory(X86 *c) {
    m_path(c, [](const std::string &p) { return (uint32_t)g_fs.unmount_directory(p); });
}
void m_unmount_packs(X86 *c) {
    m_path(c, [](const std::string &p) { return (uint32_t)g_fs.unmount_packs(p); });
}
void m_attributes(X86 *c) {
    m_path(c, [](const std::string &p) { return g_fs.attributes(p); });
}
void m_delete_file(X86 *c) {
    m_path(c, [](const std::string &p) { return (uint32_t)g_fs.delete_file(p); });
}
void m_set_attributes(X86 *c) {
    const uint32_t a = arg(c, 1);
    m_path(c, [a](const std::string &p) { return (uint32_t)g_fs.set_attributes(p, a); });
}
void m_mount_directory(X86 *c) {
    const std::string phys = read_str(arg(c, 1));
    m_path(c, [&phys](const std::string &p) { return (uint32_t)g_fs.mount_directory(p, phys); });
}
void m_mount_pack(X86 *c) {
    const std::string pack = read_str(arg(c, 1));
    m_path(c, [&pack](const std::string &p) {
        const bool ok = g_fs.mount_pack(p, pack);
        if (!ok)
            LOGV("FileSystem: MountPackFile %s %s failed", p.c_str(), pack.c_str());
        return (uint32_t)ok;
    });
}
void m_copy(X86 *c) {
    const std::string to = read_str(arg(c, 1));
    const bool fail = (arg(c, 2) & 0xffu) != 0;
    m_path(c, [&to, fail](const std::string &p) { return (uint32_t)g_fs.copy_file(p, to, fail); });
}
void m_move(X86 *c) {
    const std::string to = read_str(arg(c, 1));
    m_path(c, [&to](const std::string &p) { return (uint32_t)g_fs.move_file(p, to); });
}

void m_find_first(X86 *c) { // (bCString const &, SFFFileFind &) -> handle
    Enter e(c);
    const std::string p = read_str(arg(c, 0));
    const uint32_t out = arg(c, 1);
    auto f = std::make_unique<Find>();
    uint32_t h = 0;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (!g_fs.find(p, &f->results)) {
            ret(c, 0);
            return;
        }
        f->next = 1;
        h = g_next_find++;
        if (!h)
            h = g_next_find++;
        g_finds[h] = std::move(f);
    }
    g3fs::FindData first;
    {
        std::lock_guard<std::mutex> g(g_lock);
        first = g_finds[h]->results[0];
    }
    put_find(c, out, first);
    ret(c, h);
}
void m_find_next(X86 *c) { // (handle, SFFFileFind &)
    Enter e(c);
    const uint32_t h = arg(c, 0), out = arg(c, 1);
    g3fs::FindData d;
    {
        std::lock_guard<std::mutex> g(g_lock);
        auto it = g_finds.find(h);
        if (it == g_finds.end() || it->second->next >= it->second->results.size()) {
            ret(c, 0);
            return;
        }
        d = it->second->results[it->second->next++];
    }
    put_find(c, out, d);
    ret(c, 1);
}
void m_find_close(X86 *c) { // (handle)
    std::lock_guard<std::mutex> g(g_lock);
    ret(c, g_finds.erase(arg(c, 0)) ? 1u : 0u);
}

// CreateFileA(path, EFFFileCreate, EFFFileAccess, flags): 10da9a40, 10da9150, 10da84b0, 10da7a80, 10da66c0
void m_create_file(X86 *c) {
    Enter e(c);
    const std::string path = read_str(arg(c, 0));
    const uint32_t create = arg(c, 1), access = arg(c, 2), flags = arg(c, 3);
    std::string rest, folder_path, physical;
    bool mounted = false;
    g3fs::Folder *folder;
    {
        std::lock_guard<std::mutex> g(g_lock);
        folder = g_fs.resolve(path, &rest);
        if (!folder) {
            ret(c, 0);
            return;
        }
        folder_path = folder->path;
        mounted = folder->mounted;
        physical = folder->physical;
    }
    auto file = std::make_shared<File>();
    file->access = access;
    bool opened = false;
    if (mounted && !(flags & 0x10u) && !(flags & 0x10000u) && !(flags & 0x8000u)) {
        static const uint32_t disposition[6] = {0, 2, 1, 4, 3, 5}; // EFFFileCreate 1..5 -> Win32
        const uint32_t want = access == 1 ? 0x80000000u : access == 2 ? 0x40000000u : 0xc0000000u;
        Scratch s(c);
        file->physical = physical + rest;
        const uint32_t h = win(c, "CreateFileA", {s.str(file->physical), want, 3, 0,
                                                  create <= 5 ? disposition[create] : 0u, flags | 0x10000000u, 0});
        if (h != 0xffffffffu) {
            file->kind = File::PHYSICAL;
            file->handle = h;
            file->name = folder_path + rest;
            opened = true;
        }
    }
    if (!opened) {
        g3fs::PackHit hit;
        std::shared_ptr<Shared> mem;
        {
            std::lock_guard<std::mutex> g(g_lock);
            hit = g_fs.pack_open(folder, rest);
            if (hit.node) {
                auto it = g_buffered.find(hit.node);
                if (it != g_buffered.end())
                    mem = it->second.lock();
            }
        }
        if (!hit.node) {
            ret(c, 0);
            return;
        }
        file->pack = hit.pack;
        file->node = hit.node;
        if (hit.node->attributes & 0x40000u) { // 10da3b20, 10da7080
            if (hit.node->encryption || hit.node->compression) {
                ret(c, 0);
                return;
            }
            file->kind = File::DIRECT;
        } else { // 10da3a50, 10da3230
            if (!mem) {
                auto fresh = std::make_shared<Shared>();
                const bool lend =
                    hit.node->size >= LEND && !recomp_cur_is_main_thread() && recomp_host_work_begin();
                const auto t0 = std::chrono::steady_clock::now();
                const bool ok = g3fs::pack_read_entry(*hit.pack, *hit.node, &fresh->bytes);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (lend)
                    recomp_host_work_end();
                // An entry that costs a frame (session 21: up to 275 ms of inflate under Engine's Prefetch on the
                // game thread): which one, from which archive, how big.
                if (ms >= 20.0)
                    fprintf(stderr, "[g3fs] read %s took %.1f ms (%.1f MB packed, %.1f MB, %s, %s thread)\n",
                            path.c_str(), ms, hit.node->stored / 1048576.0, hit.node->size / 1048576.0,
                            hit.pack->path.c_str(), recomp_cur_is_main_thread() ? "main" : "other");
                if (!ok) {
                    ret(c, 0);
                    return;
                }
                std::lock_guard<std::mutex> g(g_lock);
                auto &slot = g_buffered[hit.node];
                mem = slot.lock();
                if (!mem) {
                    slot = fresh;
                    mem = fresh;
                }
            }
            file->kind = File::BUFFERED;
            file->mem = mem;
        }
    }
    if (create == 1 || create == 2) {
        std::lock_guard<std::mutex> g(g_lock);
        folder->touch(g_disk.now());
    }
    ret(c, file_new(std::move(file)));
}

void m_last_error(X86 *c) {
    ret(c, win(c, "GetLastError", {}));
}

uint32_t vtable(const char *iface, const std::vector<std::pair<void (*)(X86 *), uint8_t>> &slots,
                const char *const *names) {
    const uint32_t vt = heap_alloc_kit((uint32_t)(4 * slots.size()), true);
    for (size_t i = 0; i < slots.size(); ++i) {
        const std::string name = std::string(iface) + "::" + names[i];
        wr32(vt + 4u * (uint32_t)i, imports_alloc_trampoline(FS_DLL, name.c_str(), slots[i].first, slots[i].second));
    }
    return vt;
}
uint32_t table3(uint32_t a, uint32_t b, uint32_t c) {
    const uint32_t t = heap_alloc_kit(12, true);
    wr32(t, a);
    wr32(t + 4u, b);
    wr32(t + 8u, c);
    return t;
}

void build() {
    static const char *const module_names[] = {
        "GetVolumeCount", "GetVolumeRootPath", "CreateVolume",     "GetMountInformation", "GetCurrentDirectoryA",
        "SetCurrentDirectoryA", "CreateDirectoryA", "RemoveDirectoryA", "MountDirectory", "UnmountDirectory",
        "CreatePackFile", "MountPackFile", "UnmountPackFiles", "GetFileAttributesA", "SetFileAttributesA",
        "CreateFileA", "CopyFileA", "MoveFileA", "DeleteFileA", "FindFirstFileA", "FindNextFileA", "FindClose"};
    const uint32_t vt_module = vtable(
        "IFFModuleFileSystem",
        {{m_volume_count, 0},     {m_volume_root, 2},       {m_create_volume, 1},    {m_mount_info, 2},
         {m_current_dir, 1},      {m_current_dir, 1},       {m_create_directory, 1}, {m_remove_directory, 1},
         {m_mount_directory, 2},  {m_unmount_directory, 1}, {m_create_pack, 0},      {m_mount_pack, 2},
         {m_unmount_packs, 1},    {m_attributes, 1},        {m_set_attributes, 2},   {m_create_file, 4},
         {m_copy, 3},             {m_move, 3},              {m_delete_file, 1},      {m_find_first, 2},
         {m_find_next, 2},        {m_find_close, 1}},
        module_names);
    static const char *const primary_names[] = {"Destroy", "Slot1", "OnCreate", "OnDestroy", "GetLastError",
                                                "ReleasePackFile"};
    const uint32_t vt_primary = vtable("CFFFileSystemModule",
                                       {{m_on_create, 0},
                                        {m_on_create, 1},
                                        {m_on_create, 0},
                                        {m_on_create, 0},
                                        {m_last_error, 0},
                                        {m_release_pack, 1}},
                                       primary_names);
    g_module = heap_alloc_kit(0x30, true);
    wr32(g_module, vt_primary);
    wr32(g_module + 4u, table3(0, 0x1c, 0x20));
    wr32(g_module + 0x24u, vt_module);

    static const char *const file_names[] = {"GetAttributes",  "GetFileName",      "GetPhysicalPath", "GetTimeCreated",
                                             "GetTimeLastModified", "GetTimeLastAccessed", "SetAttributes"};
    g_vt_file = vtable("IFFVirtualFile",
                       {{p_attributes, 0},
                        {p_file_name, 1},
                        {p_physical_path, 1},
                        {p_created, 1},
                        {p_written, 1},
                        {p_accessed, 1},
                        {p_set_attributes, 1}},
                       file_names);
    static const char *const base_names[] = {"AddRef", "Release", "QueryInterface"};
    g_vt_base = vtable("IFFUnknown", {{f_addref, 0}, {f_release, 0}, {f_query, 2}}, base_names);
    static const char *const stream_names[] = {"GetLength", "GetPosition", "Seek", "Read", "Write"};
    g_vt_stream = vtable("IFFVirtualStream",
                         {{s_length, 0}, {s_position, 1}, {s_seek, 3}, {s_read, 2}, {s_write, 2}}, stream_names);
    g_vb_file = table3(0, 4, 8);
}

// ?GetInstance@CFFFileSystemModule@@SGAAV1@XZ (stdcall, no arguments)
void get_instance(X86 *c) {
    static std::once_flag once;
    std::call_once(once, build);
    ret(c, g_module);
}

const ImportShim g_shims[] = {{FS_DLL, "?GetInstance@CFFFileSystemModule@@SGAAV1@XZ", 0, get_instance}};

__attribute__((constructor)) void filesystem_register() {
    imports_register(g_shims, sizeof g_shims / sizeof g_shims[0]);
}

} // namespace
