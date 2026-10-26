// d3d9_host.cpp - the Direct3D 9 device's host entry points (dx/host_d9.h),
// over whichever GPU backend the host's device belongs to (d3d9_backend.h).
//
// Draws, clears, blits and buffer writes are encoded on a thread of their own
// while the game carries on; everything that answers the game (a read, a query
// result) or hands a frame on (present) first lets that thread finish, then
// runs here. The draw's variable data is copied into the batch, because the
// device state it points at changes as soon as the call returns.
// RECOMP_D3D9_THREAD=0 encodes on the game thread instead.
// Two switches take the game thread off that wait (both off by default; see
// present_ahead and host_d9_query_peek): RECOMP_D3D9_PRESENT_ASYNC=<n> runs the
// present itself on the render thread, up to n frames behind the game, and the
// render thread publishes finished occlusion counts for dx/d3d9.cpp's
// RECOMP_D3D9_QUERY_NOSYNC to read without a sync.
//
// On the web the GPU belongs to the browser's main thread. Each hand-over asks
// that thread to drain the queue as soon as it is free (and its animation-frame
// loop drains it too, host_d9_pump); the game runs on a worker and waits there
// for anything it needs answered.
#include "d3d9_backend.h"

#include "../../dx/d3d9_shader.h"
#include "../../platform/os.h"
#include "../../platform/ab_phase.h"
#include "../../platform/rprof.h"
#include "../d3d_render.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

// The presenter's deferred staging (host/present.h): RECOMP_D3D9_PRESENT_ASYNC.
int host_present_stage_defer(int guest_w, int guest_h, void **out);
void host_present_stage_bind(void *deferred);
void host_present_stage_unbind(void *deferred);

namespace {

D9Backend *g_test_backend = nullptr;

// The backend for the host's device: the first factory that takes it.
D9Backend *make_backend(gpu::Device *device) {
    if (!device)
        return nullptr;
#ifdef __APPLE__
    if (D9Backend *b = d9_metal_create(device))
        return b;
#endif
#ifdef __EMSCRIPTEN__
    if (D9Backend *b = d9_webgpu_create(device))
        return b;
#endif
#ifdef RECOMP_D9_VULKAN
    if (D9Backend *b = d9_vulkan_create(device))
        return b;
#endif
    return nullptr;
}

#ifdef __EMSCRIPTEN__
std::atomic<D9Backend *> g_web_backend{nullptr};
#endif

D9Backend *backend() {
    if (g_test_backend)
        return g_test_backend;
#ifdef __EMSCRIPTEN__
    return g_web_backend.load(); // made on the main thread by host_d9_web_start
#endif
    static D9Backend *b = nullptr;
    static bool tried = false;
    if (tried)
        return b;
    D3DRenderer *shared = D3DRenderer::shared();
    if (!shared)
        return nullptr; // the host has not made its device yet; ask again later
    tried = true;
    if (recomp_env("D3D9_CPU"))
        return nullptr;
    b = make_backend(shared->device());
    if (b)
        fprintf(stderr, "d3d9: rendering on %s\n", b->name());
    else
        fprintf(stderr, "d3d9: no GPU renderer for this device; drawing on the CPU\n");
    return b;
}

// Bounded reads (host_d9_texture_read_async): one mailbox slot per ticket, filled by the backend
// when the GPU has run the copy (a Metal completion thread), read by the game without waiting.
// Only this small table is locked - never the render thread or the GPU.
struct Readback {
    uint64_t request = 0; // the newest request for the ticket
    bool done = false, failed = false;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> bytes;
};
std::mutex &readback_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<uint32_t, Readback> &readbacks() {
    static std::unordered_map<uint32_t, Readback> r;
    return r;
}
std::atomic<uint64_t> g_readback_requests{0};
void deliver_readback(uint32_t ticket, uint64_t request, const uint8_t *bytes, uint32_t w,
                      uint32_t h) {
    std::lock_guard<std::mutex> lock(readback_mutex());
    auto it = readbacks().find(ticket);
    if (it == readbacks().end() || it->second.request != request)
        return; // superseded or forgotten
    Readback &r = it->second;
    r.done = true;
    r.failed = !bytes;
    r.w = w;
    r.h = h;
    if (bytes)
        r.bytes.assign(bytes, bytes + (size_t)w * h * 4u);
}

// Occlusion counts without a round trip (host_d9_query_peek, RECOMP_D3D9_QUERY_NOSYNC in dx/d3d9.cpp).
// The game thread numbers each QueryBegin; a QueryEnd queued while peeking is on is tracked:
// the render thread keeps it in its list and, whenever the backend's GPU progress moves (after each
// batch, and every millisecond while idle with a list), asks the backend for the count without a
// flush and publishes what it has here, under that number. A peek answers only the issue the game
// is asking about, so a count of an earlier issue of the same handle is never returned.
struct QueryPublished {
    uint32_t gen = 0;
    int result = 0; // the backend's query_result: 1 counted, -1 no result
    uint32_t count = 0;
};
std::mutex &query_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<uint32_t, QueryPublished> &queries_published() {
    static auto *m = new std::unordered_map<uint32_t, QueryPublished>();
    return *m;
}
// Game thread only: the id's current issue and whether its end is tracked.
struct QueryIssue {
    uint32_t gen = 0;
    bool tracked = false;
};
std::unordered_map<uint32_t, QueryIssue> &query_issues() {
    static auto *m = new std::unordered_map<uint32_t, QueryIssue>();
    return *m;
}
std::atomic<bool> g_query_peeking{false}; // set by the first host_d9_query_peek
static struct {
    uint64_t hits, pending, unknown;
} g_peek_stats;

// RECOMP_D3D9_PRESENT_ASYNC=<n> (0 or absent: off; at most 3), or PRESENT_ASYNC=<n> in the trace
// control file (host_d9_present_async): host_d9_present queues the present for the render thread
// and returns, waiting only while more than n presents are queued and not yet run. Everything that
// reads back (host_d9_texture_read, read_presented, query results without a published count,
// texture uploads) still syncs, which runs every queued present first. Admission accounting
// (RECOMP_D3D9_ADMISSION) and the single-threaded and pumped (web) queues present in step.
std::atomic<int> g_present_ahead_live{-1}; // -1: the environment's
int present_ahead() {
    static const int env = [] {
        const char *e = recomp_env("D3D9_PRESENT_ASYNC");
        return e ? atoi(e) : 0;
    }();
    const int live = g_present_ahead_live.load(std::memory_order_relaxed);
    const int n = live >= 0 ? live : env;
    return n < 0 ? 0 : n > 3 ? 3 : n;
}

// Producer writes entries/forwarding; consumer writes receipt. Present's existing
// sync barrier drains all batches before taking a cross-thread snapshot.
static HostD9Accounting admission_totals{};

enum class Op : uint8_t {
    Draw,
    RejectedDraw,
    Clear,
    Stretch,
    Define,
    Drop,
    BufferUpload,
    BufferDrop,
    QueryBegin,
    QueryEnd,
    QueryDrop,
    Probe,
    ReadAsync,
    Call,
    Tag,
    Scene, // Remaster run 13 MetalFX: host_d9_scene_split
    Camera, // Remaster run 14 foundation F1: host_d9_scene_camera (32 words at `at`)
    Atmos,  // Remaster run 14 B1 height fog: host_d9_atmosphere (32 floats at `at`)
    Present // RECOMP_D3D9_PRESENT_ASYNC: a, b, c as host_d9_present; `stage` the presenter's deferred frame
};

struct Command {
    Op op = Op::Draw;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    size_t at = 0;       // payload offset in the batch's bytes
    size_t state_at = 0; // a draw's sampler and render states, in the bytes
    HostD9Draw draw{};
    HostD9TextureDesc desc{};
    HostD9Target target{};
    HostD9Surface s0{}, s1{};
    int32_t r0[4] = {}, r1[4] = {};
    float z = 0;
    std::function<void()> *call = nullptr; // Op::Call: run on the render thread
    void *stage = nullptr;                 // Op::Present: host_present_stage_defer's handle, or null

    Command() = default;
    Command(Op o) : op(o) {}
    // A draw: the record copied in as it is made. emplace_back() value-initialised the whole command
    // (zeroing every byte of it) before the draw was copied over it, on the game thread, every draw.
    Command(Op o, const HostD9Draw &d) : op(o), draw(d) {}
};

// Test71 (RECOMP_D3D9_STATS only): what the game thread copies into the render batch per frame,
// how many batches it hands over, how many of those had to be made new and how often a batch's
// byte buffer had to grow (a reallocation and copy on the game thread).
static struct {
    uint64_t draws, bytes, const_bytes, state_puts, state_bytes, batches, fresh, grows;
    uint64_t present_wait_ns, async_presents; // the game thread's wait in host_d9_present, either way
} g_queue_stats;
// R3 run 31 O5 (review/run29/opt-design.md; A/B switch drawring, kill RECOMP_OPT_DRAWRING=0): a recycled batch is
// given the largest size a handed-over batch has had, on the render thread as it is cleared, so the game thread's
// puts stop growing it (a reallocation and copy of everything already put) after the first frames.
static bool drawring_on() {
    static const bool on = [] {
        const char *e = getenv("RECOMP_OPT_DRAWRING");
        return !(e && e[0] == '0');
    }();
    return on && !recomp_ab::old(recomp_ab::DRAWRING);
}
static std::atomic<size_t> g_batch_peak_bytes{0}, g_batch_peak_commands{0};

// A batch's payload bytes. A std::vector<uint8_t> did, but its insert at the end copies in a loop
// of its own (bytes one at a time below 64, and at -O1 always), and a draw puts seven pieces of 16
// bytes to 2 KB: the copy into the batch was 0.7% of a Gothic 3 town's game thread. Here each piece
// is one memcpy. It grows as the vector did (to twice its capacity, or what the put needs).
class BatchBytes {
  public:
    BatchBytes() = default;
    BatchBytes(const BatchBytes &) = delete;
    BatchBytes &operator=(const BatchBytes &) = delete;
    ~BatchBytes() {
        free(p_);
    }
    const uint8_t *data() const {
        return p_;
    }
    size_t size() const {
        return n_;
    }
    size_t capacity() const {
        return cap_;
    }
    void clear() {
        n_ = 0;
    }
    void reserve(size_t cap) {
        if (cap > cap_)
            grow_to(cap);
    }
    void append(const void *src, size_t n) {
        if (cap_ - n_ < n)
            grow_to(std::max(cap_ * 2, n_ + n));
        memcpy(p_ + n_, src, n);
        n_ += n;
    }

  private:
    void grow_to(size_t cap) {
        uint8_t *q = static_cast<uint8_t *>(realloc(p_, cap));
        if (!q)
            throw std::bad_alloc(); // what the vector's growth threw
        p_ = q;
        cap_ = cap;
    }
    uint8_t *p_ = nullptr;
    size_t n_ = 0, cap_ = 0;
};

struct Batch {
    std::vector<Command> commands;
    BatchBytes bytes;
    // The last draw's state tables in `bytes`, reused while the version holds.
    uint64_t state_version = 0;
    size_t state_at = 0;
    void clear() {
        commands.clear();
        bytes.clear();
        state_version = 0;
        if (drawring_on()) {
            const size_t pb = g_batch_peak_bytes.load(std::memory_order_relaxed);
            const size_t pc = g_batch_peak_commands.load(std::memory_order_relaxed);
            if (bytes.capacity() < pb)
                bytes.reserve(pb);
            if (commands.capacity() < pc)
                commands.reserve(pc);
        }
    }
    size_t put(const void *p, size_t n) {
        size_t at = bytes.size();
        if (n) {
            const uint8_t *s = (const uint8_t *)p;
            if (bytes.capacity() - at < n)
                ++g_queue_stats.grows;
            bytes.append(s, n);
        }
        return at;
    }
};

class RenderThread {
  public:
    explicit RenderThread(D9Backend *r) : r_(r) {
        const char *env = recomp_env("D3D9_THREAD");
        threaded_ = !(env && !strcmp(env, "0"));
#ifdef __EMSCRIPTEN__
        threaded_ = true;
        pumped_ = true; // the browser's main loop drains the queue
#endif
        if (threaded_ && !pumped_)
            worker_ = std::thread([this] { run(); });
    }

    // ---- the game's side ----
    Batch &batch() {
        return *filling_;
    }
    void submitted() {
        if (!threaded_) {
            execute(*filling_);
            filling_->clear();
            return;
        }
        // Hand work over often enough that the worker keeps pace with the game.
        // 48 commands or 1 MB per hand-over (run 31, D-176: the A4-lite size experiment is gone).
        if (filling_->commands.size() >= 48 || filling_->bytes.size() >= ((size_t)1 << 20))
            hand_over();
    }
    // A render thread of our own (not RECOMP_D3D9_THREAD=0, not the web's pumped queue): only it can present
    // later (RECOMP_D3D9_PRESENT_ASYNC) and poll query counts while the game waits (host_d9_query_peek).
    bool own_thread() const {
        return threaded_ && !pumped_;
    }
    // Queues the present (already in `c`) and returns once at most `ahead` presents wait to be run.
    void present_later(const Command &c, int ahead) {
        filling_->commands.push_back(c);
        {
            std::lock_guard<std::mutex> lock(m_);
            ++presents_queued_;
        }
        hand_over();
        std::unique_lock<std::mutex> lock(m_);
        present_cv_.wait(lock, [&] { return presents_queued_ - presents_done_ <= (uint64_t)ahead; });
    }
    // Everything queued has been encoded; the caller may use the backend.
    template <class F> auto sync(F &&f) -> decltype(f()) {
        if (!threaded_) {
            execute(*filling_);
            filling_->clear();
            return f();
        }
        if (pumped_)
            return sync_on_pump(std::forward<F>(f));
        hand_over();
        std::unique_lock<std::mutex> lock(m_);
        idle_cv_.wait(lock, [this] { return queue_.empty() && !busy_; });
        return f(); // the worker waits on m_ for new work, so it cannot run now
    }

    // The pumped queue: `f` runs on the pumping thread, after everything
    // queued before it, while the caller waits.
    template <class F> auto sync_on_pump(F &&f) -> decltype(f()) {
        using R = decltype(f());
        if constexpr (std::is_void<R>::value) {
            std::function<void()> job = [&] { f(); };
            run_on_pump(&job);
        } else {
            R result{};
            std::function<void()> job = [&] { result = f(); };
            run_on_pump(&job);
            return result;
        }
    }
    void run_on_pump(std::function<void()> *job) {
        Command c{Op::Call};
        c.call = job;
        filling_->commands.push_back(c);
        hand_over();
        std::unique_lock<std::mutex> lock(m_);
        idle_cv_.wait(lock, [this] { return queue_.empty() && !busy_; });
    }

#ifdef __EMSCRIPTEN__
    // The main thread: inside its frame or a drain, where locks may be held.
    void frame(bool inside) {
        main_depth_ += inside ? 1 : -1;
        if (!main_depth_ && pump_missed_) {
            pump_missed_ = false;
            pump();
        }
    }
    void asked_pump() {
        pump_asked_.store(false);
        if (main_depth_) {
            pump_missed_ = true; // drained when the frame ends
            return;
        }
        pump();
    }
#endif

    // Runs what is queued on the calling thread (pumped mode).
    void pump() {
#ifdef __EMSCRIPTEN__
        ++main_depth_;
        struct Leave {
            int &depth;
            ~Leave() {
                --depth;
            }
        } leave{main_depth_};
#endif
        std::unique_lock<std::mutex> lock(m_);
        while (!queue_.empty()) {
            std::unique_ptr<Batch> b = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            execute(*b);
            b->clear();
            lock.lock();
            busy_ = false;
            if (spare_.size() < 8)
                spare_.push_back(std::move(b));
        }
        idle_cv_.notify_all();
    }

  private:
    void hand_over() {
        if (filling_->commands.empty())
            return;
        if (filling_->bytes.size() > g_batch_peak_bytes.load(std::memory_order_relaxed))
            g_batch_peak_bytes.store(filling_->bytes.size(), std::memory_order_relaxed);
        if (filling_->commands.size() > g_batch_peak_commands.load(std::memory_order_relaxed))
            g_batch_peak_commands.store(filling_->commands.size(), std::memory_order_relaxed);
        std::unique_ptr<Batch> next;
        {
            std::lock_guard<std::mutex> lock(m_);
            queue_.push_back(std::move(filling_));
            if (!spare_.empty()) {
                next = std::move(spare_.back());
                spare_.pop_back();
            }
        }
        work_cv_.notify_one();
        ++g_queue_stats.batches;
        if (!next)
            ++g_queue_stats.fresh;
        filling_ = next ? std::move(next) : std::make_unique<Batch>();
#ifdef __EMSCRIPTEN__
        if (pumped_ && !pump_asked_.exchange(true))
            // The proxied call only asks for a timeout. The main thread runs
            // proxied calls wherever it is blocked, including inside a browser
            // lock that drawing would take again (a queue's completion callback
            // waiting on the allocator, say); a timeout runs from the event loop
            // instead, where nothing of ours is part-way through.
            emscripten_proxy_async(
                emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
                [](void *self) {
                    emscripten_async_call([](void *s) { ((RenderThread *)s)->asked_pump(); }, self,
                                          0);
                },
                this);
#endif
    }

    void run() {
        os_thread_prefer_performance();
#if defined(__APPLE__)
        pthread_setname_np("d3d9 render");
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), "d3d9 render");
#endif
        std::unique_lock<std::mutex> lock(m_);
        for (;;) {
            if (q_ended_.empty())
                work_cv_.wait(lock, [this] { return !queue_.empty(); });
            else if (!work_cv_.wait_for(lock, std::chrono::milliseconds(q_idle_ms_), [this] { return !queue_.empty(); })) {
                // Idle with ended queries not yet published: the GPU may have run them since. Every
                // millisecond while that brings counts, backing off to 4 ms while it does not (a
                // query whose command buffer nothing commits, the game paused).
                busy_ = true;
                lock.unlock();
                const size_t waiting = q_ended_.size();
                publish_pooled();
                q_idle_ms_ = q_ended_.size() < waiting ? 1 : std::min(q_idle_ms_ * 2, 4);
                lock.lock();
                busy_ = false;
                idle_cv_.notify_all();
                continue;
            }
            q_idle_ms_ = 1;
            std::unique_ptr<Batch> b = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            lock.unlock();
            execute_pooled(*b);
            b->clear();
            lock.lock();
            busy_ = false;
            if (spare_.size() < 8)
                spare_.push_back(std::move(b));
            if (queue_.empty())
                idle_cv_.notify_all();
        }
    }
    void execute_pooled(Batch &b);
    void publish_pooled();

    // ---- occlusion counts published without a sync (host_d9_query_peek) ----
    void query_forget(uint32_t id) {
        for (size_t i = 0; i < q_ended_.size(); ++i)
            if (q_ended_[i].first == id) {
                q_ended_.erase(q_ended_.begin() + (ptrdiff_t)i);
                if (i < q_checked_)
                    --q_checked_;
                return;
            }
    }

  public:
    // Asks the backend, without a flush, for the ended queries' counts and publishes what is
    // there: all of them when its GPU has moved on (or it cannot say), else only those ended
    // since the last poll (a query with no draws, or one that overflowed, has its answer at once).
    // Render thread, or whoever runs execute().
    void publish_queries() {
        const uint64_t progress = r_->query_progress();
        const bool moved = progress == D9Backend::kProgressUnknown || progress != q_progress_;
        const size_t start = moved ? 0 : q_checked_;
        if (start >= q_ended_.size())
            return;
        q_progress_ = progress;
        size_t keep = start;
        for (size_t i = start; i < q_ended_.size(); ++i) {
            const auto e = q_ended_[i];
            uint32_t count = 0;
            const int r = r_->query_result(e.first, &count, false);
            if (r == 0) {
                q_ended_[keep++] = e;
                continue;
            }
            std::lock_guard<std::mutex> lock(query_mutex());
            QueryPublished &p = queries_published()[e.first];
            p.gen = e.second;
            p.result = r;
            p.count = count;
        }
        q_ended_.resize(keep);
        q_checked_ = keep;
    }
    void execute(Batch &b) {
        for (Command &cmd : b.commands) {
            const uint8_t *base = b.bytes.data();
            switch (cmd.op) {
            case Op::RejectedDraw:
            case Op::Draw: {
                HostD9Draw &d = cmd.draw;
                if (d.admission.serial) {
                    ++admission_totals.received;
                    admission_totals.received_rejected += cmd.op == Op::RejectedDraw;
                    admission_totals.received_serial_sum += d.admission.serial;
                    admission_totals.last_received = d.admission.serial;
                }
                size_t at = cmd.at;
                auto take = [&](size_t n) -> const uint8_t * {
                    const uint8_t *p = n ? base + at : nullptr;
                    at += n;
                    return p;
                };
                d.decl = take(d.decl_size);
                d.vconst = (const float *)take(d.vconst_count * 16u);
                d.pconst = (const float *)take(d.pconst_count * 16u);
                d.inline_vertices = take(d.inline_bytes);
                d.inline_indices = take(d.inline_index_bytes);
                const uint8_t *tables = base + cmd.state_at;   // the draw's full tables (ss | rs | set)
                d.sampler_state = (const uint32_t *)tables;
                d.render_state = (const uint32_t *)(tables + 16 * 14 * 4);
                d.render_state_set = tables + 16 * 14 * 4 + 256 * 4;
                d.label = cmd.a ? (const char *)take(cmd.a) : nullptr;
                d.pool_block = d.pool_fuse ? (const float *)take(20 * 16) : nullptr;   // Remaster run 10
                if (cmd.op == Op::RejectedDraw)
                    r_->rejected_draw(d);
                else
                    r_->draw(d);
                break;
            }
            case Op::Clear:
                r_->clear(cmd.target, cmd.r0, cmd.a,
                          cmd.a ? (const int32_t *)(base + cmd.at) : nullptr, cmd.b, cmd.c, cmd.z,
                          cmd.d);
                break;
            case Op::Stretch:
                r_->stretch(cmd.s0, cmd.r0, cmd.s1, cmd.r1, cmd.a);
                break;
            case Op::Define:
                r_->define(cmd.desc);
                break;
            case Op::Drop:
                r_->drop(cmd.a);
                break;
            case Op::Tag:
                r_->tag(cmd.a, (uint64_t)cmd.c << 32 | cmd.b);
                break;
            case Op::BufferUpload:
                r_->buffer_upload(cmd.a, cmd.b, cmd.c, base + cmd.at, cmd.d);
                break;
            case Op::BufferDrop:
                r_->buffer_drop(cmd.a);
                break;
            case Op::QueryBegin:
                r_->query_begin(cmd.a);
                q_gen_[cmd.a] = cmd.b;
                query_forget(cmd.a);
                break;
            case Op::QueryEnd:
                r_->query_end(cmd.a);
                query_forget(cmd.a);
                if (cmd.c) // tracked: its count is published when the GPU has it
                    q_ended_.emplace_back(cmd.a, q_gen_[cmd.a]);
                break;
            case Op::QueryDrop:
                r_->query_drop(cmd.a);
                query_forget(cmd.a);
                q_gen_.erase(cmd.a);
                {
                    std::lock_guard<std::mutex> lock(query_mutex());
                    queries_published().erase(cmd.a);
                }
                break;
            case Op::Probe:
                r_->probe_next(cmd.a ? (const char *)(base + cmd.at) : nullptr);
                break;
            case Op::ReadAsync:
                r_->read_async(cmd.a, cmd.b, cmd.c, cmd.d, (uint64_t)cmd.at, deliver_readback);
                break;
            case Op::Call:
                (*cmd.call)();
                break;
            case Op::Scene:
                r_->scene_split(cmd.a, cmd.b, cmd.c);
                break;
            case Op::Camera: {
                float m[32];
                memcpy(m, base + cmd.at, sizeof m);
                r_->scene_camera(m, m + 16);
                break;
            }
            case Op::Atmos: {
                float m[32];
                memcpy(m, base + cmd.at, sizeof m);
                r_->atmosphere(m, m + 16);
                break;
            }
            case Op::Present:
                // The presenter's frame was pinned when the game presented; the copy goes there,
                // whatever frame the game is writing now.
                host_present_stage_bind(cmd.stage);
                r_->present(cmd.a, cmd.b, cmd.c);
                host_present_stage_unbind(cmd.stage);
                {
                    std::lock_guard<std::mutex> lock(m_);
                    ++presents_done_;
                }
                present_cv_.notify_all();
                break;
            }
        }
        publish_queries();
    }

  private:
    D9Backend *r_;
    bool threaded_ = false;
    bool pumped_ = false;
    std::atomic<bool> pump_asked_{false}; // a main-thread pump is on its way
    int main_depth_ = 0;                  // main thread only
    bool pump_missed_ = false;            // main thread only
    std::thread worker_;
    std::mutex m_;
    std::condition_variable work_cv_, idle_cv_;
    std::deque<std::unique_ptr<Batch>> queue_;
    std::vector<std::unique_ptr<Batch>> spare_;
    std::unique_ptr<Batch> filling_ = std::make_unique<Batch>();
    bool busy_ = false;
    // RECOMP_D3D9_PRESENT_ASYNC (under m_): presents handed over and presents run.
    std::condition_variable present_cv_;
    uint64_t presents_queued_ = 0, presents_done_ = 0;
    // host_d9_query_peek, the executing thread's: each id's issue number at its last QueryBegin,
    // the tracked ended queries not yet published (id, issue), oldest first, of which the first
    // q_checked_ were asked at the backend's progress q_progress_.
    std::unordered_map<uint32_t, uint32_t> q_gen_;
    std::vector<std::pair<uint32_t, uint32_t>> q_ended_;
    uint64_t q_progress_ = 0;
    size_t q_checked_ = 0;
    int q_idle_ms_ = 1; // the idle poll's period (render thread)
};

RenderThread *render_thread() {
    static RenderThread *t = nullptr;
    static D9Backend *for_backend = nullptr;
    D9Backend *r = backend();
    if (!r)
        return nullptr;
    if (r != for_backend) {
        // A test swapped the backend: the old thread (if any) is idle; leave it.
        for_backend = r;
        t = new RenderThread(r);
    }
    return t;
}

// The d9 caches decode a program once for everyone; decoding it here means the
// render thread only ever looks it up, so the bytes need not travel.
// Fills in any key the caller left out.
void predecode(HostD9Draw &d) {
    if (d.programs_ready && d.vs_key && d.ps_key)
        return; // Remaster C2: gpu_draw resolved both programs just before (same keys, same thread)
    if (!d.vs_key)
        d.vs_key = d9sh::code_key(d.vs, d.vs ? d.vs_size : 0);
    if (!d.ps_key)
        d.ps_key = d9sh::code_key(d.ps, d.ps ? d.ps_size : 0);
    d9sh::program_for_key(d.vs_key, d.vs, d.vs ? d.vs_size : 0);
    d9sh::program_for_key(d.ps_key, d.ps, d.ps ? d.ps_size : 0);
}

void push(Op op, uint32_t a) {
    if (RenderThread *t = render_thread()) {
        Command c{op};
        c.a = a;
        t->batch().commands.push_back(c);
        t->submitted();
    }
}

} // namespace

#ifdef __APPLE__
// Objective-C objects a batch creates are released as it finishes.
void d9_autorelease_run(void (*fn)(void *), void *arg);
void RenderThread::execute_pooled(Batch &b) {
    std::pair<RenderThread *, Batch *> job(this, &b);
    d9_autorelease_run(
        [](void *p) {
            auto *j = static_cast<std::pair<RenderThread *, Batch *> *>(p);
            j->first->execute(*j->second);
        },
        &job);
}
void RenderThread::publish_pooled() {
    d9_autorelease_run([](void *p) { static_cast<RenderThread *>(p)->publish_queries(); }, this);
}
#else
void RenderThread::execute_pooled(Batch &b) {
    execute(b);
}
void RenderThread::publish_pooled() {
    publish_queries();
}
#endif

int host_d9_active(void) {
    return backend() != nullptr;
}
void host_d9_texture_define(const HostD9TextureDesc *desc) {
    RenderThread *t = render_thread();
    if (!t || !desc)
        return;
    Command c{Op::Define};
    c.desc = *desc;
    t->batch().commands.push_back(c);
    t->submitted();
}
void host_d9_scene_split(uint32_t phase, uint32_t bb, uint32_t ds) {
    if (RenderThread *t = render_thread()) {
        Command c{Op::Scene};
        c.a = phase;
        c.b = bb;
        c.c = ds;
        t->batch().commands.push_back(c);
        t->submitted();
    }
}
void host_d9_scene_camera(const float *view, const float *info) {
    if (RenderThread *t = render_thread()) {
        Batch &b = t->batch();
        Command c{Op::Camera};
        c.at = b.put(view, 16 * sizeof(float));
        b.put(info, 16 * sizeof(float));
        b.commands.push_back(c);
        t->submitted();
    }
}
void host_d9_atmosphere(const float *a, const float *b) {
    if (RenderThread *t = render_thread()) {
        Batch &bt = t->batch();
        Command c{Op::Atmos};
        c.at = bt.put(a, 16 * sizeof(float));
        bt.put(b, 16 * sizeof(float));
        bt.commands.push_back(c);
        t->submitted();
    }
}
void host_d9_texture_drop(uint32_t id) {
    push(Op::Drop, id);
}
void host_d9_texture_tag(uint32_t id, uint64_t key) {
    if (RenderThread *t = render_thread()) {
        Command c{Op::Tag};
        c.a = id;
        c.b = (uint32_t)key;
        c.c = (uint32_t)(key >> 32);
        t->batch().commands.push_back(c);
        t->submitted();
    }
}
void host_d9_texture_upload(uint32_t id, uint32_t face, uint32_t level, const uint8_t *bytes,
                            uint32_t pitch) {
    // The size is the backend's to know, so this one runs in step.
    if (RenderThread *t = render_thread())
        t->sync([&] { backend()->upload(id, face, level, bytes, pitch); });
}
int host_d9_texture_read(uint32_t id, uint32_t face, uint32_t level, uint8_t *bytes,
                         uint32_t pitch) {
    RenderThread *t = render_thread();
    return t && t->sync([&] { return backend()->read(id, face, level, bytes, pitch); }) ? 1 : 0;
}
void host_d9_texture_read_async(uint32_t id, uint32_t face, uint32_t level, uint32_t ticket) {
    RenderThread *t = render_thread();
    if (!t || !ticket)
        return;
    const uint64_t request = ++g_readback_requests;
    {
        std::lock_guard<std::mutex> lock(readback_mutex());
        Readback &r = readbacks()[ticket];
        r = Readback{};
        r.request = request;
    }
    Command c{Op::ReadAsync};
    c.a = id;
    c.b = face;
    c.c = level;
    c.d = ticket;
    c.at = (size_t)request;
    t->batch().commands.push_back(c);
    t->submitted();
}
int host_d9_texture_read_poll(uint32_t ticket, uint8_t *bytes, uint32_t pitch, uint32_t w,
                              uint32_t h) {
    std::lock_guard<std::mutex> lock(readback_mutex());
    auto it = readbacks().find(ticket);
    if (it == readbacks().end())
        return -1;
    const Readback &r = it->second;
    if (!r.done)
        return 0;
    if (r.failed || r.w != w || r.h != h || !bytes || pitch < w * 4u)
        return -1;
    for (uint32_t y = 0; y < h; ++y)
        memcpy(bytes + (size_t)y * pitch, r.bytes.data() + (size_t)y * w * 4u, (size_t)w * 4u);
    return 1;
}
void host_d9_texture_read_forget(uint32_t ticket) {
    std::lock_guard<std::mutex> lock(readback_mutex());
    readbacks().erase(ticket);
}
void host_d9_buffer_upload(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes,
                           uint32_t size) {
    RenderThread *t = render_thread();
    if (!t || !bytes)
        return;
    Batch &b = t->batch();
    Command c{Op::BufferUpload};
    c.a = id;
    c.b = total;
    c.c = offset;
    c.d = size;
    c.at = b.put(bytes, size);
    b.commands.push_back(c);
    t->submitted();
}
void host_d9_buffer_drop(uint32_t id) {
    push(Op::BufferDrop, id);
}
void host_d9_admission_entry(uint64_t serial) {
    ++admission_totals.entries;
    admission_totals.entry_serial_sum += serial;
    admission_totals.last_entry = serial;
}
int host_d9_admission_enabled(void) {
    static const bool enabled = [] {
        const char *a = recomp_env("D3D9_ADMISSION");
        const char *t = recomp_env("D3D9_TRACE");
        return a && !strcmp(a, "1") && t && !strcmp(t, "1");
    }();
    return enabled;
}
static void queue_draw(const HostD9Draw *draw, bool rejected);
void host_d9_draw(const HostD9Draw *draw) {
    queue_draw(draw, false);
}
void host_d9_rejected_draw(const HostD9Draw *draw) {
    queue_draw(draw, true);
}
static void queue_draw(const HostD9Draw *draw, bool rejected) {
    RenderThread *t = render_thread();
    if (!t || !draw) {
        if (draw && draw->admission.serial)
            ++admission_totals.unforwarded;
        return;
    }
    Batch &b = t->batch();
    const size_t bytes_before = b.bytes.size();
    Command &c = b.commands.emplace_back(rejected ? Op::RejectedDraw : Op::Draw, *draw);
    if (!rejected)
        predecode(c.draw);
    c.at = b.put(draw->decl, draw->decl ? draw->decl_size : 0);
    c.draw.decl_size = draw->decl ? draw->decl_size : 0;
    b.put(draw->vconst, draw->vconst ? draw->vconst_count * 16u : 0);
    c.draw.vconst_count = draw->vconst ? draw->vconst_count : 0;
    b.put(draw->pconst, draw->pconst ? draw->pconst_count * 16u : 0);
    c.draw.pconst_count = draw->pconst ? draw->pconst_count : 0;
    b.put(draw->inline_vertices, draw->inline_vertices ? draw->inline_bytes : 0);
    c.draw.inline_bytes = draw->inline_vertices ? draw->inline_bytes : 0;
    b.put(draw->inline_indices, draw->inline_indices ? draw->inline_index_bytes : 0);
    c.draw.inline_index_bytes = draw->inline_indices ? draw->inline_index_bytes : 0;
    // The consumer walks the per-draw payload through the label. Keep it
    // contiguous: state tables below may be inserted or reused by version.
    if (draw->label) {
        size_t n = strlen(draw->label) + 1;
        b.put(draw->label, n);
        c.a = (uint32_t)n;
    }
    // Remaster run 10: the fused pool block follows the label (20 float4).
    if (draw->pool_fuse && draw->pool_block && !rejected)
        b.put(draw->pool_block, 20 * 16);
    else
        c.draw.pool_fuse = 0;
    c.draw.pool_block = nullptr;
    if (rejected)
        c.draw.pool_probe = 0;   // Remaster run 11
    {
        // The three state tables in full, or the batch's last ones again while the pipeline's state version holds
        // (run 31, D-176: the A4-lite delta encoder is gone; this is the path every build has used).
        static const uint32_t zs[16 * 14] = {}, zr[256] = {};
        static const uint8_t zf[256] = {};
        const size_t before = b.bytes.size();
        if (!draw->state_version || draw->state_version != b.state_version) {
            b.state_at = b.put(draw->sampler_state ? draw->sampler_state : zs, 16 * 14 * 4);
            b.put(draw->render_state ? draw->render_state : zr, 256 * 4);
            b.put(draw->render_state_set ? draw->render_state_set : zf, 256);
            b.state_version = draw->state_version;
        }
        c.state_at = b.state_at;
        if (b.bytes.size() != before) {
            ++g_queue_stats.state_puts;
            g_queue_stats.state_bytes += b.bytes.size() - before;
        }
    }
    ++g_queue_stats.draws;
    g_queue_stats.bytes += b.bytes.size() - bytes_before;
    g_queue_stats.const_bytes += (draw->vconst ? draw->vconst_count * 16u : 0) + (draw->pconst ? draw->pconst_count * 16u : 0);
    // Programs are found by key on the render thread.
    c.draw.vs = nullptr;
    c.draw.ps = nullptr;
    if (draw->admission.serial) {
        ++admission_totals.forwarded;
        admission_totals.rejected += rejected;
        admission_totals.forwarded_serial_sum += draw->admission.serial;
    }
    t->submitted();
}
void host_d9_clear(const HostD9Target *target, const int32_t viewport[4], uint32_t count,
                   const int32_t *rects, uint32_t flags, uint32_t color, float z,
                   uint32_t stencil) {
    RenderThread *t = render_thread();
    if (!t || !target)
        return;
    Batch &b = t->batch();
    Command c{Op::Clear};
    c.target = *target;
    if (viewport)
        memcpy(c.r0, viewport, sizeof c.r0);
    c.a = rects ? count : 0;
    c.at = b.put(rects, rects ? count * 16u : 0);
    c.b = flags;
    c.c = color;
    c.z = z;
    c.d = stencil;
    b.commands.push_back(c);
    t->submitted();
}
void host_d9_stretch(HostD9Surface src, const int32_t src_rect[4], HostD9Surface dst,
                     const int32_t dst_rect[4], uint32_t filter) {
    RenderThread *t = render_thread();
    if (!t)
        return;
    Command c{Op::Stretch};
    c.s0 = src;
    c.s1 = dst;
    if (src_rect)
        memcpy(c.r0, src_rect, sizeof c.r0);
    if (dst_rect)
        memcpy(c.r1, dst_rect, sizeof c.r1);
    c.a = filter;
    t->batch().commands.push_back(c);
    t->submitted();
}
void host_d9_present(uint32_t backbuffer, uint32_t width, uint32_t height) {
    recomp_ab::tick(); // TEST ONLY phase of an in-run A/B; nothing unless RECOMP_AB_PERIOD is set
    rprof::frame();    // Remaster A4 design input; nothing unless RECOMP_REMASTER_PROF=1
    {
        static const bool stats = recomp_env("D3D9_STATS") != nullptr;
        static uint64_t presents = 0;
        if (stats && ++presents % 600 == 0) {
            const double f = 600.0;
            fprintf(stderr,
                    "d3d9 host: per frame %.0f draws, %.0f KB copied into the batch (constants %.0f KB, state "
                    "tables %.0f KB in %.0f puts); %.1f batches, %.2f new, %.2f buffer growths; present wait "
                    "%.2f ms (%.0f%% queued); query peeks %.1f answered, %.1f not yet, %.1f untracked\n",
                    g_queue_stats.draws / f, g_queue_stats.bytes / f / 1024.0, g_queue_stats.const_bytes / f / 1024.0,
                    g_queue_stats.state_bytes / f / 1024.0, g_queue_stats.state_puts / f,
                    g_queue_stats.batches / f,
                    g_queue_stats.fresh / f, g_queue_stats.grows / f, g_queue_stats.present_wait_ns / f / 1e6,
                    100.0 * g_queue_stats.async_presents / f, g_peek_stats.hits / f, g_peek_stats.pending / f,
                    g_peek_stats.unknown / f);
            g_queue_stats = {};
            g_peek_stats = {};
        }
    }
    RenderThread *t = render_thread();
    if (!t)
        return;
    const uint64_t t0 = os_monotonic_ns();
    const int ahead = present_ahead();
    void *stage = nullptr;
    if (ahead > 0 && t->own_thread() && !host_d9_admission_enabled() &&
        host_present_stage_defer((int)width, (int)height, &stage) >= 0) {
        Command c{Op::Present};
        c.a = backbuffer;
        c.b = width;
        c.c = height;
        c.stage = stage;
        t->present_later(c, ahead);
        ++g_queue_stats.async_presents;
    } else
        t->sync([&] {
            if (host_d9_admission_enabled()) {
                HostD9Accounting snapshot = admission_totals;
                snapshot.enabled = snapshot.drained = 1;
                backend()->admission_accounting(snapshot);
            }
            backend()->present(backbuffer, width, height);
        });
    g_queue_stats.present_wait_ns += os_monotonic_ns() - t0;
}
void host_d9_present_async(int frames) {
    g_present_ahead_live.store(frames < 0 ? -1 : frames, std::memory_order_relaxed);
}
int host_d9_read_presented(uint8_t *rgb, uint32_t cap, uint32_t *w, uint32_t *h) {
    RenderThread *t = render_thread();
    return t && t->sync([&] { return backend()->read_presented(rgb, cap, w, h); }) ? 1 : 0;
}

void host_d9_query_begin(uint32_t id) {
    RenderThread *t = render_thread();
    if (!t)
        return;
    // Numbered across all ids, so a handle made again under a dropped one's id never matches a count
    // published for the old one before its drop has run.
    static uint32_t issues = 0;
    QueryIssue &q = query_issues()[id];
    q.gen = ++issues;
    q.tracked = false;
    Command c{Op::QueryBegin};
    c.a = id;
    c.b = q.gen;
    t->batch().commands.push_back(c);
    t->submitted();
}
void host_d9_query_end(uint32_t id) {
    RenderThread *t = render_thread();
    if (!t)
        return;
    Command c{Op::QueryEnd};
    c.a = id;
    if (g_query_peeking.load(std::memory_order_relaxed)) {
        query_issues()[id].tracked = true;
        c.c = 1;
    }
    t->batch().commands.push_back(c);
    t->submitted();
}
int host_d9_query_result(uint32_t id, uint32_t *count, int flush) {
    RenderThread *t = render_thread();
    return t && count ? t->sync([&] { return backend()->query_result(id, count, flush != 0); }) : -1;
}
int host_d9_query_peek(uint32_t id, uint32_t *count) {
    // Only a render thread of our own polls while the game waits; encoding on the game thread
    // (RECOMP_D3D9_THREAD=0), a result costs no wait anyway, and the web's pumped queue keeps its sync.
    RenderThread *t = render_thread();
    if (!t || !t->own_thread())
        return -2;
    if (!g_query_peeking.load(std::memory_order_relaxed))
        g_query_peeking.store(true, std::memory_order_relaxed); // from now on query ends are tracked
    auto it = query_issues().find(id);
    if (!count || it == query_issues().end() || !it->second.tracked) {
        ++g_peek_stats.unknown;
        return -2;
    }
    std::lock_guard<std::mutex> lock(query_mutex());
    auto p = queries_published().find(id);
    if (p == queries_published().end() || p->second.gen != it->second.gen) {
        ++g_peek_stats.pending;
        return 0;
    }
    ++g_peek_stats.hits;
    *count = p->second.count;
    return p->second.result;
}
void host_d9_query_drop(uint32_t id) {
    query_issues().erase(id);
    push(Op::QueryDrop, id);
}

bool host_d9_use_device_for_test(gpu::Device *device) {
    delete g_test_backend;
    g_test_backend = make_backend(device);
    return g_test_backend != nullptr;
}
void host_d9_probe_next_frame(const char *tag) {
    RenderThread *t = render_thread();
    if (!t)
        return;
    Batch &b = t->batch();
    Command c{Op::Probe};
    if (tag) {
        size_t n = strlen(tag) + 1;
        c.at = b.put(tag, n);
        c.a = (uint32_t)n;
    }
    b.commands.push_back(c);
    t->submitted();
}

// Remaster run 11 fused pools (host_d9.h): the readiness table. One writer (the render thread), any reader: a key is
// published after its state (release), so a reader that sees the key sees a state written for it. Open addressing,
// never erased; a full table leaves new keys unknown (two-pass), which is always safe.
namespace {
constexpr size_t kPoolSlots = 4096;
std::atomic<uint64_t> g_pool_keys[kPoolSlots];
std::atomic<int> g_pool_states[kPoolSlots];
} // namespace
int host_d9_pool_state(uint64_t key) {
    for (size_t i = 0, h = (size_t)(key * 0x9e3779b97f4a7c15ull >> 52); i < kPoolSlots; ++i, h = (h + 1) % kPoolSlots) {
        const uint64_t k = g_pool_keys[h].load(std::memory_order_acquire);
        if (k == key)
            return g_pool_states[h].load(std::memory_order_acquire);
        if (!k)
            return HOST_D9_POOL_UNKNOWN;
    }
    return HOST_D9_POOL_UNKNOWN;
}
void host_d9_pool_mark(uint64_t key, int state) {
    for (size_t i = 0, h = (size_t)(key * 0x9e3779b97f4a7c15ull >> 52); i < kPoolSlots; ++i, h = (h + 1) % kPoolSlots) {
        const uint64_t k = g_pool_keys[h].load(std::memory_order_relaxed);
        if (k == key) {
            g_pool_states[h].store(state, std::memory_order_release);
            return;
        }
        if (!k) {
            g_pool_states[h].store(state, std::memory_order_relaxed);
            g_pool_keys[h].store(key, std::memory_order_release);
            return;
        }
    }
}

#ifdef __EMSCRIPTEN__
bool host_d9_web_start(gpu::Device *device) {
    D9Backend *b = make_backend(device);
    g_web_backend.store(b);
    if (b)
        fprintf(stderr, "d3d9: rendering on %s\n", b->name());
    return b != nullptr;
}
void host_d9_pump(void) {
    if (!g_web_backend.load())
        return;
    if (RenderThread *t = render_thread())
        t->pump();
}
void host_d9_frame(bool inside) {
    if (!g_web_backend.load())
        return;
    if (RenderThread *t = render_thread())
        t->frame(inside);
}
#endif
