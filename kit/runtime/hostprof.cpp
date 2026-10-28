// hostprof.cpp - where the host CPU goes while guest code runs.
//
// RECOMP_HOST_PROFILE=<file prefix>: a thread samples, 1000 times a second,
// the host thread that holds the guest baton: its PC, LR and frame-pointer
// chain (up to 16 return addresses), read with the thread suspended. Unlike
// the guest profiler (profile.cpp, RECOMP_PROFILE=1) it changes nothing in
// how calls run, and it sees the runtime's helpers, shims and the renderer
// apart from the translated function that called them. macOS's `sample`
// stalls on this process; this does not walk its address space.
//
// PROFILE=reset / PROFILE=report in the trace control file (cpu.cpp) clear the
// samples or write them to <prefix>-<n>.txt: one line per distinct stack,
// "<count> <pc> <lr> <return addresses...>", addresses in the main image as
// its unslid addresses (what `nm` prints), others as "name+0xoff" by dladdr.
// work/hostprof.py names and sums them.
//
// RECOMP_HOST_PROFILE_SPIKE=<ms> as well (Gothic 3 session 18: a player's stutters of 50-90 ms that the
// frame phases did not explain): it samples the thread that presents (the game thread) instead of the
// baton's holder, keeps only the current frame's stacks, and when a frame took <ms> or more writes them
// to <prefix>-spike-<n>-<ms>.txt, from the sampler thread so the game does not wait. At most SPIKE_FILES files or
// SPIKE_BYTES in all: the first cap, 60 files, was spent on the menu and the loading screen before the player had
// walked two minutes (Gothic 3 session 19). A loading screen's frame writes a few MB, a stutter in play tens of KB.
#include "hostprof.h"

#include "../platform/os.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>

namespace {
constexpr int kDepth = 16;
struct Stack {
    uint64_t pc[kDepth + 2] = {}; // pc, lr, then the frame chain
    bool operator<(const Stack &o) const {
        return memcmp(pc, o.pc, sizeof pc) < 0;
    }
};
struct State {
    std::mutex mutex;
    std::map<Stack, uint64_t> counts;
    uint64_t idle = 0, failed = 0, reports = 0;
    // spike mode: the presenting thread, the frame's stacks, frames waiting to be written
    std::atomic<uint64_t> game_thread{0};
    std::map<Stack, uint64_t> frame;
    std::vector<std::pair<double, std::map<Stack, uint64_t>>> pending;
    unsigned spikes = 0;
    std::atomic<uint64_t> spike_bytes{0};
};
const unsigned SPIKE_FILES = 2000;
const uint64_t SPIKE_BYTES = 1ull << 30;
State &state() {
    static State *s = new State;
    return *s;
}
const char *prefix() {
    static const char *p = recomp_env("HOST_PROFILE");
    return p && *p ? p : nullptr;
}
double spike_ms() {
    static const double ms = [] {
        const char *e = recomp_env("HOST_PROFILE_SPIKE");
        return e && *e ? strtod(e, nullptr) : 0.0;
    }();
    return ms;
}
void write_stacks(FILE *f, const std::map<Stack, uint64_t> &counts);

bool sample(pthread_t thread, Stack &out) {
    const mach_port_t port = pthread_mach_thread_np(thread);
    if (port == MACH_PORT_NULL || thread_suspend(port) != KERN_SUCCESS)
        return false;
    arm_thread_state64_t st;
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    const bool ok = thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS;
    if (ok) {
        out.pc[0] = (uint64_t)__darwin_arm_thread_state64_get_pc(st);
        out.pc[1] = (uint64_t)__darwin_arm_thread_state64_get_lr(st);
        // The frame records are on the thread's own stack, which cannot move
        // or be freed while the thread is suspended; only follow ones inside it.
        const uint64_t hi = (uint64_t)pthread_get_stackaddr_np(thread);
        const uint64_t lo = hi - (uint64_t)pthread_get_stacksize_np(thread);
        uint64_t fp = (uint64_t)__darwin_arm_thread_state64_get_fp(st);
        for (int i = 0; i < kDepth; ++i) {
            if (fp < lo || fp + 16 > hi || (fp & 7))
                break;
            const uint64_t *rec = (const uint64_t *)fp;
            const uint64_t next = rec[0], ret = rec[1] & 0x0000ffffffffffffull; // strip a PAC
            out.pc[2 + i] = ret;
            if (next <= fp)
                break;
            fp = next;
        }
    }
    thread_resume(port);
    return ok;
}

void write_spikes() {
    auto &s = state();
    std::vector<std::pair<double, std::map<Stack, uint64_t>>> todo;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        todo.swap(s.pending);
    }
    for (const auto &spike : todo) {
        char path[1024];
        snprintf(path, sizeof path, "%s-spike-%u-%.0f.txt", prefix(), s.spikes++, spike.first);
        if (FILE *f = fopen(path, "w")) {
            write_stacks(f, spike.second);
            s.spike_bytes += (uint64_t)ftell(f);
            fclose(f);
        }
    }
}

void run_spikes() {
    auto &s = state();
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += std::chrono::milliseconds(1);
        std::this_thread::sleep_until(next);
        const uint64_t thread = s.game_thread.load(std::memory_order_acquire);
        Stack stack;
        if (thread && sample((pthread_t)(uintptr_t)thread, stack)) {
            std::lock_guard<std::mutex> lock(s.mutex);
            ++s.frame[stack];
        }
        write_spikes();
        auto now = std::chrono::steady_clock::now();
        if (now > next + std::chrono::milliseconds(1))
            next = now;
    }
}

void run() {
    if (spike_ms() > 0) {
        run_spikes();
        return;
    }
    auto &s = state();
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += std::chrono::milliseconds(1);
        std::this_thread::sleep_until(next);
        const OsThreadId holder = sched_current_holder_host();
        Stack stack;
        const bool ok = holder && sample((pthread_t)(uintptr_t)holder, stack);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!holder)
                ++s.idle;
            else if (!ok)
                ++s.failed;
            else
                ++s.counts[stack];
        }
        auto now = std::chrono::steady_clock::now();
        if (now > next + std::chrono::milliseconds(1))
            next = now;
    }
}

[[maybe_unused]] const bool g_started = [] {
    if (prefix())
        std::thread(run).detach();
    return true;
}();

std::string name_of(uint64_t pc, intptr_t slide, uint64_t lo, uint64_t hi) {
    char buf[256];
    if (!pc)
        return "0";
    if (pc >= lo && pc < hi) {
        snprintf(buf, sizeof buf, "%llx", (unsigned long long)(pc - slide));
        return buf;
    }
    Dl_info info;
    if (dladdr((const void *)pc, &info) && info.dli_sname) {
        snprintf(buf, sizeof buf, "%s+0x%llx", info.dli_sname,
                 (unsigned long long)(pc - (uint64_t)info.dli_saddr));
        return buf;
    }
    snprintf(buf, sizeof buf, "?%llx", (unsigned long long)pc);
    return buf;
}
} // namespace

void hostprof_reset() {
    if (!prefix())
        return;
    auto &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.counts.clear();
    s.idle = s.failed = 0;
}

namespace {
// The main image's slide and __TEXT range, so that only its PCs are written unslid.
void image_range(intptr_t *slide, uint64_t *lo, uint64_t *hi) {
    *slide = _dyld_get_image_vmaddr_slide(0);
    const auto *header = (const struct mach_header_64 *)_dyld_get_image_header(0);
    *lo = *hi = (uint64_t)header;
    const auto *cmd = (const struct load_command *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (cmd->cmd == LC_SEGMENT_64) {
            const auto *seg = (const struct segment_command_64 *)cmd;
            if (!strcmp(seg->segname, "__TEXT"))
                *hi = seg->vmaddr + *slide + seg->vmsize;
        }
        cmd = (const struct load_command *)((const char *)cmd + cmd->cmdsize);
    }
}

void write_stacks(FILE *f, const std::map<Stack, uint64_t> &counts) {
    intptr_t slide;
    uint64_t lo, hi;
    image_range(&slide, &lo, &hi);
    std::map<uint64_t, std::string> names;
    for (const auto &kv : counts) {
        fprintf(f, "%llu", (unsigned long long)kv.second);
        for (uint64_t pc : kv.first.pc) {
            if (!pc)
                break;
            auto it = names.find(pc);
            if (it == names.end())
                it = names.emplace(pc, name_of(pc, slide, lo, hi)).first;
            fprintf(f, " %s", it->second.c_str());
        }
        fputc('\n', f);
    }
}
} // namespace

void hostprof_frame(double frame_ms) {
    if (!prefix() || spike_ms() <= 0)
        return;
    auto &s = state();
    s.game_thread.store((uint64_t)(uintptr_t)pthread_self(), std::memory_order_release);
    std::lock_guard<std::mutex> lock(s.mutex);
    if (frame_ms >= spike_ms() && s.spikes + s.pending.size() < SPIKE_FILES && s.spike_bytes < SPIKE_BYTES)
        s.pending.emplace_back(frame_ms, std::move(s.frame));
    s.frame.clear();
}

void hostprof_report() {
    if (!prefix())
        return;
    auto &s = state();
    std::map<Stack, uint64_t> counts;
    uint64_t idle, failed;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        counts = s.counts;
        idle = s.idle;
        failed = s.failed;
        ++s.reports;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s-%llu.txt", prefix(), (unsigned long long)s.reports);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "hostprof: cannot write %s\n", path);
        return;
    }
    uint64_t total = 0;
    for (const auto &kv : counts)
        total += kv.second;
    fprintf(f, "# hostprof samples=%llu idle=%llu failed=%llu stacks=%zu\n", (unsigned long long)total,
            (unsigned long long)idle, (unsigned long long)failed, counts.size());
    write_stacks(f, counts);
    fclose(f);
    fprintf(stderr, "hostprof: %llu samples (%llu idle, %llu failed) in %zu stacks -> %s\n",
            (unsigned long long)total, (unsigned long long)idle, (unsigned long long)failed, counts.size(),
            path);
}

#else
void hostprof_reset() {}
void hostprof_report() {}
void hostprof_frame(double) {}
#endif
