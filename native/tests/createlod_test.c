/* native/tests/createlod_test.c - native/createlod.c (group createlod) against the translated Engine code.
 *
 * Build and run (from the checkout root):
 *   NATIVE=createlod.c sh native/tests/build.sh createlod aux-engine/chunk_10548000.c aux-engine/chunk_10544000.c \
 *       aux-engine/chunk_10540000.c aux-engine/chunk_10294000.c
 *
 * FUN_10548ad0 (a triangle's three corners into its group) run as 1053f420 runs it: series of calls over random
 * meshes (vertex maps, index lists with repeated keys, zero to three layers, the influence one or none, zero to six
 * influences a vertex over 40 bones), one to six groups with 0 to 4 influences a vertex, submeshes that reset every
 * group's start, new groups, and the group array moved now and then, as its Realloc moves it. Most series run inside
 * g3n_createlod_begin/end (the key maps), some outside (the walk). Each call runs the original (all groups off) and
 * the native (all on) from the same state and compares the registers, XMM, x87 state and memory: the whole data
 * window with every group, record, index and bone list and the heap, and the stack from the final ESP up. EAX, ECX
 * and EDX are cleared after both: 1053f420 reads none of them after any of its three calls (1053f60e, 1053f76e,
 * 1053f896). bCMemoryAdmin's GetInstance, Realloc and Free are a bump allocator whose state lives in the window, so
 * both runs allocate the same blocks; 10295e20 finds its singleton made. */
#include "difftest_rp.h"

void fn_10548ad0(X86 *c);
void g3n_10548ad0(X86 *c);
uint32_t g3n_createlod_begin(uint32_t groups);
void g3n_createlod_end(uint32_t outer);

#define ESP0 0x0e800000u
#define DATA 0x03000000u
#define DATA_LEN 0x000c0000u
#define BUMP (DATA + 0x0u)       /* the allocator's next free byte */
#define ADMIN (DATA + 0x10u)     /* bCMemoryAdmin */
#define SINGLETON (DATA + 0x40u) /* 10295e20's object: [+8] non-zero */
#define GROUPS (DATA + 0x100u)   /* bTObjArray {count, capacity, groups} */
#define MESH (DATA + 0x1000u)
#define LAYERS (DATA + 0x2000u)  /* the mesh's layer pointers, then the layers at +0x100 apart */
#define VTABLE (DATA + 0x2f00u)
#define VMAP (DATA + 0x4000u)    /* key -> vertex, 4096 keys */
#define INDICES (DATA + 0x8000u) /* 8192 corners */
#define PAIRS (DATA + 0x10000u)  /* per vertex {first influence, count} */
#define INFL (DATA + 0x18000u)   /* influences {bone, weight, unused}, 12 bytes */
#define GARR (DATA + 0x30000u)   /* where the group array goes, moved inside [GARR, GARR + 0x8000) */
#define FREELIST (DATA + 0x38000u) /* per power-of-two class, the last freed block */
#define HEAP (DATA + 0x40000u)
#define HEAP_END (DATA + DATA_LEN)

#define SHIM_GETINSTANCE 0x0ff00000u
#define SHIM_REALLOC 0x0ff00010u
#define SHIM_FREE 0x0ff00020u
#define SHIM_TYPE 0x0ff00030u

static unsigned reallocs, frees;

static void shim_ret(X86 *c, uint32_t pop, uint32_t eax) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + pop;
    c->r[0] = eax;
}
/* Blocks of a power of two from 16 bytes with an 8-byte header {size asked, next free}; a freed block goes to its
 * class's list and is the next one handed out for that class, as a real allocator reuses memory. All of it in the
 * compared window. */
static uint32_t heap_class(uint32_t size) {
    uint32_t k = 4;
    while ((1u << k) < size)
        ++k;
    return k;
}
static uint32_t heap_alloc(uint32_t size) {
    const uint32_t k = heap_class(size);
    if (rd32(FREELIST + 4u * k)) {
        const uint32_t at = rd32(FREELIST + 4u * k);
        wr32(FREELIST + 4u * k, rd32(at - 4u));
        wr32(at - 8u, size);
        wr32(at - 4u, 0);
        return at;
    }
    const uint32_t at = rd32(BUMP) + 8u, next = at + (1u << k);
    if (next > HEAP_END) {
        fprintf(stderr, "test heap full: %u bytes asked, opt %08x\n", size, g3_opt);
        abort();
    }
    wr32(at - 8u, size);
    wr32(at - 4u, 0);
    wr32(BUMP, next);
    return at;
}
static void heap_free(uint32_t at) {
    if (!at)
        return;
    const uint32_t k = heap_class(rd32(at - 8u));
    wr32(at - 4u, rd32(FREELIST + 4u * k));
    wr32(FREELIST + 4u * k, at);
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    const uint32_t esp = c->r[4];
    switch (target) {
    case SHIM_GETINSTANCE:
        shim_ret(c, 0, ADMIN);
        return;
    case SHIM_REALLOC: {
        if (c->r[1] != ADMIN) {
            fprintf(stderr, "Realloc on %08x\n", c->r[1]);
            abort();
        }
        const uint32_t old = rd32(esp + 4u), size = rd32(esp + 8u);
        const uint32_t at = heap_alloc(size);
        uint32_t keep = old ? rd32(old - 8u) : 0;
        if (keep > size)
            keep = size;
        for (uint32_t i = 0; i < size; ++i)
            g_mem[at + i] = i < keep ? g_mem[old + i] : 0xcd;
        heap_free(old);
        ++reallocs;
        shim_ret(c, 8, at);
        return;
    }
    case SHIM_FREE:
        heap_free(rd32(esp + 4u));
        ++frees;
        shim_ret(c, 4, 0);
        return;
    case SHIM_TYPE:
        shim_ret(c, 0, rd32(c->r[1] + 0x40u));
        return;
    }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}
void recomp_call(X86 *c, uint32_t target) {
    recomp_call_ic(c, target, NULL);
}

/* 1053f420 reads none of EAX, ECX, EDX after its calls (see the header). */
static void orig(X86 *c) {
    fn_10548ad0(c);
    c->r[0] = c->r[1] = c->r[2] = 0;
}
static void native(X86 *c) {
    g3n_10548ad0(c);
    c->r[0] = c->r[1] = c->r[2] = 0;
}

static uint32_t ngroups;
static unsigned paths[4]; /* calls in a series with maps, without, groups moved, submeshes */

static void new_group(void) {
    const uint32_t base = rd32(GROUPS + 8u), g = base + ngroups * 0x48u;
    for (uint32_t i = 0; i < 0x48u; i += 4)
        wr32(g + i, 0);
    wr32(g + 0x34u, dt_rand32() % 5u);
    wr32(g + 0x3cu, 0);
    wr32(g + 0x38u, 0);
    wr32(GROUPS, ++ngroups);
}

static void series(int maps) {
    for (uint32_t a = DATA; a < DATA + DATA_LEN; a += 4)
        wr32(a, 0);
    wr32(BUMP, HEAP);
    wr32(SINGLETON + 8u, SINGLETON + 0x20u);
    wr32(0x1086d988u, SINGLETON);
    /* the mesh: keys -> vertices, corners, layers */
    const uint32_t nkeys = 16u + dt_rand32() % 4000u, nverts = 1u + dt_rand32() % 2000u;
    const uint32_t ncorners = 3u * (1u + dt_rand32() % 2000u);
    for (uint32_t k = 0; k < nkeys; ++k)
        wr32(VMAP + 4u * k, dt_rand32() % nverts);
    const uint32_t spread = 1u + dt_rand32() % nkeys; /* keys repeat within a few hundred corners */
    uint32_t ninf = 0;
    for (uint32_t v = 0; v < nverts; ++v) {
        const uint32_t n = dt_rand32() % 8u == 0 ? 0u : dt_rand32() % 7u;
        wr32(PAIRS + 8u * v, ninf);
        wr32(PAIRS + 8u * v + 4u, n);
        for (uint32_t j = 0; j < n && ninf < 8000u; ++j, ++ninf) {
            wr32(INFL + 12u * ninf, dt_rand32() % 40u);
            wrf32(INFL + 12u * ninf + 4u, dt_rand32() % 16u ? (float)(dt_rand32() % 1000u) / 1000.0f : dt_float());
            wr32(INFL + 12u * ninf + 8u, dt_rand32());
        }
    }
    for (uint32_t i = 0; i < ncorners; ++i) {
        const uint32_t base = (i / 300u) * 97u % nkeys;
        wr32(INDICES + 4u * i, (base + dt_rand32() % spread) % nkeys);
    }
    wr32(MESH + 0x1cu, VMAP);
    wr32(MESH + 0x20u, INDICES);
    const uint32_t nlayers = dt_rand32() % 4u;
    wr32(MESH + 0x34u, nlayers);
    wr32(MESH + 0x3cu, LAYERS);
    wr32(VTABLE + 4u, SHIM_TYPE);
    for (uint32_t l = 0; l < nlayers; ++l) {
        const uint32_t obj = LAYERS + 0x100u * (l + 1u);
        wr32(LAYERS + 4u * l, obj);
        wr32(obj, VTABLE);
        wr32(obj + 0x40u, dt_rand32() % 3u ? 3u : dt_rand32() % 6u);
        wr32(obj + 0xcu, PAIRS);
        wr32(obj + 0x18u, INFL);
    }
    /* groups */
    ngroups = 0;
    wr32(GROUPS, 0);
    wr32(GROUPS + 4u, 8u);
    wr32(GROUPS + 8u, GARR);
    const uint32_t want = 1u + dt_rand32() % 6u;
    while (ngroups < want)
        new_group();

    const uint32_t outer = maps ? g3n_createlod_begin(GROUPS) : 0;
    const uint32_t calls = 50u + dt_rand32() % 300u;
    X86 c;
    for (uint32_t i = 0; i < calls; ++i) {
        const uint32_t pick = dt_rand32() % 64u;
        if (pick == 0) { /* a new submesh: every group starts again where it is */
            for (uint32_t gi = 0; gi < ngroups; ++gi) {
                const uint32_t g = rd32(GROUPS + 8u) + gi * 0x48u;
                wr32(g + 0x3cu, rd32(g));
                wr32(g + 0x38u, rd32(g + 0x24u));
            }
            ++paths[3];
        } else if (pick == 1 && ngroups < 8u) {
            new_group();
        } else if (pick == 2) { /* the array's Realloc moved it */
            const uint32_t from = rd32(GROUPS + 8u), to = from == GARR ? GARR + 0x4000u : GARR;
            memmove(g_mem + to, g_mem + from, ngroups * 0x48u);
            memset(g_mem + from, 0xcd, ngroups * 0x48u);
            wr32(GROUPS + 8u, to);
            ++paths[2];
        }
        const uint32_t gi = dt_rand32() % 4u ? dt_rand32() % (ngroups < 2u ? ngroups : 2u) : dt_rand32() % ngroups;
        const uint32_t g = rd32(GROUPS + 8u) + gi * 0x48u;
        const uint32_t pos = 3u * (dt_rand32() % (ncorners / 3u));
        dt_context(&c, ESP0, 0);
        c.r[4] -= 12u;
        wr32(c.r[4], 0x1053f773u);
        wr32(c.r[4] + 4u, MESH);
        wr32(c.r[4] + 8u, pos);
        c.r[6] = g;
        dt_run("FUN_10548ad0", &c, orig, native);
        ++paths[maps ? 0 : 1];
    }
    if (maps)
        g3n_createlod_end(outer);
}

int main(void) {
    harness_mem();
    dt_load_pe("Engine.dll");
    wr32(0x105f7230u, SHIM_GETINSTANCE);
    wr32(0x105f71d4u, SHIM_REALLOC);
    wr32(0x105f64b0u, SHIM_FREE);
    dt_stack_floor = ESP0 - 0x800u;
    dt_region(dt_stack_floor, 0x900u);
    dt_region(DATA, DATA_LEN);
    dt_region(GUEST_TEB_BASE, 0x10u);
    for (int s = 0; s < 60; ++s)
        series(s % 5 != 4);
    printf("calls with maps %u, walking %u; group array moved %u times, submeshes %u; reallocs %u, frees %u\n",
           paths[0], paths[1], paths[2], paths[3], reallocs, frees);
    return dt_report("createlod_test");
}
