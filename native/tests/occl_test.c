/* native/tests/occl_test.c - native/occlusion.c: bookkeeping, and its ResolveOCQuery against the
 * translated original on synthetic structs (a fake eCOcclusionProcessor, gfx admin and device).
 *
 * Build and run from the worktree root (G = the main repo; its build/ holds the generated C):
 *
 *   G="<this repository>"
 *   xcrun clang -std=c11 -O1 -w -DRECOMP_NO_HOOKS \
 *     -I "$G/build/recomp/gen" -I "$G/kit" -I "$G/kit/runtime" -I native \
 *     -DRECOMP_OVERRIDE_HEADER="\"$PWD/native/g3_overrides.h\"" \
 *     native/tests/occl_test.c "$G/build/recomp/gen/aux-engine/chunk_10404000.c" \
 *     kit/tools/recomp/tests/harness.c \
 *     -Wl,-undefined,dynamic_lookup -o /tmp/occl_test && /tmp/occl_test
 *
 * The test includes occlusion.c itself so it can reach the static bookkeeping. The chunk brings
 * the translated fn_10404390 (ResolveOCQuery); FUN_10277b80 (the gfx admin), FUN_1030c340 (free
 * list growth) and the device's GetOcclusionTestInfo (vtable +0x27c) are mocks here, and
 * fn_103fe590 is a mock extraction that drives the two queue call sites the way the real one does
 * (poll at 103fe9df with ESI = queue entry, then the wait at 103feb5b). */
#include "../occlusion.c"

#include <stdio.h>
#include <stdlib.h>

/* x86.h's recomp_return reads this; an older harness.c does not define it. All zero = no cache. */
__attribute__((weak)) uint32_t recomp_return_hits[RECOMP_RETURN_HIT_SLOTS];

uint8_t *harness_mem(void);
volatile uint32_t g3_opt = 0xffffffffu; /* native/g3_opt.c in the game */
volatile uint32_t g3_occl_vis;

/* ---- runtime pieces harness.c leaves to the caller ---------------------------------------- */
uint32_t recomp_frame_watch = 0;
const RecompModule *recomp_ic_tables[256];
void recomp_frame_changed(X86 *c, uint32_t t, uint32_t b, uint32_t a) {
    (void)c, (void)t, (void)b, (void)a;
}
int recomp_is_call_return(uint32_t target) {
    (void)target;
    return 1;
}
int32_t recomp_index_of(uint32_t target) {
    (void)target;
    return -1;
}
void recomp_jump(X86 *c, uint32_t t) {
    (void)c;
    fprintf(stderr, "unexpected jump %08x\n", t);
    abort();
}
void recomp_unknown_jump(X86 *c, uint32_t t) {
    recomp_jump(c, t);
}
void recomp_hook_call(X86 *c, uint32_t t) {
    recomp_jump(c, t);
}
void recomp_store_hooks_update(void) {}

/* ---- the synthetic world ------------------------------------------------------------------ */
#define ADMIN 0x20000000u      /* FUN_10277b80's answer; +0x590 the gfx assistant admin */
#define DEVICE 0x20001000u     /* [ADMIN+0x590+0xc] */
#define VTABLE 0x20002000u     /* [DEVICE] */
#define DEV_INFO 0x0fd10000u   /* [VTABLE+0x27c]: GetOcclusionTestInfo, a mock */
#define PROC 0x20003000u       /* eCOcclusionProcessor: +0xe free count, +0x10 free array */
#define FREE_ARRAY 0x20004000u
#define DB 0x20005000u         /* eCFrustumDatabase: +4 frame counter */
#define QUEUE 0x20006000u      /* the extraction's 0x18-byte query queue */
#define STACK_TOP 0x2f000000u
#define CALLER_RET 0x103fef7cu /* after ExtractFromFrustum's CALL 0x103fe590 */
#define RET_DYNAMIC 0x1040356cu /* after ResolveDynamicQuery's CALL 0x10404390 */

#define MAXQ 256
static struct {
    int live, ready;
    uint32_t count, frame;
} dev[MAXQ + 1];
static uint32_t dev_next = 1;
static unsigned dev_calls, dev_waits;

static void ret_n(X86 *c, uint32_t n) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + n;
}

void fn_10277b80(X86 *c) {
    c->r[0] = ADMIN;
    ret_n(c, 0);
}
void fn_1030c340(X86 *c) { /* the free array is preallocated */
    ret_n(c, 0);
}
void fn_103fe3c0(X86 *c) {
    recomp_jump(c, 0x103fe3c0u);
}
void fn_103fe3f0(X86 *c) {
    recomp_jump(c, 0x103fe3f0u);
}

/* eCDX9Device::GetOcclusionTestInfo(query, &count, wait), RET 0xc: 1 with the count once the GPU
 * has run the query; with wait it "spins", which here is the GPU finishing it on the spot. */
static void dev_info(X86 *c) {
    const uint32_t q = rd32(c->r[4] + 4u), out = rd32(c->r[4] + 8u), wait = rd32(c->r[4] + 12u) & 0xffu;
    ++dev_calls;
    uint32_t r = 0;
    if (q && q <= MAXQ && dev[q].live) {
        if (!dev[q].ready && wait) {
            ++dev_waits;
            dev[q].ready = 1;
        }
        if (dev[q].ready) {
            wr32(out, dev[q].count);
            r = 1;
        }
    }
    c->r[0] = r;
    c->r[2] = 0xdead0000u | q; /* clobbered, as a real call would */
    ret_n(c, 12);
}
void recomp_call(X86 *c, uint32_t target) {
    if (target == DEV_INFO) {
        dev_info(c);
        return;
    }
    recomp_jump(c, target);
}
void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    recomp_call(c, target);
}

static uint16_t free_n(void) {
    return (uint16_t)(rd32(PROC + 0xcu) >> 16);
}

static void world_reset(void) {
    harness_mem();
    memset(g_mem + 0x20000000u, 0, 0x10000u);
    wr32(ADMIN + 0x590u + 0xcu, DEVICE);
    wr32(DEVICE, VTABLE);
    wr32(VTABLE + 0x27cu, DEV_INFO);
    wr32(PROC + 0x10u, FREE_ARRAY);
    memset(dev, 0, sizeof dev);
    dev_next = 1;
    dev_calls = dev_waits = 0;
    memset(occl_hist, 0, sizeof occl_hist);
    occl_pending_n = 0;
    memset(&occl_stats, 0, sizeof occl_stats);
    memset(&occl_cur, 0, sizeof occl_cur);
}

/* StartOCQuery's choice of handle: the free list from its end, the first one the device has
 * finished, else a new one. */
static uint32_t start_query(uint32_t frame, uint32_t visible_count) {
    uint32_t q = 0;
    uint16_t n = free_n();
    for (int i = n - 1; i >= 0 && !q; --i) {
        uint32_t h = rd32(FREE_ARRAY + 4u * (uint32_t)i);
        if (dev[h].ready) {
            q = h;
            for (int j = i; j < n - 1; ++j)
                wr32(FREE_ARRAY + 4u * (uint32_t)j, rd32(FREE_ARRAY + 4u * (uint32_t)(j + 1)));
            wr16(PROC + 0xeu, (uint16_t)(n - 1));
        }
    }
    if (!q)
        q = dev_next++;
    dev[q].live = 1;
    dev[q].ready = 0;
    dev[q].count = visible_count;
    dev[q].frame = frame;
    return q;
}
static void gpu_finish(uint32_t frame) {
    for (uint32_t q = 1; q < dev_next; ++q)
        if (dev[q].live && dev[q].frame <= frame)
            dev[q].ready = 1;
}

/* One call of ResolveOCQuery as a translated caller makes it: ESI = queue entry. */
static uint32_t call_resolve(X86 *c, void (*fn)(X86 *), uint32_t ret, uint32_t entry, uint32_t query, uint32_t wait) {
    c->r[4] -= 4u;
    wr32(c->r[4], wait);
    c->r[4] -= 4u;
    wr32(c->r[4], query);
    c->r[4] -= 4u;
    wr32(c->r[4], ret);
    c->r[1] = PROC;
    c->r[6] = entry;
    fn(c);
    return c->r[0];
}

/* ---- the mock extraction -------------------------------------------------------------------- */
#define MAXN 16
static uint32_t script_nodes[MAXN], script_counts[MAXN], script_n;
static uint32_t script_answer[MAXN];
static uint32_t script_frame;

/* What ExtractFromFrustumOC does with its queue: every node gets a query, the poll site runs on
 * each entry, and the ones still unresolved go through the wait site. */
void fn_103fe590(X86 *c) {
    const uint32_t esi = c->r[6], edi = c->r[7], ebx = c->r[3], ebp = c->r[5];
    for (uint32_t i = 0; i < script_n; ++i) {
        uint32_t e = QUEUE + 0x18u * i;
        wr32(e, start_query(script_frame, script_counts[i]));
        wr32(e + 4u, 0xffffffffu);
        wr32(e + 8u, script_nodes[i]);
    }
    for (uint32_t i = 0; i < script_n; ++i) {
        uint32_t e = QUEUE + 0x18u * i;
        uint32_t r = call_resolve(c, FN_10404390, OCCL_RET_POLL, e, rd32(e), 0);
        wr32(e + 4u, r);
        if (r == 0xffffffffu)
            wr32(e + 4u, call_resolve(c, FN_10404390, OCCL_RET_WAIT, e, rd32(e), 1));
        script_answer[i] = rd32(e + 4u);
    }
    c->r[6] = esi, c->r[7] = edi, c->r[3] = ebx, c->r[5] = ebp;
    ret_n(c, 0);
}

static unsigned fails, checks;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(cond)) {                                                                             \
            ++fails;                                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

static X86 cpu;
static void cpu_reset(void) {
    memset(&cpu, 0, sizeof cpu);
    cpu.r[4] = STACK_TOP;
    cpu.r[3] = 0x33333333u, cpu.r[5] = 0x55555555u, cpu.r[7] = 0x77777777u;
}

/* One frame: ExtractFromFrustum counts the database frame up and calls ExtractFromFrustumOC. */
static void extract(uint32_t n, const uint32_t *nodes, const uint32_t *counts, uint32_t step) {
    script_n = n;
    for (uint32_t i = 0; i < n; ++i)
        script_nodes[i] = nodes[i], script_counts[i] = counts[i];
    wr32(DB + 4u, rd32(DB + 4u) + step);
    script_frame = rd32(DB + 4u);
    cpu.r[4] -= 4u;
    wr32(cpu.r[4], CALLER_RET);
    cpu.r[1] = DB;
    g3n_103fe590(&cpu);
    CHECK(cpu.r[4] == STACK_TOP);
    CHECK(cpu.eip == CALLER_RET);
}
static void frame(uint32_t n, const uint32_t *nodes, const uint32_t *counts) {
    extract(n, nodes, counts, 1);
}

/* ---- tests ---------------------------------------------------------------------------------- */
static void test_history(void) {
    world_reset();
    CHECK(!occl_hist_find(DB, 0x100));
    occl_hist_put(DB, 0x100, 5, 40);
    const OcclHist *h = occl_hist_find(DB, 0x100);
    CHECK(h && h->count == 40 && h->frame == 5);
    occl_hist_put(DB, 0x100, 4, 99); /* older: ignored */
    CHECK(occl_hist_find(DB, 0x100)->count == 40);
    occl_hist_put(DB, 0x100, 6, 0);
    CHECK(occl_hist_find(DB, 0x100)->count == 0 && occl_hist_find(DB, 0x100)->frame == 6);
    CHECK(!occl_hist_find(DB + 0x100, 0x100)); /* another database */
    wr32(DB + 0x1b4u, 0);                      /* the database's threshold: visible above 0 */
    occl_cur.still = 0;                        /* the frustum moved: hidden waits for the device */
    g3_opt = 0xffffffffu & ~(uint32_t)G3OPT_OCCLMOVE;
    CHECK(occl_answer(DB, 0x100, 8) == OCCL_NOT_READY && occl_stats.waited == 1);
    g3_opt = 0xffffffffu;                      /* occlmove: drawn instead, nothing waits */
    CHECK(occl_answer(DB, 0x100, 8) == OCCL_VISIBLE && occl_stats.moved_visible == 1 && occl_stats.waited == 1);
    occl_hist_put(DB, 0x180, 6, 7);
    CHECK(occl_answer(DB, 0x180, 8) == 7); /* visible before: used even with the frustum moved */
    occl_cur.still = 1;
    CHECK(occl_answer(DB, 0x100, 8) == 0);     /* age 2 */
    CHECK(occl_answer(DB, 0x100, 9) == OCCL_VISIBLE); /* age 3 */
    CHECK(occl_answer(DB, 0x100, 5) == OCCL_VISIBLE); /* from the future: a new database */
    CHECK(occl_answer(DB, 0x200, 6) == OCCL_VISIBLE); /* never queried */

    /* Fill far past capacity: lookups stay bounded and the newest keys survive. */
    for (uint32_t i = 1; i <= 3u * OCCL_HIST_SLOTS; ++i) {
        ++occl_tick;
        occl_hist_put(DB, 0x01000000u + i * 0x40u, 10, i);
    }
    unsigned found = 0;
    for (uint32_t i = 3u * OCCL_HIST_SLOTS - 999u; i <= 3u * OCCL_HIST_SLOTS; ++i) {
        const OcclHist *e = occl_hist_find(DB, 0x01000000u + i * 0x40u);
        found += e && e->count == i;
    }
    CHECK(found == 1000);
}

static void test_pending_overflow(void) {
    world_reset();
    for (uint32_t i = 0; i < OCCL_PENDING_MAX + 5u; ++i)
        occl_pending_add(PROC, i + 1u, DB, 0x100u + i, 1);
    CHECK(occl_pending_n == OCCL_PENDING_MAX);
    CHECK(occl_stats.dropped == 5);
    CHECK(occl_pending[0].query == 6u);
    CHECK(occl_pending[OCCL_PENDING_MAX - 1u].query == OCCL_PENDING_MAX + 5u);
}

/* Off, or on but called from somewhere else, g3n_10404390 is the original: same EAX, registers,
 * stack, free list and device calls. */
static void diff_one(uint32_t opt, uint32_t ret, int in_extraction, uint32_t wait, int ready, uint32_t query_zero) {
    uint8_t snap_mem[0x8000];
    X86 a, b;
    unsigned calls_a, waits_a;
    g3_opt = opt;
    world_reset();
    uint32_t q = query_zero ? 0u : start_query(1, 77);
    dev[q].ready = ready;
    /* two handles already free */
    wr32(FREE_ARRAY, start_query(0, 1));
    wr32(FREE_ARRAY + 4u, start_query(0, 1));
    wr16(PROC + 0xeu, 2);
    wr32(QUEUE + 8u, 0x4242u);
    occl_cur.db = in_extraction ? DB : 0u;
    occl_cur.frame = 1;
    memcpy(snap_mem, g_mem + 0x20000000u, sizeof snap_mem);
    __typeof__(dev) snap_dev;
    memcpy(snap_dev, dev, sizeof dev);

    cpu_reset();
    call_resolve(&cpu, fn_10404390, ret, QUEUE, q, wait);
    a = cpu;
    calls_a = dev_calls, waits_a = dev_waits;
    uint8_t after_a[0x8000];
    memcpy(after_a, g_mem + 0x20000000u, sizeof after_a);

    memcpy(g_mem + 0x20000000u, snap_mem, sizeof snap_mem);
    memcpy(dev, snap_dev, sizeof dev);
    dev_calls = dev_waits = 0;
    cpu_reset();
    call_resolve(&cpu, g3n_10404390, ret, QUEUE, q, wait);
    b = cpu;

    CHECK(a.r[0] == b.r[0]);
    for (int r = 1; r < 8; ++r)
        CHECK(a.r[r] == b.r[r]);
    CHECK(a.eip == b.eip && b.eip == ret);
    CHECK(b.r[4] == STACK_TOP);
    CHECK(calls_a == dev_calls && waits_a == dev_waits);
    CHECK(!memcmp(after_a, g_mem + 0x20000000u, sizeof after_a));
    CHECK(occl_pending_n == 0);
}

static void test_differential(void) {
    const uint32_t all = 0xffffffffu, off = 0xffffffffu & ~(uint32_t)G3OPT_OCCL;
    for (int ready = 0; ready < 2; ++ready)
        for (uint32_t wait = 0; wait < 2; ++wait) {
            diff_one(off, OCCL_RET_WAIT, 1, wait, ready, 0); /* group off */
            diff_one(off, OCCL_RET_POLL, 1, wait, ready, 0);
            diff_one(all, RET_DYNAMIC, 1, wait, ready, 0);   /* other caller */
            diff_one(all, OCCL_RET_WAIT, 0, wait, ready, 0); /* outside an extraction */
            diff_one(all, OCCL_RET_WAIT, 1, wait, ready, 1); /* null query: original answers 0 */
            diff_one(all, OCCL_RET_POLL, 1, wait, ready, 1);
        }
}

/* The replaced path: register effects of one wait-site call, as the original's exit. */
static void test_wait_site_abi(void) {
    g3_opt = 0xffffffffu;
    world_reset();
    occl_cur.db = DB, occl_cur.frame = 3;
    uint32_t q = start_query(3, 5);
    wr32(QUEUE + 8u, 0x4242u);
    cpu_reset();
    cpu.r[0] = 0x11111111u, cpu.r[2] = 0x22222222u;
    call_resolve(&cpu, g3n_10404390, OCCL_RET_WAIT, QUEUE, q, 1);
    CHECK(cpu.r[0] == OCCL_VISIBLE);
    CHECK(cpu.r[1] == PROC && cpu.r[3] == 0x33333333u && cpu.r[5] == 0x55555555u);
    CHECK(cpu.r[6] == QUEUE && cpu.r[7] == 0x77777777u);
    CHECK(cpu.r[4] == STACK_TOP && cpu.eip == OCCL_RET_WAIT);
    CHECK(dev_calls == 0 && dev_waits == 0);
    CHECK(occl_pending_n == 1 && occl_pending[0].query == q && occl_pending[0].node == 0x4242u);
    CHECK(occl_pending[0].proc == PROC && occl_pending[0].frame == 3);
}

static void test_frames(void) {
    const uint32_t nodes[3] = {0xa000, 0xb000, 0xc000};
    const uint32_t vis[3] = {50, 0, 9};
    g3_opt = 0xffffffffu;
    world_reset();
    cpu_reset();

    /* Frame 1: nothing known yet, everything visible, no device call at all. */
    frame(3, nodes, vis);
    CHECK(script_answer[0] == OCCL_VISIBLE && script_answer[1] == OCCL_VISIBLE && script_answer[2] == OCCL_VISIBLE);
    CHECK(dev_calls == 0 && dev_waits == 0);
    CHECK(occl_pending_n == 3 && occl_stats.polls_skipped == 3 && occl_stats.answered_visible == 3);
    CHECK(free_n() == 0);

    /* The GPU runs frame 1. Frame 2 reads those three (no wait), hands the handles back to the
     * pool, answers from them, and its own new queries reuse the handles. */
    gpu_finish(1);
    frame(3, nodes, vis);
    CHECK(script_answer[0] == 50 && script_answer[1] == 0 && script_answer[2] == 9);
    CHECK(dev_waits == 0);
    CHECK(occl_stats.read == 3 && occl_pending_n == 3);
    CHECK(dev_next == 4); /* the three handles were reused, no fourth query was created */

    /* The GPU is behind: frame 3 cannot read frame 2's queries, answers from frame 1 (age 2). */
    frame(3, nodes, vis);
    CHECK(script_answer[0] == 50 && script_answer[1] == 0 && script_answer[2] == 9);
    CHECK(dev_waits == 0 && occl_stats.not_ready == 3);
    CHECK(occl_pending_n == 6 && dev_next == 7); /* nothing came back to the pool: new handles */

    /* Frame 4: still nothing back; frame 1's results are now too old, so "visible". */
    frame(3, nodes, vis);
    CHECK(script_answer[0] == OCCL_VISIBLE && script_answer[1] == OCCL_VISIBLE);
    CHECK(dev_waits == 0);

    /* The GPU catches up; B becomes visible. Frame 5 reads frames 2-4 and uses frame 4's. */
    gpu_finish(4);
    const uint32_t vis2[3] = {50, 30, 9};
    frame(3, nodes, vis2);
    CHECK(script_answer[0] == 50 && script_answer[1] == 0 && script_answer[2] == 9); /* frame 4 issued with vis */
    CHECK(occl_pending_n == 3 && dev_waits == 0);
    gpu_finish(5);
    frame(3, nodes, vis2);
    CHECK(script_answer[1] == 30); /* one frame late */

    /* Node C leaves the queue (its parent stops being traversed) for three frames, then returns:
     * its old result is stale, so it is visible rather than wrongly hidden. */
    for (int i = 0; i < 3; ++i) {
        gpu_finish(rd32(DB + 4u));
        frame(2, nodes, vis2);
    }
    gpu_finish(rd32(DB + 4u));
    const uint32_t vis3[3] = {50, 30, 0};
    frame(3, nodes, vis3);
    CHECK(script_answer[2] == OCCL_VISIBLE);
    CHECK(dev_waits == 0);
}

/* A query the GPU never answers is forgotten after OCCL_DROP_AGE frames, not waited for. */
static void test_drop(void) {
    const uint32_t node[1] = {0xd000}, cnt[1] = {1};
    g3_opt = 0xffffffffu;
    world_reset();
    cpu_reset();
    frame(1, node, cnt);
    dev[1].live = 0; /* lost handle: the device never answers it */
    for (uint32_t i = 0; i < OCCL_DROP_AGE + 2u; ++i)
        frame(0, node, cnt);
    CHECK(occl_stats.dropped == 1);
    CHECK(dev_waits == 0);
}

/* The original against the same frames: it waits on every node it cannot read at once. */
static void test_original_waits(void) {
    const uint32_t nodes[3] = {0xa000, 0xb000, 0xc000};
    const uint32_t vis[3] = {50, 0, 9};
    g3_opt = 0xffffffffu & ~(uint32_t)G3OPT_OCCL;
    world_reset();
    cpu_reset();
    frame(3, nodes, vis);
    CHECK(script_answer[0] == 50 && script_answer[1] == 0 && script_answer[2] == 9);
    CHECK(dev_waits == 3);
    CHECK(free_n() == 3);
}

/* Switched off with queries still held: the next extraction reads them back into the pool. */
static void test_switch_off(void) {
    const uint32_t nodes[2] = {0xa000, 0xb000}, vis[2] = {5, 6};
    g3_opt = 0xffffffffu;
    world_reset();
    cpu_reset();
    frame(2, nodes, vis);
    CHECK(occl_pending_n == 2);
    gpu_finish(1);
    g3_opt = 0xffffffffu & ~(uint32_t)G3OPT_OCCL;
    frame(2, nodes, vis);
    CHECK(occl_pending_n == 0 && occl_stats.read == 2);
    CHECK(script_answer[0] == 5 && script_answer[1] == 6);
}

/* A second extraction in the same database frame leaves that frame's queries alone: their draws
 * would still be in the open command buffer, and asking costs a commit in the kit. */
static void test_same_frame_twice(void) {
    const uint32_t nodes[2] = {0xa000, 0xb000}, vis[2] = {5, 6};
    g3_opt = 0xffffffffu;
    world_reset();
    cpu_reset();
    frame(2, nodes, vis);
    gpu_finish(1);
    extract(0, nodes, vis, 0);
    CHECK(dev_calls == 0 && occl_pending_n == 2);
    frame(0, nodes, vis);
    CHECK(dev_calls == 2 && occl_pending_n == 0);
}

int main(void) {
    test_history();
    test_pending_overflow();
    test_differential();
    test_wait_site_abi();
    test_frames();
    test_drop();
    test_original_waits();
    test_switch_off();
    test_same_frame_twice();
    printf("occl_test: %u checks, %u failed\n", checks, fails);
    return fails != 0;
}
