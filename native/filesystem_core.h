// native/filesystem_core.h - FileSystem.dll's virtual file system on the host (D-043, docs/perf/ladowanie-natywne.md
// part 1, "Rozpoznanie przed kodem").
//
// The host half of the native FileSystem.dll: volumes, virtual folders, mounted directories and archives, and every
// answer the module gives (attributes, finds, which entry an open gets), computed the way the original computes them,
// odd cases included. It owns no guest memory and makes no guest calls: the physical side goes through Disk, which
// the game's adapter (native/filesystem.cpp) implements with the kit's Win32 calls and a test with POSIX ones.
//
// Paths are the original's: '/' separators after the first step, a virtual folder's path ends in '/', comparisons
// fold ASCII only (the module's CRT runs in the C locale). Not thread-safe: the adapter serialises calls.
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace g3fs {

// SFFFileFind as the module fills it: w[0..5] the three FILETIMEs (created, accessed, written), w[6]/w[7] the size's
// high and low dword, w[8]/w[9] zero, w[10] the attributes; then the name (+0x2c) and the physical path (+0x30).
// `filled` false: the original wrote only the name (an entry without a path), the rest keeps the previous result.
struct FindData {
    uint32_t w[11] = {};
    std::string name, physical;
    bool filled = true;
};

// The physical side. Paths are what the original hands Win32: the mounted directory's path (with '/') plus the rest.
class Disk {
  public:
    virtual ~Disk() = default;
    virtual uint32_t attributes(const std::string &path) = 0; // GetFileAttributesA, 0xffffffff for none
    virtual bool set_attributes(const std::string &path, uint32_t attributes) = 0;
    // FindFirstFileA and FindNextFileA to the end, every result in order; false when FindFirstFileA fails.
    virtual bool find(const std::string &pattern, std::vector<FindData> *out) = 0;
    virtual bool create_directory(const std::string &path) = 0;
    virtual bool remove_directory(const std::string &path) = 0;
    virtual bool delete_file(const std::string &path) = 0;
    virtual bool copy_file(const std::string &from, const std::string &to, bool fail_if_exists) = 0;
    virtual bool move_file(const std::string &from, const std::string &to) = 0; // MoveFileExA(..., COPY_ALLOWED)
    virtual std::string full_path(const std::string &path) = 0;                // the CRT's _fullpath
    virtual std::string host_path(const std::string &path) = 0;                // where an archive is read from, "" none
    virtual uint64_t now() = 0;                                                // FILETIME of GetSystemTime
};

// One node of an archive's tree: a folder (dirs and files in the archive's order) or a file.
struct Node {
    bool folder = false;
    std::string path;     // as stored: full, a folder's with '/' at the end, the root's empty
    std::string name;     // what GetName gives (after the last '/', a folder's last component)
    uint32_t data[8] = {}; // the node's first 0x20 bytes: three FILETIMEs and a u64 (entry +8..+0x24)
    uint32_t attributes = 0;
    uint64_t offset = 0, stored = 0, size = 0;
    uint32_t encryption = 0, compression = 0;
    std::string from; // the path it was packed from (entry +0x60)
    Node *parent = nullptr;
    std::vector<Node *> dirs, files;
    std::unordered_map<std::string, size_t> dir_at, file_at; // lower-case path -> first position
    // Every child's path is this path plus its name (a folder's with '/'), and no two children share a key: then a
    // find without wildcards gets the same results from the maps as from the walk over every name.
    bool exact_names = true;
};

struct Pack {
    std::string path, host_path; // the guest path MountPackFile got, the host file
    int fd = -1;
    uint64_t file_size = 0; // of the archive file, the bound every offset and length is checked against
    std::deque<Node> nodes; // nodes[0] is the root
    Node *root() { return &nodes.front(); }
    ~Pack();
};

struct Folder {
    std::string path; // "#G3:/Data/library/"
    uint32_t data[8] = {};
    uint32_t attributes = 0x10010;
    Folder *parent = nullptr;
    std::vector<std::unique_ptr<Folder>> kids;
    bool mounted = false; // a physical directory: `physical`, with '/' at the end
    std::string physical;
    std::vector<std::shared_ptr<Pack>> packs; // shared with the file objects reading from them
    void touch(uint64_t now); // the written and accessed times = now
};

struct Volume {
    std::string name; // "G3"
    std::unique_ptr<Folder> root;
};

// Where an open of `rest` in an archive lands: the entry and its archive, or none.
struct PackHit {
    std::shared_ptr<Pack> pack;
    Node *node = nullptr;
};

class FileSystem {
  public:
    explicit FileSystem(Disk &disk) : disk_(disk) {}

    // The module's methods by the original's names; paths as the caller gave them.
    bool create_volume(const std::string &name);
    bool create_directory(const std::string &path);
    bool remove_directory(const std::string &path);
    bool mount_directory(const std::string &path, const std::string &physical);
    bool unmount_directory(const std::string &path);
    bool mount_pack(const std::string &path, const std::string &pack);
    bool unmount_packs(const std::string &path);
    uint32_t attributes(const std::string &path);
    bool set_attributes(const std::string &path, uint32_t attributes);
    bool copy_file(const std::string &from, const std::string &to, bool fail_if_exists);
    bool move_file(const std::string &from, const std::string &to);
    bool delete_file(const std::string &path);
    // FindFirstFileA's results, all of them, in the original's order; false when it returns no handle.
    bool find(const std::string &pattern, std::vector<FindData> *out);

    // CreateFileA's steps: the folder that holds `path` and the rest after it (10da9880), and the archive entry an open
    // of the rest gets when the physical directory gives none (10da66c0).
    Folder *resolve(const std::string &path, std::string *rest);
    PackHit pack_open(Folder *folder, const std::string &rest);

    size_t volume_count() const { return volumes_.size(); }
    Disk &disk() { return disk_; }

  private:
    Volume *volume_of(const std::string &path);
    Disk &disk_;
    std::vector<std::unique_ptr<Volume>> volumes_;
};

// Bytes whose resize leaves the new ones as they are, not zeroed: for buffers that are written whole before they are
// read (an entry read from its archive, 108 of DoLoad's samples zeroed such a buffer in the player's game 29.10).
template <class T> struct NoInit : std::allocator<T> {
    template <class U> struct rebind {
        using other = NoInit<U>;
    };
    NoInit() = default;
    template <class U> NoInit(const NoInit<U> &) {}
    template <class U> void construct(U *p) { ::new ((void *)p) U; }
    template <class U, class... A> void construct(U *p, A &&...a) { ::new ((void *)p) U(std::forward<A>(a)...); }
};
using Bytes = std::vector<uint8_t, NoInit<uint8_t>>;

// The archive reader and the entry's bytes, for the adapter's file objects and the test.
bool pack_load(Pack *pack, std::string *error);
// The whole entry as the original's buffered stream holds it: stored bytes copied, zlib inflated; false on an
// encrypted entry, a damaged stream or a short read (the original's open fails then).
bool pack_read_entry(const Pack &pack, const Node &node, Bytes *out);
// A stored, unencrypted, non-empty entry whose bytes lie inside the file: one a read can take from the archive as is.
// False for any other (pack_read_entry then decides).
bool pack_entry_stored(const Pack &pack, const Node &node);
// Bytes of the archive file itself from `at` (the direct stream's read); the count read, -1 on an error.
int64_t pack_pread(const Pack &pack, uint64_t at, void *buf, size_t n);

// A buffered entry's bytes as its open streams share them (the original's memory stream, 10da3a50). The open of a
// stored entry reads nothing (player's game 29.10: 770 of DoLoad's 1388 samples read up to 21 MB a texture there and
// copied it again into the reader's buffer). A big read of it goes from the archive file straight into the reader's
// buffer (direct); a small one has the adapter read the whole entry in once (adopt), as the open did, since the world's
// objects are read a few bytes a call. A first write reads it in too. A zlib entry has its own bytes from the open.
// A memory map of the archive in place of the reads was tried and was slower (game 14:53: page faults of 16 KB each,
// 5119 samples in s_read against 541). Not thread-safe: the adapter holds the entry's lock around every call.
class EntryBytes {
  public:
    static constexpr size_t DIRECT = 64u << 10; // a read this big, or of the whole entry, goes to the archive
    // A stored entry (pack_entry_stored): true, nothing read; the archive is kept while the entry refers to it.
    bool refer(const std::shared_ptr<Pack> &pack, const Node &node);
    // The entry read into its own bytes (pack_read_entry), false where that fails.
    bool load(const Pack &pack, const Node &node);
    uint64_t size() const { return pack_ ? size_ : own_.size(); }
    // Whether the entry's bytes are still only in the archive.
    bool in_archive() const { return pack_ != nullptr; }
    // In the archive, and a read of n at `at` takes them from there rather than from bytes read in first.
    bool direct(uint64_t at, size_t n) const;
    // The archive's bytes from `at`, up to the entry's end: the count read, 0 on an error. Needs no lock of the
    // entry's: the archive's bytes never change and the file object holds the archive.
    static size_t read_archive(const Pack &pack, const Node &node, uint64_t at, void *dst, size_t n);
    // The whole entry, read by the caller outside the entry's lock, becomes its bytes if it is still in the archive.
    void adopt(Bytes &&whole);
    // Up to the end, the count read: from the archive while the entry is there (direct or not), else its own bytes.
    size_t read(uint64_t at, void *dst, size_t n) const;
    // Grows the entry as far as it reaches, a gap zeroed; false when an entry still in the archive could not be read in.
    bool write(uint64_t at, const void *src, size_t n);

  private:
    std::shared_ptr<Pack> pack_;
    const Node *node_ = nullptr;
    uint64_t size_ = 0;
    Bytes own_;
};

// The name match FindFirstFileA uses (10da9e10 with 10da9d30): pattern and name split at the first '.'.
bool name_fits(const std::string &pattern, const std::string &name);
std::string lower(const std::string &s);
// Tests only: every find walks the archive folder's names, as the original does, instead of using the maps.
extern bool linear_names;

} // namespace g3fs
