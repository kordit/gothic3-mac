// cpu.cpp - the CPU-level call-outs x86.h declares as "provided by
// runtime/": import dispatch, unknown call targets, divide errors
// and the handful of privileged or environment-sensing instructions.
#include "imports.h"
#include "profile.h"
#include "hostprof.h"
#include "mods_seam.h"
#include "intrinsics.h"
#include "game_config.h"
#include "discovery.h"
#include "interp.h"
#include "loader.h"
#include "win32.h"
#include "thunks.h"
#include "loader.h"
#include "seh.h"
#include "../platform/os.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <strings.h>

// Free threads (kernel32.cpp): the modules whose threads run without the baton, each [lo, hi), set
// when a module named in RECOMP_FREE_THREADS registers (a list: "fmodex.dll PhysXCore.dll"). g_free_hi
// is the highest end, 0 while there is none, so the guard below costs one compare.
static const int FREE_MODULES_MAX = 4;
static uint32_t g_free_lo[FREE_MODULES_MAX], g_free_end[FREE_MODULES_MAX];
static int g_free_count = 0;
static uint32_t g_free_hi = 0;
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// State for the _setjmp/_longjmp intrinsics, outside extern "C" because these
// helpers have C++ types.
// ---------------------------------------------------------------------------
namespace {
struct SetjmpRecord {
    uint32_t buf = 0;
    X86 saved{};
    jmp_buf env;
    bool armed = false;
    uint32_t profile_depth = 0;
    uint32_t callback_depth = 0;
};
// Keyed by the guest jmp_buf address. Heap allocated so a record outlives the
// frame that created it.
std::map<uint32_t, SetjmpRecord *> &setjmps() {
    static std::map<uint32_t, SetjmpRecord *> m;
    return m;
}
// Gothic 3 session 16: guest threads may run at once (free threads), so the records are taken under a lock and
// the context and value read after a longjmp belong to the thread that jumped.
std::mutex g_setjmp_mutex;
// Read after a longjmp, so it must not be a local of the returning frame.
thread_local X86 *g_setjmp_ctx = nullptr;
} // namespace

// ---------------------------------------------------------------------------
// Undeliverable call targets. A call the address table cannot deliver is
// always a translator bug - a missing entry point, or a call target inside a
// block nothing recovered - and the guest carries on with the wrong answer
// rather than stopping. Recording each distinct one lets a run report them
// instead of leaving them to be found in a log.
// ---------------------------------------------------------------------------
namespace {
std::vector<UnknownCall> &unknown_calls() {
    static std::vector<UnknownCall> v;
    return v;
}
} // namespace

const std::vector<UnknownCall> &recomp_unknown_calls() {
    return unknown_calls();
}

// ---------------------------------------------------------------------------
// Auxiliary module registry. Modules register from static constructors, so
// the vector is function-local to survive any construction order.
// ---------------------------------------------------------------------------
namespace {
std::vector<const RecompModule *> &modules() {
    static std::vector<const RecompModule *> v;
    return v;
}
// Every call into another module looks its target up: a binary search and a
// walk of the module list each time, which was two fifths of the time Gothic 3
// spent building its SpeedTree ambient occlusion with the return check below.
// The answer is cached by address, direct-mapped, one 64-bit word per entry so
// a reader never sees half an update. Registering a module empties the cache.
constexpr uint32_t kLookupCache = 1u << 14;
std::atomic<uint64_t> g_call_cache[kLookupCache]; // target << 32 | module number << 24 | index
uint32_t lookup_slot(uint32_t target) {
    return (target ^ (target >> 14)) & (kLookupCache - 1);
}
void lookup_caches_clear();

// Which module owns a 64 KB page of the guest: its number (1-based, as in
// recomp_ic_tables), 0 for none, kPageShared for a page two modules touch or
// a module numbered past the byte. Without it every lookup walked the module
// list, fifteen of them in Gothic 3, comparing two bounds each.
constexpr uint8_t kPageShared = 0xffu;
uint8_t g_module_page[1u << 16];

// A module's call continuations, one bit per address between the first and the
// last (x86.h RecompModule::call_returns, sorted). Every RET the inline hit table
// misses asks here: a binary search through some 100 000 addresses before, about
// as costly as the call it answers, and the direct-mapped cache that sat in front
// of it had the hit table's own slots, so the two always missed together.
//
// Indexed by module number below 256, like recomp_ic_tables. words null: no
// continuations, a module numbered past the table, or continuations too far
// apart; the binary search answers those. Plain arrays rather than a guarded
// function-local static, which costs a load and a branch at every use.
struct ReturnBits {
    uint32_t first, span; // bits cover [first, first + span]
    const uint64_t *words; // allocated once per module, never freed (modules are never unregistered)
};
ReturnBits g_return_bits[256];
// Spread wider than this, a module keeps the binary search (8 MB of bits at most).
constexpr uint32_t kReturnBitsSpan = 64u << 20;

void index_module(const RecompModule *m, uint32_t number) {
    if (number < 256 && m->call_return_count && m->call_returns) {
        const uint32_t first = m->call_returns[0], last = m->call_returns[m->call_return_count - 1];
        if (last >= first && last - first < kReturnBitsSpan) {
            uint64_t *words = new uint64_t[(last - first) / 64 + 1]();
            for (uint32_t i = 0; i < m->call_return_count; ++i) {
                const uint32_t off = m->call_returns[i] - first;
                words[off / 64] |= 1ull << (off % 64);
            }
            g_return_bits[number] = ReturnBits{first, last - first, words};
        }
    }
    if (m->end <= m->base)
        return;
    const uint8_t mine = number < kPageShared ? (uint8_t)number : kPageShared;
    for (uint32_t page = m->base >> 16; page <= (m->end - 1) >> 16; ++page)
        g_module_page[page] = g_module_page[page] ? kPageShared : mine;
}

// The number of the first registered module whose range holds addr, as the
// list walk answers, or 0. A page of its own names a module below 255, which
// recomp_ic_tables holds by number.
inline uint32_t module_containing_number(uint32_t addr) {
    const uint8_t n = g_module_page[addr >> 16];
    if (n == 0)
        return 0;
    if (n != kPageShared) {
        const RecompModule *m = recomp_ic_tables[n];
        return addr >= m->base && addr < m->end ? n : 0;
    }
    const auto &all = modules();
    for (size_t i = 0; i < all.size(); ++i)
        if (addr >= all[i]->base && addr < all[i]->end)
            return (uint32_t)i + 1;
    return 0;
}
const RecompModule *module_containing(uint32_t addr) {
    const uint32_t n = module_containing_number(addr);
    return n ? modules()[n - 1] : nullptr;
}
int32_t module_index(const RecompModule *m, uint32_t target) {
    uint32_t lo = 0, hi = m->func_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (m->func_addrs[mid] < target)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < m->func_count && m->func_addrs[lo] == target) ? (int32_t)lo : -1;
}

// The entry of the module that owns target as (module, number, index) through
// g_call_cache, which a hit answers with one load; false when no module has
// target as an entry. A miss fills the cache as recomp_module_call always did.
bool module_entry(uint32_t target, const RecompModule **mp, uint32_t *np, int32_t *ip) {
    std::atomic<uint64_t> &slot = g_call_cache[lookup_slot(target)];
    const uint64_t seen = slot.load(std::memory_order_relaxed);
    if (seen && (uint32_t)(seen >> 32) == target) {
        *np = (uint32_t)((seen >> 24) & 0xffu);
        *mp = modules()[*np - 1];
        *ip = (int32_t)(seen & 0xffffffu);
        return true;
    }
    const uint32_t number = module_containing_number(target);
    if (!number)
        return false;
    const RecompModule *m = modules()[number - 1];
    const int32_t i = module_index(m, target);
    if (i < 0)
        return false;
    if (number < 256 && (uint32_t)i < (1u << 24))
        slot.store((uint64_t)target << 32 | (uint64_t)number << 24 | (uint32_t)i, std::memory_order_relaxed);
    *mp = m;
    *np = number;
    *ip = i;
    return true;
}
} // namespace

extern "C" {
uint32_t recomp_return_hits[RECOMP_RETURN_HIT_SLOTS]; // x86.h recomp_return
}
namespace {
void lookup_caches_clear() {
    for (uint32_t i = 0; i < kLookupCache; ++i)
        g_call_cache[i].store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < RECOMP_RETURN_HIT_SLOTS; ++i)
        __atomic_store_n(&recomp_return_hits[i], 0, __ATOMIC_RELAXED);
}
} // namespace

extern "C" {

const int recomp_resumable_stacks = RECOMP_RESUMABLE_STACKS;

// A switched guest stack owns its saved registers and return address. A
// generated CALL propagates a mismatched EIP here instead of continuing in
// the wrong native caller. All CALL continuations are dispatch entries in
// this opt-in translation mode; ordinary games retain their existing path.
void recomp_run(X86 *c, uint32_t target) {
    c->eip = target;
    do {
        recomp_call(c, c->eip);
    } while (recomp_resumable_stacks && c->eip != GUEST_RETURN_SENTINEL);
}

// x86.h's inline call cache: table 0 is the image (recomp_trace_calls_image), table n the n-th module.
const RecompModule *recomp_ic_tables[256];
namespace {
RecompModule g_ic_image;
}
void recomp_module_register(const RecompModule *m) {
    modules().push_back(m);
    // RECOMP_FREE_THREADS=<module> or "none"; without it the game's [game] free_threads, except in a pinned run
    // (RECOMP_PIN_CLOCK): a free thread runs on the machine's schedule, so what it has done by the time the run
    // thread asks differs from run to run (Gothic 3: FMOD Ex's mixer, whose channels the logic asks IsPlaying).
    // Under the baton it runs where the scheduler hands it on, which a pinned run makes the same every time.
    const char *f = recomp_env("FREE_THREADS");
#ifdef RECOMP_FREE_THREADS_DEFAULT
    const char *pin = recomp_env("PIN_CLOCK");
    if (!f && !(pin && *pin && strcmp(pin, "0") != 0))
        f = RECOMP_FREE_THREADS_DEFAULT;
#endif
    if (f)
        if (m->name && *f && strcasestr(f, m->name) && m->end > m->base && g_free_count < FREE_MODULES_MAX) {
            g_free_lo[g_free_count] = m->base;
            g_free_end[g_free_count] = m->end;
            ++g_free_count;
            g_free_hi = std::max(g_free_hi, m->end);
            LOGW("free threads: %s [%08x, %08x) runs its threads without the baton", m->name, m->base, m->end);
        }
    if (modules().size() < 256)
        recomp_ic_tables[modules().size()] = m;
    index_module(m, (uint32_t)modules().size());
    lookup_caches_clear();
}
uint32_t recomp_module_count(void) {
    return (uint32_t)modules().size();
}
const RecompModule *recomp_module_at(uint32_t i) {
    return i < modules().size() ? modules()[i] : nullptr;
}
const RecompModule *recomp_module_named(const char *name) {
    if (!name)
        return nullptr;
    for (const RecompModule *m : modules())
        if (os_strcasecmp(m->name, name) == 0)
            return m;
    return nullptr;
}
// RECOMP_TRACE_CALLS="<module>:<hex address>,..." (diagnostics): each named
// function of a translated module logs its first eight calls and every
// 1000th after, with the return address and ECX (`this`), and then runs as
// before. It rides the hook flag every call site already tests, so it
// costs nothing when unset.
// RECOMP_TRACE_CONTROL=<file> (diagnostics): the same settings, read again
// while the game runs (recomp_trace_control_poll). One KEY=VALUE a line, the
// keys the RECOMP_TRACE_* names without the prefix (CALLS, IF_ARG1, STATS,
// DUMP, DUMP_FROM, ARG, DEREF, STACK; and DRAWS, DRAWS_PS, SKIP_PS, SKIP_DRAWS, QUERY_WAIT, QUERY_NOSYNC, PRESENT_ASYNC, FRAMES, FRAME, PROFILE, below); '#' starts a comment. Each change of the file
// replaces every trace and option, so a key left out is off.
namespace {
struct TracedCall {
    const char *name;
    void (*const *base)(X86 *);
    uint32_t index, addr;
    uint64_t calls;
    // RECOMP_TRACE_STATS: what the last report printed, and the running sum and maximum of the
    // item's field ("@offset[:shift]"), when it names one.
    uint64_t reported, field_sum;
    uint32_t field_max;
    bool field, field_on_stack;
    uint32_t field_at, field_shift;
    // What the slot held before the trace took it, put back when the trace is removed.
    RecompHookFn *hook;
    uint8_t *flag;
    RecompHookFn prior_hook;
    uint8_t prior_flag;
};
struct TraceOptions {
    bool if_arg1 = false, deref = false;
    uint32_t if_arg1_value = 0, stats = 0, dump = 0, from = 0, arg = 0, deref_at = 0, stack = 0, first = 8;
};
TraceOptions g_trace_options;
// The options from a lookup of the RECOMP_TRACE_* names without the prefix.
extern "C++" template <typename Get> TraceOptions trace_options(Get get) {
    TraceOptions o;
    // RECOMP_TRACE_IF_ARG1=<hex>: only the calls whose first stack argument is that value.
    if (const char *e = get("IF_ARG1")) {
        o.if_arg1 = true;
        o.if_arg1_value = (uint32_t)strtoul(e, nullptr, 16);
    }
    // RECOMP_TRACE_STATS=<seconds>: instead of single calls, every that many seconds a line per
    // traced function with its call count. An item "<module>:<address>@<offset, hex>[:<shift>]"
    // adds the sum and maximum of the dword at [ECX+offset] shifted right, e.g. an element count
    // or a depth kept in a bit field; "@s<offset>" reads [ESP+offset] instead (ESP+4 = the first argument).
    if (const char *e = get("STATS"))
        o.stats = (uint32_t)strtoul(e, nullptr, 10);
    // RECOMP_TRACE_DUMP=<bytes, hex>: also that much of the object ECX points at, as words.
    if (const char *e = get("DUMP"))
        o.dump = (uint32_t)strtoul(e, nullptr, 16) & ~3u;
    // RECOMP_TRACE_DUMP_FROM=<offset, hex>: where in the object the dump starts.
    if (const char *e = get("DUMP_FROM"))
        o.from = (uint32_t)strtoul(e, nullptr, 16) & ~3u;
    // RECOMP_TRACE_ARG=<n>: dump the object stack argument n (1 = the first) points at instead of ECX's.
    if (const char *e = get("ARG"))
        o.arg = (uint32_t)strtoul(e, nullptr, 10);
    // RECOMP_TRACE_DEREF=<offset, hex>: dump the object [that+offset] points at instead.
    if (const char *e = get("DEREF")) {
        o.deref = true;
        o.deref_at = (uint32_t)strtoul(e, nullptr, 16);
    }
    // RECOMP_TRACE_STACK=<dwords>: also the call chain, every dword of that many from ESP that is
    // a call continuation (recomp_is_call_return), outermost last. Stale continuations left in
    // the stack's dead slots can show up too; the first ones are the real callers.
    if (const char *e = get("STACK"))
        o.stack = (uint32_t)strtoul(e, nullptr, 10);
    // RECOMP_TRACE_FIRST=<n>: how many first calls of each traced function are reported in full
    // (8 by default; after them every 1000th).
    if (const char *e = get("FIRST"))
        o.first = (uint32_t)strtoul(e, nullptr, 10);
    return o;
}
// The image's own tables, handed over by the host that links them (boot.cpp):
// the runtime's test binaries link no translation.
struct TraceImage {
    void (*const *base)(X86 *) = nullptr;
    RecompHookFn *hooks = nullptr;
    uint8_t *const *flags = nullptr;
} g_trace_image;
std::vector<TracedCall> &traced_calls() {
    static std::vector<TracedCall> v;
    return v;
}
void trace_call(X86 *c, uint32_t index) {
    const TraceOptions &o = g_trace_options;
    for (TracedCall &t : traced_calls()) {
        if (t.index != index)
            continue;
        if (o.if_arg1 && rd32(c->r[4] + 4) != o.if_arg1_value) {
            t.base[index](c);
            return;
        }
        ++t.calls;
        if (o.stats) {
            if (t.field) {
                const uint32_t at = (t.field_on_stack ? c->r[4] : c->r[1]) + t.field_at;
                const uint32_t v = gm_valid(at, 4) ? rd32(at) >> t.field_shift : 0u;
                t.field_sum += v;
                if (v > t.field_max)
                    t.field_max = v;
            }
            static uint64_t next_us = 0;
            static uint32_t tick = 0;
            if ((++tick & 1023) == 0) {
                const uint64_t now = os_monotonic_ns() / 1000u;
                if (now >= next_us) {
                    for (TracedCall &r : traced_calls()) {
                        fprintf(stderr, "[recomp] trace stats: %s %08x calls %llu (+%llu)", r.name, r.addr,
                                (unsigned long long)r.calls, (unsigned long long)(r.calls - r.reported));
                        if (r.field)
                            fprintf(stderr, ", field %s+%x sum %llu max %u", r.field_on_stack ? "ESP" : "ECX", r.field_at,
                                    (unsigned long long)r.field_sum, r.field_max);
                        fprintf(stderr, "\n");
                        r.reported = r.calls;
                    }
                    next_us = now + 1000000ull * o.stats;
                }
            }
            t.base[index](c);
            return;
        }
        if (t.calls <= o.first || t.calls % 1000 == 0) {
            fprintf(stderr, "[recomp] trace: %s %08x call %llu from %08x, ECX=%08x, args %08x %08x\n", t.name,
                    t.addr, (unsigned long long)t.calls, rd32(c->r[4]), c->r[1], rd32(c->r[4] + 4),
                    rd32(c->r[4] + 8));
            if (o.stack) {
                char line[1024];
                int n = snprintf(line, sizeof line, "[recomp] trace:   stack");
                for (uint32_t i = 1; i < o.stack && n < (int)sizeof line - 24; ++i) {
                    const uint32_t at = c->r[4] + 4u * i;
                    if (!gm_valid(at, 4))
                        break;
                    const uint32_t v = rd32(at);
                    if (v >= 0x00400000u && recomp_is_call_return(v))
                        n += snprintf(line + n, sizeof line - n, " +%x:%08x", 4u * i, v);
                }
                fprintf(stderr, "%s\n", line);
            }
            uint32_t obj = c->r[1];
            if (o.arg)
                obj = rd32(c->r[4] + 4u * o.arg);
            if (o.deref && gm_valid(obj + o.deref_at, 4))
                obj = rd32(obj + o.deref_at);
            for (uint32_t at = o.from; o.dump && at < o.from + o.dump && gm_valid(obj + at, 32); at += 32) {
                fprintf(stderr, "[recomp] trace:   +%03x:", at);
                for (uint32_t w = 0; w < 32; w += 4)
                    fprintf(stderr, " %08x", rd32(obj + at + w));
                fprintf(stderr, "\n");
            }
        }
        t.base[index](c);
        if (t.calls <= o.first)
            fprintf(stderr, "[recomp] trace: %s %08x call %llu returned EAX=%08x\n", t.name, t.addr,
                    (unsigned long long)t.calls, c->r[0]);
        return;
    }
}
} // namespace

void recomp_trace_calls_image(void (*const *base)(X86 *), RecompHookFn *hooks, uint8_t *const *flags) {
    g_trace_image.base = base;
    g_trace_image.hooks = hooks;
    g_trace_image.flags = flags;
    g_ic_image = RecompModule{"image", 0, 0, nullptr, 0, base, hooks, flags, nullptr, 0, nullptr};
    recomp_ic_tables[0] = base && flags ? &g_ic_image : nullptr;
}

// The inline call cache's miss: the site's word learns the table and index of a target the image
// or a module owns, and the call runs as recomp_call would run it. A hooked target is cached too;
// recomp_call_cached sends it here while its flag is up.
//
// A site that alternates between targets (a virtual call over several classes) misses on every
// change. The module's entry then comes from recomp_module_call's address cache and the function
// runs from here; before, each miss searched the module's whole entry table and then recomp_call
// looked the target up twice more, 2.6% of a Gothic 3 town's main thread (host profile, 24 Oct
// 2026, nearly half of it on one load of that binary search).
} // extern "C"
extern __thread bool recomp_t_free;
void sched_free_acquire();
void sched_free_release();
// The free module holding addr, or -1.
static int free_module_of(uint32_t addr) {
    for (int i = 0; i < g_free_count; ++i)
        if (addr - g_free_lo[i] < g_free_end[i] - g_free_lo[i])
            return i;
    return -1;
}
bool recomp_free_module_has(uint32_t addr) {
    return free_module_of(addr) >= 0;
}
extern "C" {

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    // A call from a free module to code outside it (a callback the game registered) is never
    // cached, so it always comes here, and on a free thread it runs with the baton. The return
    // address the CALL pushed names the calling module.
    int from;
    if (__builtin_expect(g_free_hi != 0, 0) && (from = free_module_of(rd32(c->r[R_ESP]))) >= 0 &&
        free_module_of(target) != from && !imports_is_trampoline(target)) {
        if (recomp_t_free) {
            sched_free_acquire();
            recomp_call(c, target);
            sched_free_release();
        } else {
            recomp_call(c, target);
        }
        return;
    }
    if (!recomp_profile_enabled && ic && !recomp_frame_watch && imports_is_trampoline(target)) {
        __atomic_store_n(ic, (uint64_t)target << 32 | (uint64_t)RECOMP_IC_SHIM << 24, __ATOMIC_RELAXED);
        recomp_shim_call(c, target);
        return;
    }
    if (!recomp_profile_enabled && ic) {
        uint32_t table = 0;
        const RecompModule *m = recomp_ic_tables[0];
        int32_t i = m ? recomp_index_of(target) : -1;
        if (i < 0 && !module_entry(target, &m, &table, &i))
            i = -1;
        if (i >= 0) {
            if ((uint32_t)i < (1u << 24) && table < RECOMP_IC_SHIM && recomp_ic_tables[table])
                __atomic_store_n(ic, (uint64_t)target << 32 | (uint64_t)table << 24 | (uint32_t)i, __ATOMIC_RELAXED);
            // recomp_call's own path for the same answer, without its frame watch.
            if (!recomp_frame_watch) {
#ifdef RECOMP_NO_HOOKS
                m->base_ptrs[i](c);
#else
                if (__atomic_load_n(m->hooked[i], __ATOMIC_ACQUIRE))
                    m->hook_ptrs[i](c, (uint32_t)i);
                else
                    m->base_ptrs[i](c);
#endif
                return;
            }
        }
    }
    recomp_call(c, target);
}

// seh.h recomp_jump_cached's miss: a function entry the image or a module owns, or an import shim, is learnt by the
// site's word and run as recomp_jump runs it; anything else (a CALL continuation, a gap) is recomp_jump's. A jump from
// a free module (c->eip is the JMP) never learns, so recomp_jump's own path keeps its baton handling.
void recomp_jump_ic(X86 *c, uint32_t target, uint64_t *ic) {
    if (recomp_profile_enabled || !ic || recomp_frame_watch || recomp_seh_pending_target() ||
        (__builtin_expect(g_free_hi != 0, 0) && free_module_of(c->eip) >= 0)) {
        recomp_jump(c, target);
        return;
    }
    if (imports_is_trampoline(target)) {
        __atomic_store_n(ic, (uint64_t)target << 32 | (uint64_t)RECOMP_IC_SHIM << 24, __ATOMIC_RELAXED);
        recomp_shim_call(c, target);
        return;
    }
    uint32_t table = 0;
    const RecompModule *m = recomp_ic_tables[0];
    int32_t i = m ? recomp_index_of(target) : -1;
    if (i < 0 && !module_entry(target, &m, &table, &i))
        i = -1;
    if (i < 0) {
        recomp_jump(c, target);
        return;
    }
    if ((uint32_t)i < (1u << 24) && table < RECOMP_IC_SHIM && recomp_ic_tables[table])
        __atomic_store_n(ic, (uint64_t)target << 32 | (uint64_t)table << 24 | (uint32_t)i, __ATOMIC_RELAXED);
#ifdef RECOMP_NO_HOOKS
    m->base_ptrs[i](c);
#else
    if (__atomic_load_n(m->hooked[i], __ATOMIC_ACQUIRE))
        m->hook_ptrs[i](c, (uint32_t)i);
    else
        m->base_ptrs[i](c);
#endif
}

namespace {
// Puts every traced slot back as it was before the trace took it, newest first.
void trace_remove_all() {
    auto &all = traced_calls();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        __atomic_store_n(it->flag, it->prior_flag, __ATOMIC_RELEASE);
        __atomic_store_n(it->hook, it->prior_hook, __ATOMIC_RELEASE);
        fprintf(stderr, "[recomp] trace: no longer watching %s %08x (%llu calls)\n", it->name, it->addr,
                (unsigned long long)it->calls);
    }
    all.clear();
}
void trace_install_spec(const std::string &all) {
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find(',', at);
        if (end == std::string::npos)
            end = all.size();
        std::string item = all.substr(at, end - at);
        at = end + 1;
        while (!item.empty() && (item.back() == ' ' || item.back() == '\r'))
            item.pop_back();
        while (!item.empty() && item.front() == ' ')
            item.erase(0, 1);
        if (item.empty())
            continue;
        const size_t at_sign = item.find('@');
        const std::string field = at_sign == std::string::npos ? std::string() : item.substr(at_sign + 1);
        if (at_sign != std::string::npos)
            item.resize(at_sign);
        const size_t colon = item.find(':');
        // "exe" names the image itself, whose tables are not in the module registry.
        const std::string module = colon == std::string::npos ? std::string() : item.substr(0, colon);
        const bool image = module == "exe";
        const RecompModule *m = image || module.empty() ? nullptr : recomp_module_named(module.c_str());
        const uint32_t addr = colon == std::string::npos ? 0 : (uint32_t)strtoul(item.c_str() + colon + 1, nullptr, 16);
        const int32_t i = image ? (g_trace_image.base ? recomp_index_of(addr) : -1) : m ? module_index(m, addr) : -1;
        if (i < 0) {
            fprintf(stderr, "[recomp] trace: %s is not a translated function\n", item.c_str());
            continue;
        }
        for (const TracedCall &t : traced_calls())
            if (t.index == (uint32_t)i)
                fprintf(stderr, "[recomp] trace: %s shares its table index with %s %08x; both are reported as one\n",
                        item.c_str(), t.name, t.addr);
        RecompHookFn *hooks = image ? g_trace_image.hooks : m->hook_ptrs;
        uint8_t *flag = image ? g_trace_image.flags[i] : m->hooked[i];
        traced_calls().push_back({image ? "exe" : m->name, image ? g_trace_image.base : m->base_ptrs, (uint32_t)i, addr, 0, 0, 0, 0,
                                 !field.empty(), !field.empty() && field[0] == 's',
                                 (uint32_t)strtoul(field.c_str() + (!field.empty() && field[0] == 's'), nullptr, 16),
                                 field.find(':') == std::string::npos ? 0u
                                     : (uint32_t)strtoul(field.c_str() + field.find(':') + 1, nullptr, 10),
                                 &hooks[i], flag, __atomic_load_n(&hooks[i], __ATOMIC_ACQUIRE),
                                 __atomic_load_n(flag, __ATOMIC_ACQUIRE)});
        __atomic_store_n(&hooks[i], (RecompHookFn)trace_call, __ATOMIC_RELEASE);
        __atomic_store_n(flag, (uint8_t)1, __ATOMIC_RELEASE);
        fprintf(stderr, "[recomp] trace: watching %s %08x\n", image ? "exe" : m->name, addr);
    }
}
} // namespace

void recomp_trace_calls_install(void) {
    g_trace_options = trace_options([](const char *key) { return recomp_env((std::string("TRACE_") + key).c_str()); });
    const char *spec = recomp_env("TRACE_CALLS");
    if (spec && *spec)
        trace_install_spec(spec);
}

namespace {
// The settings in a RECOMP_TRACE_CONTROL file's text: KEY=VALUE lines.
extern "C++" std::map<std::string, std::string> trace_control_parse(const std::string &text) {
    std::map<std::string, std::string> keys;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (size_t hash = line.find('#'); hash != std::string::npos)
            line.resize(hash);
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        auto trim = [](std::string v) {
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
                v.pop_back();
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
                v.erase(0, 1);
            return v;
        };
        std::string key = trim(line.substr(0, eq));
        for (const char *prefix : {"RECOMP_TRACE_", "TRACE_"})
            if (key.rfind(prefix, 0) == 0)
                key.erase(0, strlen(prefix));
        if (!key.empty())
            keys[key] = trim(line.substr(eq + 1));
    }
    return keys;
}
} // namespace

// DRAWS=<n> in the control file: the renderer reports its next n draws in detail (d3d9.cpp sets
// this; the runtime's test binaries link no renderer). Each change of a file that has the key
// opens a new window.
extern "C" {
void (*recomp_trace_draws_hook)(uint32_t count) = nullptr;
}
// SKIP_PS=<pixel shader keys, comma separated, a prefix of 8 hex digits or more> and
// SKIP_DRAWS=<first>-<last> (draws counted from 1 after each Present): the renderer drops those
// draws, to see what each one puts on the screen. DRAWS_PS=<keys, the same way>: the renderer
// reports the next 40 draws with those pixel shaders in detail, wherever they fall in the frame.
// Every applied text sets all three, "" when absent.
extern "C" {
void (*recomp_trace_skip_hook)(const char *skip_ps, const char *skip_draws, const char *draws_ps) = nullptr;
}
// FORCE_PS=<pixel shader keys, as SKIP_PS> and FORCE=<bits>: the renderer draws those shaders' draws with
// render states overridden, to see which state hides them: 1 depth test ALWAYS, 2 blend ONE/ONE, 4 no alpha
// test, 8 every colour channel written, 16 black
// (blend ZERO/ZERO) wherever a fragment lands, 32 stage 0 samples stage 1's texture, 64 no blending. Every applied text sets both, "" when absent.
extern "C" {
void (*recomp_trace_force_hook)(const char *force_ps, const char *force) = nullptr;
}
// QUERY_WAIT=0|1, QUERY_KEEP=0|1, QUERY_VISIBLE=<pixels> and FRAMES=<s>: how the renderer answers
// occlusion queries, and its mean frame time every <s> seconds (d3d9.cpp). Every applied text sets
// all four, "" when absent.
extern "C" {
void (*recomp_trace_perf_hook)(const char *query_wait, const char *frames, const char *query_keep,
                               const char *query_visible) = nullptr;
}
// QUERY_NOSYNC=0|1 and PRESENT_ASYNC=<n>: whether the renderer answers occlusion queries of earlier
// frames and presents without waiting for its render thread (d3d9.cpp). Every applied text sets
// both, "" when absent.
extern "C" {
void (*recomp_trace_sync_hook)(const char *query_nosync, const char *present_async) = nullptr;
}
// The game's own keys: the whole control text goes to a game's native code (game.toml
// [translate] native), which reads the lines it knows; called after every applied change.
extern "C" {
void (*recomp_trace_game_hook)(const char *text) = nullptr;
}

// Applies a RECOMP_TRACE_CONTROL file's settings: every current trace is
// removed, then the file's options and CALLS take effect.
void recomp_trace_control_apply(const char *text) {
    const auto keys = trace_control_parse(text ? text : "");
    trace_remove_all();
    g_trace_options = trace_options([&](const char *key) -> const char * {
        auto it = keys.find(key);
        return it == keys.end() ? nullptr : it->second.c_str();
    });
    auto calls = keys.find("CALLS");
    if (calls != keys.end())
        trace_install_spec(calls->second);
    auto draws = keys.find("DRAWS");
    if (draws != keys.end() && recomp_trace_draws_hook)
        recomp_trace_draws_hook((uint32_t)strtoul(draws->second.c_str(), nullptr, 10));
    if (recomp_trace_skip_hook) {
        auto ps = keys.find("SKIP_PS"), range = keys.find("SKIP_DRAWS"), report = keys.find("DRAWS_PS");
        recomp_trace_skip_hook(ps == keys.end() ? "" : ps->second.c_str(),
                               range == keys.end() ? "" : range->second.c_str(),
                               report == keys.end() ? "" : report->second.c_str());
    }
    if (recomp_trace_force_hook) {
        auto ps = keys.find("FORCE_PS"), bits = keys.find("FORCE");
        recomp_trace_force_hook(ps == keys.end() ? "" : ps->second.c_str(),
                                bits == keys.end() ? "" : bits->second.c_str());
    }
    if (recomp_trace_perf_hook) {
        auto wait = keys.find("QUERY_WAIT"), frames = keys.find("FRAMES"), keep = keys.find("QUERY_KEEP"),
             visible = keys.find("QUERY_VISIBLE");
        recomp_trace_perf_hook(wait == keys.end() ? "" : wait->second.c_str(),
                               frames == keys.end() ? "" : frames->second.c_str(),
                               keep == keys.end() ? "" : keep->second.c_str(),
                               visible == keys.end() ? "" : visible->second.c_str());
    }
    if (recomp_trace_sync_hook) {
        auto nosync = keys.find("QUERY_NOSYNC"), async = keys.find("PRESENT_ASYNC");
        recomp_trace_sync_hook(nosync == keys.end() ? "" : nosync->second.c_str(),
                               async == keys.end() ? "" : async->second.c_str());
    }
    if (recomp_trace_game_hook)
        recomp_trace_game_hook(text ? text : "");
    // FRAME=<anything>: the next presented frame goes to RECOMP_FRAME_DUMP's directory.
    if (keys.count("FRAME"))
        recomp_frame_dump_request();
    // PROFILE=reset|report (RECOMP_PROFILE=1): the guest profiler's counts start again, or its report so
    // far goes to the log now; reset then report times one stretch of play.
    auto prof = keys.find("PROFILE");
    if (prof != keys.end() && prof->second == "reset") {
        profile_reset();
        hostprof_reset();
        sched_hold_reset();
    } else if (prof != keys.end() && prof->second == "report") {
        profile_report_now();
        hostprof_report();
        sched_hold_report();
    }
    fprintf(stderr, "[recomp] trace: control applied, %zu traced\n", traced_calls().size());
}

// The current value of an option (the RECOMP_TRACE_* name without the prefix), 0 when off; for tests.
uint32_t recomp_trace_option(const char *key) {
    const TraceOptions &o = g_trace_options;
    const std::string k = key ? key : "";
    return k == "IF_ARG1" ? (o.if_arg1 ? o.if_arg1_value : 0u) : k == "STATS" ? o.stats : k == "DUMP" ? o.dump
         : k == "DUMP_FROM" ? o.from : k == "ARG" ? o.arg : k == "DEREF" ? (o.deref ? o.deref_at : 0u) : 0u;
}

// Called by the host's pump, which holds the baton: no guest thread is inside
// trace_call while the traces change. Reads the file at most once a second
// and applies it when its text changed (the first read applies it too).
void recomp_trace_control_poll(void) {
    static const char *path = recomp_env("TRACE_CONTROL");
    if (!path || !*path)
        return;
    static uint64_t next_ns = 0;
    static std::string last;
    static bool read_once = false;
    const uint64_t now = os_monotonic_ns();
    if (now < next_ns)
        return;
    next_ns = now + 1000000000ull;
    // No file yet: whatever RECOMP_TRACE_CALLS installed stays.
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    std::string text;
    char block[4096];
    size_t n;
    while ((n = fread(block, 1, sizeof block, f)) > 0 && text.size() < (1u << 20))
        text.append(block, n);
    fclose(f);
    if (read_once && text == last)
        return;
    read_once = true;
    last = text;
    recomp_trace_control_apply(text.c_str());
}

const RecompModule *recomp_module_containing(uint32_t addr) {
    return module_containing(addr);
}
int32_t recomp_module_lookup(uint32_t target) {
    const RecompModule *m = nullptr;
    uint32_t number = 0;
    int32_t i = -1;
    return module_entry(target, &m, &number, &i) ? i : -1;
}
int recomp_module_is_call_return(uint32_t target) {
    const uint32_t number = module_containing_number(target);
    if (!number)
        return 0;
    if (number < 256 && g_return_bits[number].words) {
        const ReturnBits &bits = g_return_bits[number];
        const uint32_t off = target - bits.first;
        return off <= bits.span && ((bits.words[off / 64] >> (off % 64)) & 1u);
    }
    // No continuations, or too far apart for the bits.
    const RecompModule *m = modules()[number - 1];
    uint32_t lo = 0, hi = m->call_return_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (m->call_returns[mid] < target)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < m->call_return_count && m->call_returns[lo] == target;
}
int recomp_module_call(X86 *c, uint32_t target) {
    const RecompModule *m = nullptr;
    uint32_t number = 0;
    int32_t i = -1;
    if (!module_entry(target, &m, &number, &i))
        return 0;
#ifdef RECOMP_NO_HOOKS
    m->base_ptrs[i](c);
#else
    if (__atomic_load_n(m->hooked[i], __ATOMIC_ACQUIRE))
        m->hook_ptrs[i](c, (uint32_t)i);
    else
        m->base_ptrs[i](c);
#endif
    return 1;
}

// Consumes the return address the caller pushed and continues after the call,
// which is what the callee's RET would have done. Without this a call that the
// runtime cannot deliver leaves a dword on the guest stack and every later
// Where an exception record's parameters go: just below the guest stack
// pointer, which the dispatcher copies into its own record before any handler
// runs. A fault can arrive with a stack pointer that is null, tiny or wild -
// that is often WHY it faulted - and ESP - 16 then wraps to the top of the
// address space, so writing there would take the host out instead of the
// guest. Returns false when there is nowhere safe to put them.
static bool exception_info_block(const X86 *c, uint32_t *info) {
    if (!c || c->r[R_ESP] < GUEST_NULL_LIMIT + 16)
        return false;
    const uint32_t at = c->r[R_ESP] - 16;
    if (at + 8 > GUEST_SIZE)
        return false;
    *info = at;
    return true;
}

// frame is displaced by four bytes.
static void return_as_if_ret(X86 *c) {
    c->eip = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4;
    c->r[R_EAX] = 0;
}

// An indirect call whose target landed in the trampoline range.
void recomp_shim_call(X86 *c, uint32_t target) {
    if (imports_dispatch(c, target))
        return;
    LOGW("recomp_shim_call: %08x is not an allocated trampoline (ESP=%08x, return=%08x)", target,
         c->r[R_ESP], rd32(c->r[R_ESP]));
    return_as_if_ret(c);
}

// An indirect call to an address that is neither a translated function nor a
// shim. Almost always a translator bug or an uninitialised function pointer,
// so it is logged with the target and the guest continues with EAX = 0.
void recomp_unknown_call(X86 *c, uint32_t target) {
    if (recomp_run_thunk(c, target))
        return;
    // Code no translation covers runs in the interpreter when every
    // instruction in it decodes: code the guest built or copied into its heap
    // at run time, and code inside the image that discovery missed. The
    // second kind is a translator gap, so the run keeps going and reports it
    // (see discovery.h) instead of returning a zero into whatever asked.
    // Not for a target in the first 64 KB: Windows maps nothing there, so a
    // call through a null pointer faults at the call, and there are no
    // instructions at it to interpret. Reading them would be the very
    // dereference this page exists to refuse.
    if (target >= GUEST_NULL_LIMIT && interp_call(c, target))
        return;
    if (target == GUEST_RETURN_SENTINEL) {
        recomp_callback_return(c);
        // With no matching live callback this is an ordinary undeliverable
        // CALL, whose pushed return still needs to be consumed.
        return_as_if_ret(c);
        return;
    }
    uint32_t ret = rd32(c->r[R_ESP]);
    // Windows maps nothing in the first 64 KB, so a call there - through a nil
    // interface, whose vtable reads back as zero - faults, and the program's
    // own handlers see an access violation. A Delphi program turns that into
    // EAccessViolation, and a try/except around the call carries on: the
    // original drew speech with a surface that could be nil and relied on
    // exactly that. Returning 0 instead ran on with the garbage and aborted at
    // the next jump through it. Only when no handler takes the fault does the
    // call return as before.
    if (target < 0x10000) {
        // EXCEPTION_ACCESS_VIOLATION's two parameters: an execute fault (8) at
        // the target. They sit below the stack pointer, which the dispatcher
        // copies into its own record before any handler runs.
        uint32_t info = 0;
        if (exception_info_block(c, &info)) {
            wr32(info, 8);
            wr32(info + 4, target);
            log_once("null-call",
                     "call to %08x (return=%08x): raising an access violation, as Windows "
                     "would, for the guest's handlers",
                     target, ret);
            recomp_seh_raise(c, 0xc0000005u, 0, 2, info);
        }
    }
    char key[64];
    snprintf(key, sizeof key, "unknown-call:%08x", target);
    // Bounded for the same reason log_once is: a guest that generates code
    // presents a new target on every call, and a run report wants a sample of
    // them, not all of them.
    // The registers say what the call was made ON. A virtual dispatch holds the
    // object in a register and reached this target through the object's vtable,
    // so a null target beside a plausible object means the vtable slot is
    // empty, while a wild object means the pointer never was one.
    if (log_once(key,
                 "call to unknown target %08x (ESP=%08x, return=%08x, EAX=%08x EBX=%08x "
                 "ECX=%08x EDX=%08x ESI=%08x EDI=%08x): returning 0",
                 target, c->r[R_ESP], ret, c->r[R_EAX], c->r[R_EBX], c->r[R_ECX], c->r[R_EDX],
                 c->r[R_ESI], c->r[R_EDI]) &&
        unknown_calls().size() < 256)
        unknown_calls().push_back(UnknownCall{target, ret});
    // Every undeliverable call is an address the run proves is code, so it is
    // also what a regeneration wants back (see discovery.h).
    discovery_note("call", target, ret);
    return_as_if_ret(c);
}

// DIV/IDIV with a zero divisor or an out-of-range quotient. On real hardware
// this raises #DE; there is no SEH here, so it is reported and the registers
// are left untouched.
void recomp_div_error(X86 *c, uint32_t addr) {
    LOGW("divide error at %08x (EAX=%08x EDX=%08x)", addr, c->r[R_EAX], c->r[R_EDX]);
}

// A read or a write through a pointer in the first 64 KB. Windows maps
// nothing there, so the program's own handlers see an access violation and a
// try/except around the dereference carries on; a flat arena hands back a
// zero instead and lets the guest run on with it. NFS Most Wanted walked an
// std::map whose node was null, read _Isnil out of guest 0x15, got that zero
// and looped on the same node with no call into the runtime at all, until the
// watchdog ended the run three minutes later.
void recomp_null_access(uint32_t addr, int write) {
    // Reached only in a build made with -DRECOMP_NULL_CHECKS=1; see x86.h for
    // why the check is not in every build.
    //
    // Off by default even then, and deliberately. Windows would fault here and the
    // guest's own handlers would see it, but this port reaches these reads
    // with pointers Windows would have filled in: the nulls are ours, from
    // a shim that answered 0 or an object nothing built, and raising on them
    // ends the run at the first one instead of at the one that matters.
    // RECOMP_NULL_FAULTS=1 turns the page back into the hole it is on
    // Windows, which is how the std::map spin in NFS Most Wanted was found -
    // a null node whose _Isnil read back as zero, walked forever.
    static const bool fault = recomp_env("NULL_FAULTS") != nullptr;
    if (!fault)
        return;
    X86 *c = guest_current_context();
    uint32_t info = 0;
    // A fault raised at an instruction we are already raising for means the
    // handler returned us onto it to fault again. Tracking the EIP rather
    // than a flag keeps this honest across the raise's longjmp: an unrelated
    // fault elsewhere still gets its exception, and only a genuine loop on
    // one instruction is refused.
    static thread_local uint32_t raising_at = 0;
    if (c && c->eip != raising_at && exception_info_block(c, &info)) {
        // EXCEPTION_ACCESS_VIOLATION's two parameters: the kind of access,
        // 0 for a read and 1 for a write, then the address.
        raising_at = c->eip;
        wr32(info, write ? 1u : 0u);
        wr32(info + 4, addr);
        char key[64];
        snprintf(key, sizeof key, "null-%s:%08x", write ? "write" : "read", addr);
        // The EIP is the last one the translated code stored, which is a
        // call or a branch rather than this instruction: generated bodies do
        // not sync it per access. It names the neighbourhood, not the fault.
        log_once(key,
                 "%s of %08x (near EIP=%08x): raising an access violation, as Windows "
                 "would, for the guest's handlers",
                 write ? "write" : "read", addr, c->eip);
        recomp_seh_raise(c, 0xc0000005u, 0, 2, info);
        raising_at = 0; // it returned, so nothing handled it
    }
    // Returning would put the guest straight back on the same instruction to
    // fault again, which is the spin this exists to end.
    LOGW("unhandled null %s of %08x%s", write ? "write" : "read", addr,
         c ? "" : " with no guest context");
    abort();
}

// An SSE/SSE2 instruction the translator left as a trap (MMX is translated).
// The CPUID below advertises neither extension, so only a guest that skips the
// CPUID check gets here; continuing would compute garbage, so this is fatal.
void recomp_unmodelled(X86 *c, uint32_t addr) {
    LOGW("unmodelled SSE instruction at %08x (ESP=%08x, return=%08x): the guest used a "
         "CPU extension recomp_cpuid does not advertise",
         addr, c->r[R_ESP], rd32(c->r[R_ESP]));
    abort();
}

// INT3: a breakpoint no debugger will handle. On Windows that is an unhandled
// exception, so the process ends; so does this one, with the address.
void recomp_breakpoint(X86 *c, uint32_t addr) {
    LOGW("INT3 at %08x (ESP=%08x, return=%08x): breakpoint reached, stopping", addr, c->r[R_ESP],
         rd32(c->r[R_ESP]));
    abort();
}

// A monotonically increasing cycle counter derived from the millisecond clock,
// so repeated runs see the same ordering.
void recomp_rdtsc(X86 *c) {
    static uint64_t tsc = 0;
    uint64_t from_ms = (uint64_t)host_millis() * 1000000ull;
    if (from_ms > tsc)
        tsc = from_ms;
    tsc += 1000;
    c->r[R_EAX] = (uint32_t)tsc;
    c->r[R_EDX] = (uint32_t)(tsc >> 32);
}

// Deterministic CPUID per the plan: GenuineIntel, family 6, model 3, and only
// FPU, TSC and CMOV. No MMX, so the guest cannot pick a path the translator
// does not cover.
#ifndef RECOMP_CPUID_FEATURES_EDX
#define RECOMP_CPUID_FEATURES_EDX 0u
#endif
#ifndef RECOMP_CPUID_BRAND_INDEX
#define RECOMP_CPUID_BRAND_INDEX 0u
#endif
void recomp_cpuid(X86 *c) {
    switch (c->r[R_EAX]) {
    case 0:
        c->r[R_EAX] = 1;
        memcpy(&c->r[R_EBX], "Genu", 4);
        memcpy(&c->r[R_EDX], "ineI", 4);
        memcpy(&c->r[R_ECX], "ntel", 4);
        break;
    case 1:
        c->r[R_EAX] = 0x00000633; // family 6, model 3, stepping 3
        c->r[R_EBX] = RECOMP_CPUID_BRAND_INDEX; // brand index, [game] cpu_brand_index (default 0 = none)
        c->r[R_ECX] = 0;
        c->r[R_EDX] = 0x00008011u | RECOMP_CPUID_FEATURES_EDX; // FPU | TSC | CMOV + the game's [game] cpu_features
        break;
    default:
        c->r[R_EAX] = c->r[R_EBX] = c->r[R_ECX] = c->r[R_EDX] = 0;
        break;
    }
}

uint32_t recomp_in(X86 *c, uint32_t port, int size) {
    (void)c;
    char key[48];
    snprintf(key, sizeof key, "in:%04x", port);
    log_once(key, "IN from port %04x (%d bytes): returning 0", port, size);
    return 0;
}

void recomp_out(X86 *c, uint32_t port, uint32_t val, int size) {
    (void)c;
    char key[48];
    snprintf(key, sizeof key, "out:%04x", port);
    log_once(key, "OUT to port %04x value %08x (%d bytes): ignored", port, val, size);
}

void recomp_cli(X86 *c) {
    (void)c;
    log_once("cli", "CLI ignored");
}
void recomp_sti(X86 *c) {
    (void)c;
    log_once("sti", "STI ignored");
}
void recomp_hlt(X86 *c) {
    (void)c;
    log_once("hlt", "HLT ignored");
}

void recomp_int(X86 *c, uint32_t vec) {
    LOGW("INT %02x at EIP %08x: no interrupt handling, continuing", vec, c->eip);
}

// ---------------------------------------------------------------------------
// _setjmp / _longjmp intrinsics. See intrinsics.h for the two substitution
// forms and why the two-call one is correct.
// ---------------------------------------------------------------------------
jmp_buf *recomp_setjmp_prepare(X86 *c) {
    uint32_t buf = rd32(c->r[R_ESP] + 4); // cdecl argument 0
    SetjmpRecord *rec;
    {
        std::lock_guard<std::mutex> lock(g_setjmp_mutex);
        SetjmpRecord *&slot = setjmps()[buf];
        if (!slot)
            slot = new SetjmpRecord();
        rec = slot;
    }
    rec->buf = buf;
    rec->saved = *c;
    rec->profile_depth = recomp_profile_depth();
    rec->callback_depth = recomp_callback_depth();
    rec->armed = true;
    g_setjmp_ctx = c;
    // Leave a marker in the guest jmp_buf so a stale buffer is recognisable.
    if (buf) {
        wr32(buf, 0x4d504f50u /* 'POPM' */);
        wr32(buf + 4, buf);
    }
    LOGV("_setjmp(%08x): saved guest state, ESP=%08x", buf, c->r[R_ESP]);
    return &rec->env;
}

void recomp_setjmp_return(X86 *c, int value) {
    uint32_t ret = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4; // cdecl: emulate RET, the caller pops the argument
    c->eip = ret;
    c->r[R_EAX] = (uint32_t)value;
}

// Deliberately fatal. Taking the host setjmp here would save a frame that has
// returned by the time _longjmp fires, so the only safe substitution is the
// two-call form the translator now emits at the call site.
void recomp_setjmp(X86 *c) {
    LOGW("_setjmp reached the single-call intrinsic at ESP=%08x. The call site must emit "
         "{ jmp_buf *b = recomp_setjmp_prepare(c); recomp_setjmp_return(c, setjmp(*b)); } "
         "so the host setjmp belongs to a frame that is still live when _longjmp runs.",
         c->r[R_ESP]);
    abort();
}

static thread_local int g_longjmp_value = 1;
int recomp_longjmp_value(void) {
    return g_longjmp_value;
}

void recomp_longjmp(X86 *c) {
    uint32_t buf = rd32(c->r[R_ESP] + 4);
    int value = (int)rd32(c->r[R_ESP] + 8);
    SetjmpRecord *rec = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_setjmp_mutex);
        auto it = setjmps().find(buf);
        if (it != setjmps().end() && it->second->armed)
            rec = it->second;
    }
    if (!rec) {
        LOGW("_longjmp(%08x, %d): no matching _setjmp, cannot unwind", buf, value);
        abort();
    }
    if (value == 0)
        value = 1; // longjmp(buf, 0) makes setjmp return 1
    // A guest longjmp jumps over every host frame between here and the
    // setjmp, including any mod hook invocation sitting on them. Tell the mod
    // layer so those invocations are abandoned rather than left to run their
    // after hooks against a stack that no longer exists. Before the registers
    // are restored, because the ESP being unwound to is the saved one.
    mods_hooks_unwind_to_esp(rec->saved.r[R_ESP]);
    recomp_profile_truncate(rec->profile_depth);
    recomp_seh_callback_leave(c, rec->callback_depth + 1);
    recomp_callback_truncate(rec->callback_depth);
    *c = rec->saved; // guest registers as they were at the _setjmp
    g_setjmp_ctx = c;
    LOGV("_longjmp(%08x, %d): restoring ESP=%08x", buf, value, c->r[R_ESP]);
    g_longjmp_value = value;
    longjmp(rec->env, value);
}

} // extern "C"
