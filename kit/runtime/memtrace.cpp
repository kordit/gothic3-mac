// memtrace.cpp - see memtrace.h.
//
// SIGNAL SAFETY: memtrace_fault runs inside the SIGSEGV/SIGBUS handler. It takes a spin lock no other code holds
// across a fault, calls mprotect, copies a page that it has just made readable, and writes preallocated tables; no
// allocation, no stdio, and it never reads a guest page that may still be protected.
#include "memtrace.h"
#include "memory.h"
#include "win32.h"
#include "../platform/os.h"

#include <atomic>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#endif

namespace {
#if defined(__APPLE__) && defined(__aarch64__)
constexpr int kFrames = 10;          // host return addresses kept per touch, after pc and lr
constexpr uint32_t kShadowPages = 32768; // pages a trace can write (512 MB of shadow at 16 KB, reserved, not committed)

enum : uint8_t { kOff = 0, kNone = 1, kRead = 2, kWritten = 3 };

struct Touch {
    uint64_t pc, lr, frames[kFrames];
    uint32_t thread, eip, esp;
    uint8_t guest;
};
struct Rec {
    Touch first, write;
    uint32_t shadow;
};

std::atomic<bool> g_on{false};
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
uint32_t g_page = 0, g_pages = 0;
uint8_t *g_state = nullptr;   // [g_pages]
Rec *g_rec = nullptr;         // [g_pages], committed only where touched
uint32_t *g_touched = nullptr; // [g_pages]: pages in the order they were first touched
std::atomic<uint32_t> g_ntouched{0}, g_nshadow{0}, g_shadow_full{0};
uint8_t *g_shadow = nullptr;  // [kShadowPages * g_page]
uint32_t g_self = 0;

uint32_t thread_now() {
    return (uint32_t)pthread_mach_thread_np(pthread_self());
}

// Host return addresses from a frame pointer, within the thread's own stack.
void walk(Touch &t, uint64_t fp) {
    pthread_t self = pthread_self();
    const uint64_t top = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(self);
    const uint64_t bottom = top - pthread_get_stacksize_np(self);
    for (int k = 0; k < kFrames && fp >= bottom && fp + 16 <= top && !(fp & 7); ++k) {
        const uint64_t *f = (const uint64_t *)(uintptr_t)fp;
        t.frames[k] = f[1];
        if (f[0] <= fp)
            break;
        fp = f[0];
    }
}

void note(Touch &t, uint64_t pc, uint64_t lr, uint64_t fp) {
    memset(&t, 0, sizeof t);
    t.pc = pc;
    t.lr = lr;
    t.thread = thread_now();
    if (const X86 *c = guest_current_context()) {
        t.guest = 1;
        t.eip = c->eip;
        t.esp = c->r[R_ESP];
    }
    walk(t, fp);
}

void lock() {
    while (g_lock.test_and_set(std::memory_order_acquire)) {
    }
}
void unlock() {
    g_lock.clear(std::memory_order_release);
}

uint8_t *page_at(uint32_t page) {
    return g_mem + (size_t)page * g_page;
}

// Moves a page on for a read or a write; the caller holds the lock. False when the access is not one a traced page
// would refuse (the fault is someone else's).
bool open_page(uint32_t page, bool write, uint64_t pc, uint64_t lr, uint64_t fp) {
    const uint8_t st = g_state[page];
    if (st == kOff)
        return false;
    if (st == kWritten)
        return true; // another thread opened it for writing since this one faulted
    Rec &r = g_rec[page];
    if (st == kNone) {
        note(r.first, pc, lr, fp);
        r.shadow = 0;
        g_touched[g_ntouched.fetch_add(1, std::memory_order_relaxed)] = page;
        mprotect(page_at(page), g_page, PROT_READ);
        g_state[page] = kRead;
        if (!write)
            return true;
    } else if (!write) {
        return true; // another thread opened it for reading since this one faulted
    }
    note(r.write, pc, lr, fp);
    const uint32_t s = g_nshadow.fetch_add(1, std::memory_order_relaxed);
    if (s < kShadowPages) {
        memcpy(g_shadow + (size_t)s * g_page, page_at(page), g_page);
        r.shadow = s + 1;
    } else {
        g_shadow_full.fetch_add(1, std::memory_order_relaxed);
    }
    mprotect(page_at(page), g_page, PROT_READ | PROT_WRITE);
    g_state[page] = kWritten;
    return true;
}

bool tables() {
    if (g_state)
        return true;
    g_page = (uint32_t)os_vm_page_size();
    g_pages = (uint32_t)(((uint64_t)GUEST_SIZE + g_page - 1) / g_page);
    g_state = (uint8_t *)os_vm_reserve(g_pages);
    g_rec = (Rec *)os_vm_reserve((size_t)g_pages * sizeof(Rec));
    g_touched = (uint32_t *)os_vm_reserve((size_t)g_pages * sizeof(uint32_t));
    g_shadow = (uint8_t *)os_vm_reserve((size_t)kShadowPages * g_page);
    return g_state && g_rec && g_touched && g_shadow;
}

// Calls fn(first, last) for each run of pages whose state is not kOff.
template <class Fn> void each_run(Fn fn) {
    uint32_t p = 0;
    while (p < g_pages) {
        while (p < g_pages && g_state[p] == kOff)
            ++p;
        const uint32_t first = p;
        while (p < g_pages && g_state[p] != kOff)
            ++p;
        if (p > first)
            fn(first, p);
    }
}

uint64_t image_rel(uint64_t pc) {
    const uint64_t image = os_image_load();
    return image && pc >= image ? pc - image : pc;
}

void print_touch(FILE *f, const char *tag, const Touch &t) {
    fprintf(f, " %s t=%x%s", tag, t.thread, t.thread == g_self ? "*" : "");
    if (t.guest)
        fprintf(f, " eip=%x esp=%x", t.eip, t.esp);
    fprintf(f, " pc=%llx lr=%llx f=", (unsigned long long)image_rel(t.pc), (unsigned long long)image_rel(t.lr));
    for (int k = 0; k < kFrames && t.frames[k]; ++k)
        fprintf(f, "%s%llx", k ? "," : "", (unsigned long long)image_rel(t.frames[k]));
}

std::vector<uint32_t> g_blocks_seen; // written blocks of the report being made, for their first dwords

void print_block(FILE *f, uint32_t a) {
    uint32_t base, size;
    bool used;
    if (heap_block_at(a, &base, &size, &used)) {
        fprintf(f, " blk=%x+%x%s", base, size, used ? "" : "(free)");
        if (used)
            g_blocks_seen.push_back(base);
    }
}
#endif
} // namespace

int memtrace_fault(uint64_t addr, void *uc_) {
#if defined(__APPLE__) && defined(__aarch64__)
    if (!g_state || !g_mem)
        return 0;
    const uint64_t base = (uint64_t)(uintptr_t)g_mem;
    if (addr < base || addr >= base + (uint64_t)g_pages * g_page)
        return 0;
    // A fault taken just before recomp_memtrace_end opened the arena again: the page is open now, so the access is
    // simply made again. Off, a page traced last time is open (only sealed pages, kOff, can still refuse).
    if (!g_on.load(std::memory_order_acquire))
        return g_state[(addr - base) / g_page] != kOff ? 1 : 0;
    ucontext_t *uc = (ucontext_t *)uc_;
    if (!uc || !uc->uc_mcontext)
        return 0;
    // A data abort (EC 0x24/0x25); ISS bit 6 (WnR) is set for a write. An exclusive or atomic read-modify-write that
    // faults as a read first comes back as a write once the page is readable.
    const uint64_t esr = uc->uc_mcontext->__es.__esr;
    const uint64_t ec = (esr >> 26) & 0x3f;
    if (ec != 0x24 && ec != 0x25)
        return 0;
    const bool write = (esr >> 6) & 1;
    const uint32_t page = (uint32_t)((addr - base) / g_page);
    lock();
    const bool ours = open_page(page, write, (uint64_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss),
                                (uint64_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss),
                                (uint64_t)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss));
    unlock();
    return ours ? 1 : 0;
#else
    (void)addr;
    (void)uc_;
    return 0;
#endif
}

extern "C" int recomp_memtrace_active(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    return g_on.load(std::memory_order_acquire) ? 1 : 0;
#else
    return 0;
#endif
}

extern "C" int recomp_memtrace_begin(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    if (g_on.load() || !g_mem || !tables())
        return 0;
    memset(g_state, kNone, g_pages);
    // Sealed read-only pages keep their protection and their own fault handling.
    uint32_t sealed_addr[64], sealed_bytes[64];
    const uint32_t n = heap_readonly_sealed(sealed_addr, sealed_bytes, 64);
    for (uint32_t i = 0; i < n; ++i)
        memset(g_state + sealed_addr[i] / g_page, kOff, (sealed_bytes[i] + g_page - 1) / g_page);
    g_ntouched.store(0);
    g_nshadow.store(0);
    g_shadow_full.store(0);
    g_self = thread_now();
    g_on.store(true, std::memory_order_release);
    each_run([](uint32_t first, uint32_t last) { mprotect(page_at(first), (size_t)(last - first) * g_page, PROT_NONE); });
    return 1;
#else
    return 0;
#endif
}

extern "C" void recomp_memtrace_touch(uint32_t addr, uint32_t len, int write) {
#if defined(__APPLE__) && defined(__aarch64__)
    if (!g_on.load(std::memory_order_acquire) || !len)
        return;
    const uint64_t fp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    const uint64_t pc = (uint64_t)(uintptr_t)__builtin_return_address(0);
    const uint32_t last = (uint32_t)(((uint64_t)addr + len - 1) / g_page);
    for (uint32_t page = addr / g_page; page <= last && page < g_pages; ++page) {
        lock();
        open_page(page, write != 0, pc, pc, fp);
        unlock();
    }
#else
    (void)addr;
    (void)len;
    (void)write;
#endif
}

extern "C" void recomp_memtrace_end(const char *label, const char *path) {
#if defined(__APPLE__) && defined(__aarch64__)
    if (!g_on.load())
        return;
    // Written pages are compared while the rest stay protected, so what is written meanwhile is still caught.
    const uint32_t touched = g_ntouched.load();
    struct Run {
        uint32_t addr, len;
    };
    static Run runs[1 << 16];
    FILE *f = path ? fopen(path, "a") : stderr;
    if (!f)
        f = stderr;
    uint32_t nread = 0, nwritten = 0;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < touched; ++i)
        (g_state[g_touched[i]] == kWritten ? nwritten : nread) += 1;
    fprintf(f, "memtrace %s pages=%u read=%u written=%u shadow_full=%u image=%llx page=%u self=%x\n", label ? label : "",
            touched, nread, nwritten, g_shadow_full.load(), (unsigned long long)os_image_load(), g_page, g_self);
    for (uint32_t i = 0; i < touched; ++i) {
        const uint32_t page = g_touched[i];
        const Rec &r = g_rec[page];
        const bool w = g_state[page] == kWritten;
        fprintf(f, "%c %x", w ? 'W' : 'R', page * g_page);
        print_touch(f, "r", r.first);
        if (w)
            print_touch(f, "w", r.write);
        fputc('\n', f);
        if (!w || !r.shadow)
            continue;
        // Changed bytes, runs closer than 8 bytes joined.
        const uint8_t *was = g_shadow + (size_t)(r.shadow - 1) * g_page, *now = page_at(page);
        uint32_t nruns = 0;
        for (uint32_t o = 0; o < g_page;) {
            if (was[o] == now[o]) {
                ++o;
                continue;
            }
            uint32_t e = o + 1, same = 0;
            for (; e < g_page && same < 8; ++e)
                same = was[e] == now[e] ? same + 1 : 0;
            const uint32_t len = e - o - same;
            if (nruns < sizeof runs / sizeof runs[0])
                runs[nruns++] = {page * g_page + o, len};
            bytes += len;
            o = e;
        }
        for (uint32_t k = 0; k < nruns; ++k) {
            fprintf(f, "  b %x %x", runs[k].addr, runs[k].len);
            print_block(f, runs[k].addr);
            fputc('\n', f);
        }
    }
    each_run([](uint32_t first, uint32_t last) {
        mprotect(page_at(first), (size_t)(last - first) * g_page, PROT_READ | PROT_WRITE);
    });
    g_on.store(false, std::memory_order_release);
    // Each written block's first dword, read with the arena open again: for an object, its vtable.
    std::sort(g_blocks_seen.begin(), g_blocks_seen.end());
    g_blocks_seen.erase(std::unique(g_blocks_seen.begin(), g_blocks_seen.end()), g_blocks_seen.end());
    for (uint32_t blk : g_blocks_seen)
        fprintf(f, "V %x %x\n", blk, rd32(blk));
    g_blocks_seen.clear();
    fprintf(f, "end %s bytes_written=%llu\n", label ? label : "", (unsigned long long)bytes);
    if (f != stderr)
        fclose(f);
    for (uint32_t i = 0; i < touched; ++i)
        memset(&g_rec[g_touched[i]], 0, sizeof(Rec));
#else
    (void)label;
    (void)path;
#endif
}
