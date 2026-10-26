// remaster_cache.h - NFSMW Remaster A1 step 3: the per-key replacement cache (plan Rev 2.5, challenge C-03).
//
// One entry per key, shared by every texture object with that key. A request (a keyed texture's first top-level
// upload, or a bind after a cooldown) queues a job; two worker threads read the pack's DDS and create and fill the
// MTLTexture, which no encoder can see yet. Completions are installed only at present, so a frame never mixes the
// original and the replacement of a key that just became resident. A budget (RECOMP_REMASTER_BUDGET_MB, default
// 1536) evicts least-recently-bound entries unused for 3 presents; what cannot fit is refused. Evicted, refused
// and failed keys retry at a later bind, after 120 presents. Evicted bytes are tracked for 3 presents (until the GPU
// is done with them) but do not count against the budget (run 15: counting them made one install evict every
// evictable entry, 906 at once in the owner's 8 Oct log, then reload them: the pop-in).
//
// Run 15 (pop-in): the default budget follows physical memory (>= 16 GB: 3072 MB, or the pack plus headroom up to a
// quarter of RAM; below: 1536 MB; RECOMP_REMASTER_BUDGET_MB overrides). When the whole pack fits the budget and the
// replacements are shown (TEXSWAP on), start() queues every pack key as a low-priority "warm" job, so HD is resident
// before the game streams the section in; a texture the game uploads or binds while its warm job still waits jumps
// to the high-priority queue. Warm loads have no game size yet: a replacement is shown only after the size check
// against the game's texture (at its first upload), exactly as before. RECOMP_REMASTER_WARM=0 turns the warm off,
// =1 forces it. All file reads and texture fills stay on the workers; install at present only swaps pointers.
//
// Threads: the encoding side (request, bind, present) runs on one thread at a time, the render thread or the
// game thread inside RenderThread::sync, which never overlap. Workers touch only their job, the device and the
// two guarded queues.
#pragma once

#import <Metal/Metal.h>

#include "remaster_tex.h"

#include <pthread.h>
#include <pthread/qos.h>

#include <sys/sysctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace remaster {

class ReplacementCache {
public:
    explicit ReplacementCache(TexKeys &keys) : keys_(keys) {
        const char *b = std::getenv("RECOMP_REMASTER_BUDGET_MB");
        if (b && *b) {
            budget_ = (size_t)std::strtoull(b, nullptr, 10) << 20;
            budget_env_ = true;
        }
        const char *w = std::getenv("RECOMP_REMASTER_WARM");
        if (w && *w)
            warm_mode_ = *w == '0' ? 0 : 1;
    }
    ~ReplacementCache() { stop(); }

    // Starts the workers when a pack is configured. No pack: nothing runs (R1-0 runs this way).
    void start(id<MTLDevice> device, bool bc) {
        if (!keys_.has_pack() || started_)
            return;
        device_ = device;
        bc_ = bc;
        started_ = true;
        const auto files = keys_.pack_files();
        std::vector<size_t> sizes(files.size());
        size_t pack_bytes = 0;
        for (size_t i = 0; i < files.size(); ++i)
            pack_bytes += sizes[i] = file_size(files[i].second);
        const uint64_t ram = physical_ram();
        if (!budget_env_)
            budget_ = default_budget(ram, pack_bytes);
        // R3 run 29 (review/run29/presets-design.md 3): below 16 GB the budget is 768 MB and a pack that does not fit
        // it (with the warm rule's 1/16) is not used at all: the 8 Oct owner log showed a 2.2 GB pack at 1536 MB is
        // constant evictions and pop-in, not HD. The game's own textures are drawn; Setup's smaller pack fits.
        if (!budget_env_ && ram < (16ull << 30) && pack_bytes + pack_bytes / 16 > budget_) {
            std::fprintf(stderr, "[remaster] cache: HD textures OFF on this %llu GB Mac (pack %.0f MB does not fit the "
                                 "%zu MB budget); the game's own textures are drawn\n",
                         (unsigned long long)(ram >> 30), pack_bytes / 1048576.0, budget_ >> 20);
            return;
        }
        const bool warm = keys_.swap() && warm_mode_ != 0 && (warm_mode_ == 1 || pack_bytes + pack_bytes / 16 <= budget_);
        if (warm) {
            std::lock_guard<std::mutex> lock(job_mutex_);
            for (size_t i = 0; i < files.size(); ++i) {
                if (!sizes[i])
                    continue;
                Entry &e = entries_[files[i].first];
                e.state = S_PENDING;
                e.warm = true;
                e.reserved = sizes[i];
                e.gen = 1;
                e.loads = 1;
                reserved_ += sizes[i];
                ++requests_;
                ++warm_left_;
                jobs_lo_.push_back(Job{files[i].first, files[i].second, 0, 0, 1});
            }
            max_loads_ = warm_left_ ? 1 : 0;
        }
        std::fprintf(stderr, "[remaster] cache: budget %zu MB (%s; RAM %llu GB), pack %zu keys %.1f MB, warm %s (%zu keys)\n",
                     budget_ >> 20, budget_env_ ? "RECOMP_REMASTER_BUDGET_MB" : "from RAM",
                     (unsigned long long)(ram >> 30), files.size(), pack_bytes / 1048576.0,
                     warm ? "on" : !keys_.swap() ? "off (Original)" : warm_mode_ == 0 ? "off (RECOMP_REMASTER_WARM=0)"
                                                                      : "off (pack exceeds budget)",
                     warm_left_);
        warm_start_ = std::chrono::steady_clock::now();
        const int n = warm ? 4 : 2;
        for (int i = 0; i < n; ++i)
            workers_.emplace_back([this] { work(); });
    }
    bool active() const { return started_; }

    // Encoding side.
    void request(uint64_t key, uint32_t game_w, uint32_t game_h) {
        if (!started_ || !key)
            return;
        Entry &e = entries_[key];
        if (game_w && game_h) {
            e.game_w = game_w;
            e.game_h = game_h;
        }
        if (e.state == S_RESIDENT) {
            check(e); // a warm load meets its game size here
            return;
        }
        if (e.state == S_PENDING) {
            boost(key, e);
            return;
        }
        if (e.state != S_NONE && frame_ < e.retry_at)
            return;
        const std::string file = keys_.pack_file(key);
        if (file.empty()) {
            e.state = S_ABSENT; // not in the pack: never asked again
            e.retry_at = UINT64_MAX;
            return;
        }
        const size_t est = file_size(file);
        if (resident_ + reserved_ + est > budget_ && !evictable()) {
            refuse(e); // cannot fit and nothing can make room: no file read (C-03 H2, T2 thrash)
            return;
        }
        Job j{key, file, e.game_w, e.game_h, e.gen + 1};
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            if (jobs_hi_.size() >= 256) {
                ++queue_full_;
                e.retry_at = frame_ + 120;
                if (e.state == S_NONE)
                    e.state = S_REFUSED;
                return;
            }
            // (counted as a request below only once queued)
            jobs_hi_.push_back(j);
        }
        job_cv_.notify_one();
        e.gen = j.gen;
        e.warm = false;
        if (e.state != S_NONE)
            ++reloads_;
        e.state = S_PENDING;
        e.reserved = est;
        reserved_ += e.reserved;
        ++requests_;
        if (++e.loads > max_loads_)
            max_loads_ = e.loads;
        changed_ = true;
    }
    // The replacement to bind for `key`, or nil (the original binds).
    id<MTLTexture> bind(uint64_t key) {
        if (!started_ || !key)
            return nil;
        auto it = entries_.find(key);
        if (it == entries_.end())
            return nil;
        Entry &e = it->second;
        e.last_used = frame_;
        if (e.state == S_RESIDENT)
            return check(e) ? e.tex : nil;
        if (e.state == S_PENDING) {
            ++binds_pending_;
            boost(key, e);
        } else if ((e.state == S_EVICTED || e.state == S_REFUSED || e.state == S_FAILED) && frame_ >= e.retry_at)
            request(key, e.game_w, e.game_h);
        return nil;
    }
    void present() {
        if (!started_)
            return;
        ++frame_;
        while (!freeing_.empty() && freeing_.front().until <= frame_) {
            freeing_bytes_ -= freeing_.front().bytes;
            freeing_.pop_front();
        }
        if (has_done_.exchange(false))
            install_done();
        if (warm_left_ == 0 && !warm_logged_ && requests_) {
            warm_logged_ = true;
            if (warm_keys_done_)
                std::fprintf(stderr, "[remaster] cache: warm done: %llu keys, resident %.1f MB, %.1f s after start, frame %llu\n",
                             (unsigned long long)warm_keys_done_, resident_ / 1048576.0,
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - warm_start_).count(),
                             (unsigned long long)frame_);
        }
        if (frame_ % 600 == 0 && changed_) {
            changed_ = false;
            size_t pending = 0;
            for (const auto &kv : entries_)
                pending += kv.second.state == S_PENDING;
            std::fprintf(stderr,
                         "[remaster] cache: frame %llu resident %.1f MB reserved %.1f MB freeing %.1f MB budget %zu MB; "
                         "requests %llu installs %llu refused %llu failed %llu rejected %llu evictions %llu reloads %llu "
                         "max-loads/key %u queue-full %llu pending %zu binds-while-pending %llu boosted %llu warm-left %zu\n",
                         (unsigned long long)frame_, resident_ / 1048576.0, reserved_ / 1048576.0,
                         freeing_bytes_ / 1048576.0, budget_ >> 20, (unsigned long long)requests_,
                         (unsigned long long)installs_, (unsigned long long)refused_, (unsigned long long)failed_,
                         (unsigned long long)rejected_, (unsigned long long)evictions_, (unsigned long long)reloads_,
                         max_loads_, (unsigned long long)queue_full_, pending, (unsigned long long)binds_pending_,
                         (unsigned long long)boosted_, warm_left_);
        }
    }

private:
    enum State { S_NONE, S_PENDING, S_RESIDENT, S_REFUSED, S_EVICTED, S_FAILED, S_ABSENT };
    struct Entry {
        State state = S_NONE;
        id<MTLTexture> tex = nil;
        size_t bytes = 0, reserved = 0;
        uint64_t last_used = 0, retry_at = 0;
        uint32_t loads = 0, game_w = 0, game_h = 0;
        uint64_t backoff = 0; // presents before the next retry after a refusal: 120, doubling to 7200
        uint32_t img_w = 0, img_h = 0, gen = 0; // gen: the load the newest job carries (workers skip duplicates)
        bool warm = false, boosted = false, checked = false;
    };
    struct Job {
        uint64_t key;
        std::string file;
        uint32_t game_w, game_h, gen;
    };
    enum Result { LOAD_OK, LOAD_FAILED, LOAD_REJECTED };
    struct Done {
        uint64_t key;
        id<MTLTexture> tex;
        size_t bytes;
        Result result;
        uint32_t w, h;
    };
    struct Freeing {
        size_t bytes;
        uint64_t until;
    };

    static size_t file_size(const std::string &f) {
        struct stat st {};
        return ::stat(f.c_str(), &st) == 0 ? (size_t)st.st_size : 0;
    }
    static uint64_t physical_ram() {
        if (const char *o = std::getenv("RECOMP_REMASTER_RAM_GB"); o && *o)   // TEST ONLY (run 29 memory tiers)
            return (uint64_t)std::strtoull(o, nullptr, 10) << 30;
        uint64_t m = 0;
        size_t n = sizeof m;
        return ::sysctlbyname("hw.memsize", &m, &n, nullptr, 0) == 0 ? m : 0;
    }
    // >= 16 GB: 3072 MB, or the whole pack plus 1/8 and 256 MB when that is more, at most a quarter of RAM.
    // Run 29: below 16 GB, 768 MB (start() turns the pack off when it does not fit).
    static size_t default_budget(uint64_t ram, size_t pack) {
        if (ram < (16ull << 30))
            return (size_t)768 << 20;
        const size_t want = std::max<size_t>((size_t)3072 << 20, pack + pack / 8 + ((size_t)256 << 20));
        return std::min<size_t>(want, std::max<size_t>((size_t)3072 << 20, (size_t)(ram / 4)));
    }
    static bool size_ok(uint32_t iw, uint32_t ih, uint32_t gw, uint32_t gh) {
        return iw >= gw && ih >= gh && (uint64_t)iw * gh == (uint64_t)ih * gw;
    }
    // A resident replacement is shown only once it passed the size check against the game's texture (demand loads
    // are checked on the worker; warm loads here, when the game size arrives). A failed check drops it for good.
    bool check(Entry &e) {
        if (e.checked)
            return true;
        if (!e.game_w || !e.game_h)
            return false;
        if (size_ok(e.img_w, e.img_h, e.game_w, e.game_h)) {
            e.checked = true;
            return true;
        }
        e.tex = nil;
        resident_ -= std::min(resident_, e.bytes);
        freeing_.push_back({e.bytes, frame_ + 3});
        freeing_bytes_ += e.bytes;
        e.bytes = 0;
        e.state = S_ABSENT;
        e.retry_at = UINT64_MAX;
        ++rejected_;
        changed_ = true;
        return false;
    }
    // A waiting warm job the game needs now: queue it again at high priority (the worker that takes it first wins).
    void boost(uint64_t key, Entry &e) {
        if (!e.warm || e.boosted)
            return;
        e.boosted = true;
        Job j{key, keys_.pack_file(key), e.game_w, e.game_h, e.gen};
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            if (taken_.count(key) && taken_[key] == e.gen)
                return; // already loading
            jobs_hi_.push_front(j);
        }
        ++boosted_;
        job_cv_.notify_one();
    }

    void install_done() {
        std::deque<Done> done;
        {
            std::lock_guard<std::mutex> lock(done_mutex_);
            done.swap(done_);
        }
        for (Done &d : done) {
            Entry &e = entries_[d.key];
            reserved_ -= std::min(reserved_, e.reserved);
            e.reserved = 0;
            if (e.warm) {
                e.warm = false;
                warm_left_ -= std::min<size_t>(warm_left_, 1);
                ++warm_keys_done_;
            }
            if (d.result != LOAD_OK) {
                (d.result == LOAD_FAILED ? failed_ : rejected_)++;
                e.state = d.result == LOAD_FAILED ? S_FAILED : S_ABSENT; // a rejected file never fits: stop asking
                e.retry_at = d.result == LOAD_FAILED ? frame_ + 120 : UINT64_MAX;
                continue;
            }
            if (!fit(d.bytes)) {
                refuse(e);
                continue;
            }
            e.backoff = 0;
            e.tex = d.tex;
            e.bytes = d.bytes;
            e.img_w = d.w;
            e.img_h = d.h;
            e.checked = false;
            e.state = S_RESIDENT;
            resident_ += d.bytes;
            ++installs_;
            check(e);
        }
        changed_ = true;
    }
    void refuse(Entry &e) {
        ++refused_;
        e.state = S_REFUSED;
        e.backoff = e.backoff ? std::min<uint64_t>(e.backoff * 2, 7200) : 120;
        e.retry_at = frame_ + e.backoff;
        changed_ = true;
    }
    bool evictable() const {
        for (const auto &kv : entries_)
            if (kv.second.state == S_RESIDENT && kv.second.last_used + 3 <= frame_)
                return true;
        return false;
    }
    // Evicts least-recently-bound residents unused for 3 presents until `need` fits. Only resident bytes count: an
    // eviction moves bytes to freeing_, so counting freeing_ here never shrank the sum and emptied the cache.
    bool fit(size_t need) {
        while (resident_ + need > budget_) {
            Entry *lru = nullptr;
            for (auto &kv : entries_) {
                Entry &c = kv.second;
                if (c.state == S_RESIDENT && c.last_used + 3 <= frame_ && (!lru || c.last_used < lru->last_used))
                    lru = &c;
            }
            if (!lru)
                return false;
            lru->tex = nil; // in-flight command buffers retain it
            lru->state = S_EVICTED;
            lru->retry_at = frame_ + 120;
            resident_ -= lru->bytes;
            freeing_.push_back({lru->bytes, frame_ + 3});
            freeing_bytes_ += lru->bytes;
            lru->bytes = 0;
            ++evictions_;
        }
        return true;
    }

    void work() {
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lock(job_mutex_);
                job_cv_.wait(lock, [this] { return stopping_ || !jobs_hi_.empty() || !jobs_lo_.empty(); });
                if (stopping_)
                    return;
                std::deque<Job> &q = jobs_hi_.empty() ? jobs_lo_ : jobs_hi_;
                j = std::move(q.front());
                q.pop_front();
                uint32_t &g = taken_[j.key];
                if (g == j.gen)
                    continue; // the other copy of a boosted job is already loading or loaded
                g = j.gen;
            }
            Done d{j.key, nil, 0, LOAD_FAILED, 0, 0};
            @autoreleasepool {
                load(j, d);
            }
            {
                std::lock_guard<std::mutex> lock(done_mutex_);
                done_.push_back(d);
            }
            has_done_.store(true);
        }
    }
    void load(const Job &j, Done &d) {
        TexKeys::Image im = TexKeys::read_dds(j.file, keys_.is_mod(j.key));   // run 19: a player's mod file
        if (im.levels.empty())
            return; // LOAD_FAILED
        if ((TexKeys::block_fmt(im.fmt) && !bc_) || (j.game_w && j.game_h && !size_ok(im.w, im.h, j.game_w, j.game_h))) {
            d.result = LOAD_REJECTED;
            return;
        }
        const MTLPixelFormat pf = im.fmt == TexKeys::F_BC1     ? MTLPixelFormatBC1_RGBA
                                  : im.fmt == TexKeys::F_BC2   ? MTLPixelFormatBC2_RGBA
                                  : im.fmt == TexKeys::F_BC3   ? MTLPixelFormatBC3_RGBA
                                  : im.fmt == TexKeys::F_BC7   ? MTLPixelFormatBC7_RGBAUnorm
                                  : im.fmt == TexKeys::F_RGBA8 ? MTLPixelFormatRGBA8Unorm
                                                               : MTLPixelFormatBGRA8Unorm;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                                                                                     width:im.w
                                                                                    height:im.h
                                                                                 mipmapped:im.levels.size() > 1];
        td.mipmapLevelCount = im.levels.size();
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [device_ newTextureWithDescriptor:td];
        if (!t)
            return;
        for (uint32_t l = 0; l < im.levels.size(); ++l) {
            const uint32_t lw = std::max<uint32_t>(im.w >> l, 1), lh = std::max<uint32_t>(im.h >> l, 1);
            [t replaceRegion:MTLRegionMake2D(0, 0, lw, lh) mipmapLevel:l withBytes:im.levels[l].data()
                 bytesPerRow:im.row_bytes[l]];
        }
        d.tex = t;
        d.w = im.w;
        d.h = im.h;
        d.bytes = t.allocatedSize;
        d.result = LOAD_OK;
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            stopping_ = true;
            jobs_hi_.clear();
            jobs_lo_.clear();
        }
        job_cv_.notify_all();
        for (std::thread &w : workers_)
            if (w.joinable())
                w.join();
        workers_.clear();
    }

    TexKeys &keys_;
    id<MTLDevice> device_ = nil;
    bool bc_ = false, started_ = false, changed_ = false, budget_env_ = false, warm_logged_ = false;
    int warm_mode_ = -1; // RECOMP_REMASTER_WARM: -1 auto (the pack fits the budget), 0 off, 1 on
    size_t warm_left_ = 0;
    uint64_t warm_keys_done_ = 0, boosted_ = 0;
    std::chrono::steady_clock::time_point warm_start_;
    size_t budget_ = (size_t)1536 << 20, resident_ = 0, reserved_ = 0, freeing_bytes_ = 0;
    uint64_t frame_ = 0, requests_ = 0, installs_ = 0, refused_ = 0, failed_ = 0, rejected_ = 0, evictions_ = 0,
             reloads_ = 0, queue_full_ = 0, binds_pending_ = 0;
    uint32_t max_loads_ = 0;
    std::unordered_map<uint64_t, Entry> entries_;
    std::deque<Freeing> freeing_;
    // Worker side.
    std::mutex job_mutex_, done_mutex_;
    std::condition_variable job_cv_;
    std::deque<Job> jobs_hi_, jobs_lo_; // demand (and boosted) jobs first, then the warm
    std::unordered_map<uint64_t, uint32_t> taken_; // key -> the newest load a worker took (guarded by job_mutex_)
    std::deque<Done> done_;
    std::atomic<bool> has_done_{false};
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

} // namespace remaster
