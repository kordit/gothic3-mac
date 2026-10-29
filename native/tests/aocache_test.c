/* native/tests/aocache_test.c - native/aocache.c (group aocache) against the translated eCAOMeshBase::Compute.
 *
 * Build and run (from the checkout root):
 *   NATIVE="aocache.c ao.c pdo.c rand.c" sh native/tests/build.sh aocache aux-engine/chunk_10354000.c \
 *       aux-engine/chunk_10358000.c aux-engine/chunk_1035c000.c aux-engine/chunk_102ac000.c \
 *       aux-engine/chunk_10468000.c aux-engine/chunk_103f8000.c aux-engine/chunk_10318000.c \
 *       aux-engine/chunk_104d8000.c aux-engine/chunk_105a8000.c aux-sharedbase/chunk_10010000.c \
 *       aux-sharedbase/chunk_10018000.c aux-sharedbase/chunk_10030000.c aux-sharedbase/chunk_10084000.c \
 *       aux-sharedbase/chunk_10074000.c aux-sharedbase/chunk_10070000.c aux-sharedbase/chunk_1006c000.c \
 *       aux-engine/chunk_105ac000.c aux-engine/chunk_1056c000.c aux-engine/chunk_10564000.c \
 *       aux-engine/chunk_10568000.c
 *
 * Random receivers (an eCAOPerTriangleMesh16 with up to 40 disks, or a leaf mesh that is its own emitter as
 * CreateLeafGeometry has it) under random emitter groups (one to three eCAOCustomMesh or leaf meshes, spheres or
 * disks, emitter trees as ao_test builds them), bounding spheres near and far so that FindMeshesOfInterest takes
 * some meshes, all or none, receiving trees with skipped nodes (flag 4), leaves and opened nodes. Each case runs the
 * original (all groups off) against aocache in its two modes from the same state: the tool's (compute and store) and
 * the game's with the result the tool stored (no computation). Compared: every register the callers read, the x87
 * state but its condition codes, all the meshes' memory (every disk's result), the disk counter and the stack from
 * the final ESP up. Then one word the key covers is changed and the game's mode must miss. The cache goes to a
 * new folder under /tmp as HOME (left there). The heap block the original's FindMeshesOfInterest grows comes from a bump allocator outside the
 * compared memory: the ready path allocates nothing. */
#include "difftest_rp.h"

#include <unistd.h>

void fn_10357ba0(X86 *c);
void g3n_10357ba0(X86 *c);
void fn_1031ae00(X86 *c), fn_103fac50(X86 *c), fn_10357f90(X86 *c), fn_102adfd0(X86 *c), fn_10359460(X86 *c);
void fn_10357a90(X86 *c), fn_102ac6a0(X86 *c), fn_10359290(X86 *c), fn_10357c90(X86 *c), fn_10357eb0(X86 *c);
void fn_1046a420(X86 *c), fn_10359300(X86 *c);
void fn_100138d0(X86 *c), fn_10010440(X86 *c), fn_10010480(X86 *c), fn_10010060(X86 *c), fn_1001ae70(X86 *c);
void fn_10010e20(X86 *c), fn_100103b0(X86 *c), fn_1001b580(X86 *c), fn_10010c50(X86 *c);
extern unsigned g3n_aocache_hits, g3n_aocache_misses, g3n_aocache_unknown;
extern int g3n_aocache_store;

#define ESP0 0x0e800000u
#define DATA 0x03000000u
#define DATA_LEN 0x00080000u
#define IDENT (DATA + 0x0u)
#define MESHES (DATA + 0x100u)  /* 0x400 each */
#define NODES (DATA + 0x2000u)  /* 0x6c each */
#define RECS (DATA + 0x40000u)  /* 0x20 each */
#define IDX (DATA + 0x60000u)
#define ALLOC 0x04000000u
#define ADMIN 0x04ff0000u
#define COUNTER 0x10862db0u
#define SOFTNESS 0x10862dbcu

#define SHIM_GETLASTERROR 0x0ff00000u
#define SHIM_SETLASTERROR 0x0ff00010u
#define SHIM_TLSGETVALUE 0x0ff00020u
#define FLSINDEX 5u
#define GETVALUEINDEX 6u
#define PTD 0x02000000u

static uint32_t last_error, bump = ALLOC;
static uint32_t arg_at(X86 *c, unsigned i) {
    return rd32(c->r[4] + 4u + 4u * i);
}
static void ret(X86 *c, unsigned n, uint32_t eax) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + 4u * n;
    c->r[0] = eax;
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    switch (target) {
    case 0x1031ae00u: fn_1031ae00(c); return;
    case 0x103fac50u: fn_103fac50(c); return;
    case 0x10357f90u: fn_10357f90(c); return;
    case 0x102adfd0u: fn_102adfd0(c); return;
    case 0x10359460u: fn_10359460(c); return;
    case 0x10357a90u: fn_10357a90(c); return;
    case 0x102ac6a0u: fn_102ac6a0(c); return;
    case 0x10359290u: fn_10359290(c); return;
    case 0x10357c90u: fn_10357c90(c); return;
    case 0x10357eb0u: fn_10357eb0(c); return;
    case 0x1046a420u: fn_1046a420(c); return;
    case 0x10359300u: fn_10359300(c); return;
    case 0x100138d0u: fn_100138d0(c); return;
    case 0x10010440u: fn_10010440(c); return;
    case 0x10010480u: fn_10010480(c); return;
    case 0x10010060u: fn_10010060(c); return;
    case 0x1001ae70u: fn_1001ae70(c); return;
    case 0x10010e20u: fn_10010e20(c); return;
    case 0x100103b0u: fn_100103b0(c); return;
    case 0x1001b580u: fn_1001b580(c); return;
    case 0x10010c50u: fn_10010c50(c); return;
    case 0x1002a1f0u: ret(c, 0, 0); return;     /* GE_MESSAGEF_INFO (cdecl) */
    case 0x1002e600u: ret(c, 0, IDENT); return; /* bCMatrix::GetIdentity */
    case 0x1000c800u: ret(c, 0, ADMIN); return; /* bCMemoryAdmin::GetInstance */
    case 0x1000c9c0u: {                         /* Realloc(ptr, size), thiscall */
        const uint32_t old = arg_at(c, 0), size = arg_at(c, 1), at = bump;
        bump += (size + 31u) & ~15u;
        if (old)
            memmove(g_mem + at, g_mem + old, size);
        ret(c, 2, at);
        return;
    }
    case 0x1000c9e0u: ret(c, 1, 0); return; /* Free / DeleteObject */
    case SHIM_GETLASTERROR: ret(c, 0, last_error); return;
    case SHIM_SETLASTERROR: last_error = arg_at(c, 0); ret(c, 1, 0); return;
    case SHIM_TLSGETVALUE: {
        const uint32_t i = arg_at(c, 0);
        ret(c, 1, i < 64 ? rd32(c->fs_base + 0x1000u + 4u * i) : 0);
        return;
    }
    }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}
void recomp_call(X86 *c, uint32_t target) {
    recomp_call_ic(c, target, NULL);
}

static float uni(float lo, float hi) {
    return lo + (hi - lo) * (float)(dt_rand32() >> 8) / 16777216.0f;
}
static void wv(uint32_t a, float x, float y, float z) {
    wrf32(a, x);
    wrf32(a + 4u, y);
    wrf32(a + 8u, z);
}
static int one_in(uint32_t n) {
    return dt_rand32() % n == 0;
}

static uint32_t nmeshes, nnodes, nrecs, nidx;
static unsigned ran_cases, self_cases, skipped_nodes;

static uint32_t new_node(void) {
    const uint32_t n = NODES + 0x6cu * nnodes++;
    for (uint32_t i = 0; i < 0x6cu; i += 4)
        wr32(n + i, 0);
    return n;
}
/* A record: position, normal, radius, area (the result's slot for a receiver). */
static void record(uint32_t at, float spread) {
    wv(at, uni(-spread, spread), uni(0.0f, 2.0f * spread), uni(-spread, spread));
    if (one_in(150))
        wrf32(at + 4u * (dt_rand32() % 3u), dt_bits(0x7fc00000u)); /* a NaN now and then: IE raised */
    wv(at + 0xcu, uni(-1, 1), uni(0.2f, 1), uni(-1, 1));
    wrf32(at + 0x18u, one_in(10) ? 0.0f : uni(0.01f, 2.0f));
    wrf32(at + 0x1cu, uni(0.01f, 4.0f));
}
/* A tree over the mesh's records [first, first + count): emitting fields (flag 8, the node as a disk) and receiving
 * ones (flag 4 now and then, flag 2 with indices) on every node, so that both walks find what they read. */
static void fill(uint32_t n, uint32_t arr, uint32_t first, uint32_t count, int depth) {
    record(n, 5.0f);
    wrf32(n + 0x18u, one_in(8) ? 0.0f : uni(0.01f, 3.0f));
    uint32_t flags = (one_in(12) ? 0u : 8u) | (dt_rand32() & 0xf0u);
    if (one_in(14)) {
        flags |= 4u;
        ++skipped_nodes;
    }
    if (depth >= 3 || count < 3 || one_in(3)) {
        flags |= 2u;
        wr32(n + 0x58u, arr);
        wr32(n + 0x5cu, IDX + 4u * nidx);
        wr32(n + 0x60u, count);
        for (uint32_t i = 0; i < count; ++i)
            wr32(IDX + 4u * nidx++, first + i);
    } else {
        uint32_t at = first, left = count;
        for (uint32_t k = 0x38u; k <= 0x54u && left; k += 4u) {
            if (one_in(3))
                continue;
            const uint32_t take = k == 0x54u ? left : 1u + dt_rand32() % left;
            const uint32_t child = new_node();
            wr32(n + k, child);
            fill(child, arr, at, take, depth + 1);
            at += take;
            left -= take;
        }
    }
    wr32(n + 0x68u, flags);
}
/* An eCAOCustomMesh or a leaf mesh (+0x2ac spheres) or an eCAOPerTriangleMesh16: records, six trees, a sphere. */
static uint32_t mesh(uint32_t vt, uint32_t count, float far) {
    const uint32_t m = MESHES + 0x400u * nmeshes++;
    for (uint32_t i = 0; i < 0x400u; i += 4)
        wr32(m + i, 0);
    wr32(m, vt);
    const int tri = vt == 0x1069013cu;
    const uint32_t arr = m + (tri ? 0x48u : 0x4u), nodes = m + (tri ? 0x54u : 0x10u);
    const uint32_t first = nrecs;
    for (uint32_t i = 0; i < count; ++i)
        record(RECS + 0x20u * nrecs++, 6.0f);
    wr32(arr, RECS + 0x20u * first);
    wr32(arr + 4u, count);
    uint32_t at = 0;
    for (uint32_t t = 0; t < 6u; ++t) {
        const uint32_t n = nodes + 0x6cu * t;
        for (uint32_t i = 0; i < 0x6cu; i += 4)
            wr32(n + i, 0);
        const uint32_t take = t == 5u ? count - at : (count - at) / 3u;
        if (take && !one_in(6))
            fill(n, arr, at, take, 0);
        else
            wr32(n + 0x68u, one_in(2) ? 0xfu : 0u);
        at += take;
    }
    const uint32_t sphere = m + (tri ? 0x2e0u : 0x29cu);
    wrf32(sphere, uni(0.5f, 8.0f));
    wv(sphere + 4u, uni(-far, far), uni(-far, far), uni(-far, far));
    if (!tri) {
        wr8(m + 0x298u, one_in(15));
        if (vt == 0x106d0c74u)
            wr8(m + 0x2acu, dt_rand32() & 1u);
    }
    return m;
}

static void wrap(X86 *c) {
    c->r[0] = c->r[1] = c->r[2] = 0;
    memset(c->xmm, 0, sizeof c->xmm);
    c->fpu_sw &= (uint16_t)~0x4700u;
}
static void orig(X86 *c) {
    fn_10357ba0(c);
    wrap(c);
}
static void tool(X86 *c) {
    g3n_aocache_store = 1;
    g3n_10357ba0(c);
    g3n_aocache_store = 0;
    wrap(c);
}
static void game(X86 *c) {
    g3n_10357ba0(c);
    wrap(c);
}

static uint8_t snap[DATA_LEN];
static unsigned misses_bad, hits_bad, node_changes, ie_cases;

static void one_case(void) {
    for (uint32_t a = DATA; a < DATA + DATA_LEN; a += 4)
        wr32(a, 0);
    wrf32(IDENT, 1.0f), wrf32(IDENT + 0x14u, 1.0f), wrf32(IDENT + 0x28u, 1.0f), wrf32(IDENT + 0x3cu, 1.0f);
    nmeshes = nnodes = nrecs = nidx = 0;
    bump = ALLOC;
    wrf32(SOFTNESS, one_in(3) ? 0.0f : uni(-0.3f, 0.3f));
    const int self = one_in(4);
    const float far = one_in(3) ? 30.0f : 4.0f;
    uint32_t recv, emitters;
    if (self) {
        recv = emitters = mesh(0x106d0c74u, 1u + dt_rand32() % 40u, far);
        ++self_cases;
    } else {
        recv = mesh(0x1069013cu, 1u + dt_rand32() % 40u, far);
        emitters = MESHES + 0x400u * nmeshes++;
        const uint32_t n = 1u + dt_rand32() % 3u, list = emitters + 0x40u;
        wr32(emitters, 0x1068ff58u);
        wr32(emitters + 0x4u, list);
        wr32(emitters + 0x8u, n);
        wr32(emitters + 0xcu, n);
        wr8(emitters + 0x10u, 1);
        for (uint32_t i = 0; i < n; ++i)
            wr32(list + 4u * i, mesh(one_in(2) ? 0x106901acu : 0x106d0c74u, dt_rand32() % 30u, far));
    }
    wr32(COUNTER, dt_rand32());
    X86 c;
    dt_context(&c, ESP0 - 0x10u, 0);
    if (one_in(2))
        c.fpu_cw = 0x027f;
    c.fpu_sw = one_in(2) ? 0 : (uint16_t)(dt_rand32() & 0x3fu); /* clean: the flags the walk raises show */
    wr32(c.r[4], 0x1046386fu);
    wr32(c.r[4] + 4u, emitters);
    c.r[1] = recv;
    const X86 start = c;
    memcpy(snap, g_mem + DATA, DATA_LEN);
    const uint32_t counter0 = rd32(COUNTER);

    dt_run("store", &c, orig, tool);
    if (rd32(COUNTER) != counter0)
        ++ran_cases;
    memcpy(g_mem + DATA, snap, DATA_LEN);
    wr32(COUNTER, counter0);
    c = start;
    const unsigned hits = g3n_aocache_hits;
    dt_run("ready", &c, orig, game);
    if (g3n_aocache_hits != hits + 1u)
        ++hits_bad;
    if (c.fpu_sw & ~start.fpu_sw & 1u)
        ++ie_cases;

    /* one word the key covers, changed: a receiver's or an emitter's record, the softness, or the node-as-a-disk of
     * an emitter's tree that has content (flag 8) */
    memcpy(g_mem + DATA, snap, DATA_LEN);
    wr32(COUNTER, counter0);
    uint32_t at = 0;
    switch (dt_rand32() % 4u) {
    case 0: at = RECS + 0x20u * (dt_rand32() % nrecs) + 4u * (dt_rand32() % 8u); break;
    case 1: at = SOFTNESS; break;
    case 2: {
        const uint32_t e = self ? emitters : rd32(rd32(emitters + 0x4u) + 4u * (dt_rand32() % rd32(emitters + 0x8u)));
        for (uint32_t t = 0; t < 6u && !at; ++t)
            if (rd32(e + 0x10u + 0x6cu * t + 0x68u) & 8u)
                at = e + 0x10u + 0x6cu * t + 4u * (dt_rand32() % 8u);
        ++node_changes;
        if (at)
            break;
    }
    /* fall through */
    default: at = rd32(recv + (rd32(recv) == 0x1069013cu ? 0x48u : 0x4u)); break;
    }
    const uint32_t keep = rd32(at);
    wr32(at, keep ^ 1u);
    c = start;
    const unsigned misses = g3n_aocache_misses;
    g3_opt = dt_native_opt;
    game(&c);
    if (g3n_aocache_misses != misses + 1u)
        ++misses_bad;
    wr32(at, keep);
}

int main(void) {
    harness_mem();
    dt_load_pe("Engine.dll");
    dt_load_pe("SharedBase.dll");
    char home[] = "/tmp/aocache-test-XXXXXX";
    if (!mkdtemp(home))
        return 2;
    setenv("HOME", home, 1);
    wr32(0x105f7158u, 0x100138d0u); /* SetVector */
    wr32(0x105f6840u, 0x10010440u); /* Transform */
    wr32(0x105f684cu, 0x10010480u); /* TransformDirection */
    wr32(0x105f6904u, 0x10010060u); /* Normalize */
    wr32(0x105f66ecu, 0x1001ae70u); /* InvTranslate */
    wr32(0x105f677cu, 0x10010e20u); /* GetSquareMagnitude */
    wr32(0x105f6684u, 0x100103b0u); /* Scale(float) */
    wr32(0x105f6854u, 0x1001b580u); /* GetDotProduct */
    wr32(0x105f67c0u, 0x10010c50u); /* GetMagnitude */
    wr32(0x105f6864u, 0x1008b4b0u); /* g_fEpsilon */
    wr32(0x105f63fcu, 0x1008b490u); /* g_fPI */
    wr32(0x105f648cu, 0x1002a1f0u); /* GE_MESSAGEF_INFO */
    wr32(0x105f6764u, 0x1002e600u); /* bCMatrix::GetIdentity */
    wr32(0x105f7230u, 0x1000c800u); /* bCMemoryAdmin::GetInstance */
    wr32(0x105f71d4u, 0x1000c9c0u); /* Realloc */
    wr32(0x105f64b0u, 0x1000c9e0u); /* Free */
    wr32(0x105f7234u, 0x1000c9e0u); /* DeleteObject */
    wr32(0x105f631cu, SHIM_GETLASTERROR);
    wr32(0x105f6274u, SHIM_SETLASTERROR);
    wr32(0x105f6264u, SHIM_TLSGETVALUE);
    wr32(0x10835690u, FLSINDEX);
    wr32(0x10835694u, GETVALUEINDEX);
    wr32(GUEST_TEB_BASE + 0x1000u + 4u * FLSINDEX, PTD);
    wr32(GUEST_TEB_BASE + 0x1000u + 4u * GETVALUEINDEX, SHIM_TLSGETVALUE);
    wr32(0x100891a0u, SHIM_GETLASTERROR);
    wr32(0x100891a4u, SHIM_SETLASTERROR);
    wr32(0x100891d8u, SHIM_TLSGETVALUE);
    wr32(0x100d1158u, FLSINDEX);
    wr32(0x100d115cu, GETVALUEINDEX);
    dt_native_opt = 0xffffffffu & ~(uint32_t)(G3OPT_PDO | G3OPT_RAND);
    dt_stack_floor = ESP0 - 0x8000u;
    dt_region(dt_stack_floor, 0x8100u);
    dt_region(DATA, DATA_LEN);
    dt_region(COUNTER, 4u);
    dt_region(PTD, 0x100u);
    dt_region(GUEST_TEB_BASE, 0x10u);
    for (int i = 0; i < 1500; ++i)
        one_case();
    printf("cases %u (the walk ran in %u, a leaf mesh against itself in %u), skipped nodes %u; hits %u, misses %u, "
           "unknown %u, IE raised %u; a stored result not found %u, a changed input not missed %u\n",
           dt_cases / 2u, ran_cases, self_cases, skipped_nodes, g3n_aocache_hits, g3n_aocache_misses,
           g3n_aocache_unknown, ie_cases, hits_bad, misses_bad);
    if (hits_bad || misses_bad)
        ++dt_fail;
    return dt_report("aocache_test");
}
