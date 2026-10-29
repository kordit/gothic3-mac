/* native/createlod.c - CreateLoD's split of an actor's meshes into hard-skinning groups, without the square search
 * (group createlod; docs/perf/createlod.md, decision of 29.10.2026).
 *
 * eCAnimation_HardRenderActor::CreateLoD (Engine 102a3300) -> FillHardBuffersAndPrimitves (102a8860) -> FUN_1053f420
 * (lod = ECX; actor, bTObjArray of groups {count, capacity, 0x48-byte groups}, max bones; RET 0xc) walks every
 * triangle of every submesh and hands each one to FUN_10548ad0 (ESI = its group; mesh, position in the mesh's index
 * array; RET 8). For each of the three corners that one looks for the group's first vertex record (ESI+8, 0x14 bytes,
 * the corner's index in the submesh at +0x10) from the submesh's first, ESI+0x3c, up to the count, ESI+0: found, the
 * record's index goes to the group's index list; not found, a new record is built from the mesh's influence layer.
 * Each new vertex makes every later walk longer, so a model costs the square of its vertices: in the player's game of
 * 28.10 9% of all the samples of their stutters, frames of up to 1.2 s.
 *
 * Records are only ever appended while 1053f420 runs, and the walk takes the first match, so a map from the key to its
 * first index in [start, count) gives the walk's answer. g3n_1053f420 is the original with that map's lifetime around
 * it: one per group (found by its index in the group array 1053f420 was given, which may move when it grows), checked
 * against the group's start and count on every use and rebuilt from guest memory when they differ. g3n_10548ad0 is
 * all of FUN_10548ad0 in C, every allocation through the same bCMemoryAdmin calls in the same order with the same
 * sizes, so the heap comes out the same; outside 1053f420, or for a group it cannot place, it walks as the original.
 * What a caller sees afterwards is the original's: the memory, EBX, EBP, ESI, EDI, ESP, and XMM0 as each step leaves
 * it (the original's caller reads none of EAX, ECX, EDX). native/tests/createlod_test.c runs both from the same
 * state. */
#include "chunk.h"
#include "g3_opt.h"

#include <stdlib.h>
#include <string.h>

void fn_1053f420(X86 *c);
void fn_10548ad0(X86 *c);
void fn_105461e0(X86 *c); /* the mesh's first influence layer (type 3), -1 for none; RET 4 */
void fn_105416c0(X86 *c); /* (group, index) a vertex record from the template at EDI; RET 8 */
void fn_10295e20(X86 *c); /* the memory admin's singleton, before each of its calls */

#define IAT_GETINSTANCE 0x105f7230u /* SharedBase bCMemoryAdmin::GetInstance, cdecl */
#define IAT_REALLOC 0x105f71d4u     /* bCMemoryAdmin::Realloc(void *, unsigned long), thiscall, RET 8 */
#define IAT_FREE 0x105f64b0u        /* bCMemoryAdmin::Free(void *), thiscall, RET 4 */

static uint64_t ic_getinstance, ic_realloc, ic_free;

static inline void cl_push(X86 *c, uint32_t v) {
    c->r[4] -= 4u;
    wr32(c->r[4], v);
}
/* CALL target from the original's call site `ret`. */
static inline void cl_call(X86 *c, uint32_t ret, uint32_t target, void (*f)(X86 *)) {
    cl_push(c, ret);
    if (recomp_profile_enabled)
        recomp_call(c, target);
    else
        f(c);
}
static inline void cl_icall(X86 *c, uint32_t ret, uint32_t slot, uint64_t *ic) {
    cl_push(c, ret);
    recomp_call_cached(c, rd32(slot), ic);
}

/* CALL 10295e20, then bCMemoryAdmin::GetInstance()->Realloc(ptr, size), from the call sites at[0..2]. */
static uint32_t cl_realloc(X86 *c, uint32_t ptr, uint32_t size, const uint32_t at[3]) {
    cl_call(c, at[0], 0x10295e20u, fn_10295e20);
    cl_push(c, size);
    cl_push(c, ptr);
    cl_icall(c, at[1], IAT_GETINSTANCE, &ic_getinstance);
    c->r[1] = c->r[0];
    cl_icall(c, at[2], IAT_REALLOC, &ic_realloc);
    return c->r[0];
}

/* An array {count at +n, capacity at +n+4, elements at +n+8} grown by one, as the original's inline Add does it: the
 * capacity becomes count + 1 when it is short. Returns the new count. */
static uint32_t cl_add(X86 *c, uint32_t arr, uint32_t esize, const uint32_t at[3]) {
    const uint32_t n = rd32(arr) + 1u;
    wr32(arr, n);
    if ((int32_t)rd32(arr + 4u) < (int32_t)n) {
        const uint32_t cap = n + 1u;
        const uint32_t p = cl_realloc(c, rd32(arr + 8u), cap * esize, at);
        wr32(arr + 8u, p);
        wr32(arr + 4u, cap);
    }
    return rd32(arr);
}
/* The same array grown ahead by `step` elements when its count is a multiple of `step` and the capacity short. */
static void cl_ahead(X86 *c, uint32_t arr, uint32_t step, uint32_t esize, const uint32_t at[3]) {
    const int32_t n = (int32_t)rd32(arr);
    if (n % (int32_t)step != 0)
        return;
    const int32_t want = n + (int32_t)step;
    if ((int32_t)rd32(arr + 4u) >= want)
        return;
    const uint32_t p = cl_realloc(c, rd32(arr + 8u), (uint32_t)want * esize, at);
    wr32(arr + 8u, p);
    wr32(arr + 4u, (uint32_t)want);
}
/* Stores v at element count - 1 of the array, unless that address is 0 (the original's placement-new test). */
static void cl_store_last(uint32_t arr, uint32_t esize, uint32_t v) {
    const uint32_t a = rd32(arr + 8u) + (rd32(arr) - 1u) * esize;
    if (a)
        wr32(a, v);
}

/* ---- the key map of one group --------------------------------------------------------------------------------- */

typedef struct {
    uint32_t gen;   /* the 1053f420 call it belongs to */
    uint32_t start; /* the group's start (ESI+0x3c) it was built for */
    uint32_t n;     /* records [start, n) are in it */
    uint32_t mask;  /* slots - 1, a power of two minus one */
    uint32_t used;
    uint32_t *key, *idx; /* idx ~0u: empty */
} ClMap;

static uint32_t cl_groups; /* the group array of the running 1053f420, 0 outside */
static uint32_t cl_gen;
static ClMap *cl_maps;
static uint32_t cl_nmaps;

static inline uint32_t cl_hash(uint32_t k) {
    return (k * 2654435761u) ^ (k >> 15);
}
static void cl_clear(ClMap *m, uint32_t slots) {
    if (slots > m->mask + 1u || !m->key) {
        free(m->key);
        free(m->idx);
        m->key = malloc(slots * sizeof *m->key);
        m->idx = malloc(slots * sizeof *m->idx);
        m->mask = slots - 1u;
    }
    memset(m->idx, 0xff, (m->mask + 1u) * sizeof *m->idx);
    m->used = 0;
}
static void cl_insert(ClMap *m, uint32_t k, uint32_t i);
static void cl_grow(ClMap *m) {
    const uint32_t slots = (m->mask + 1u) * 2u;
    uint32_t *key = m->key, *idx = m->idx;
    const uint32_t old = m->mask + 1u;
    m->key = malloc(slots * sizeof *m->key);
    m->idx = malloc(slots * sizeof *m->idx);
    m->mask = slots - 1u;
    memset(m->idx, 0xff, slots * sizeof *m->idx);
    m->used = 0;
    for (uint32_t s = 0; s < old; ++s)
        if (idx[s] != ~0u)
            cl_insert(m, key[s], idx[s]);
    free(key);
    free(idx);
}
/* Keeps the first index of a key: records are added in index order. */
static void cl_insert(ClMap *m, uint32_t k, uint32_t i) {
    if ((m->used + 1u) * 2u > m->mask + 1u)
        cl_grow(m);
    for (uint32_t s = cl_hash(k) & m->mask;; s = (s + 1u) & m->mask) {
        if (m->idx[s] == ~0u) {
            m->key[s] = k;
            m->idx[s] = i;
            ++m->used;
            return;
        }
        if (m->key[s] == k)
            return;
    }
}
static uint32_t cl_find(const ClMap *m, uint32_t k) {
    for (uint32_t s = cl_hash(k) & m->mask;; s = (s + 1u) & m->mask) {
        if (m->idx[s] == ~0u)
            return ~0u;
        if (m->key[s] == k)
            return m->idx[s];
    }
}

/* The map of the group at `g`, brought up to its records [start, count); 0 when g is not in the running 1053f420's
 * group array (the caller then walks). */
static ClMap *cl_map(uint32_t g, uint32_t start, uint32_t count) {
    if (!cl_groups)
        return 0;
    const uint32_t base = rd32(cl_groups + 8u), off = g - base;
    if (off % 0x48u || off / 0x48u >= rd32(cl_groups))
        return 0;
    const uint32_t gi = off / 0x48u;
    if (gi >= cl_nmaps) {
        uint32_t n = cl_nmaps ? cl_nmaps : 16u;
        while (n <= gi)
            n *= 2u;
        cl_maps = realloc(cl_maps, n * sizeof *cl_maps);
        memset(cl_maps + cl_nmaps, 0, (n - cl_nmaps) * sizeof *cl_maps);
        cl_nmaps = n;
    }
    ClMap *m = &cl_maps[gi];
    if (m->gen != cl_gen || m->start != start || m->n > count || m->n < start) {
        m->gen = cl_gen;
        m->start = m->n = start;
        cl_clear(m, 256u);
    }
    const uint32_t recs = rd32(g + 8u);
    for (; m->n < count; ++m->n)
        cl_insert(m, rd32(recs + m->n * 0x14u + 0x10u), m->n);
    return m;
}

/* ---- FUN_1053f420 ---------------------------------------------------------------------------------------------- */

/* The key maps belong to one run of 1053f420 over the group array `groups`; begin returns what end restores. */
uint32_t g3n_createlod_begin(uint32_t groups) {
    const uint32_t outer = cl_groups;
    cl_groups = groups;
    ++cl_gen;
    return outer;
}
void g3n_createlod_end(uint32_t outer) {
    cl_groups = outer;
    ++cl_gen;
}

void g3n_1053f420(X86 *c) {
    if (!G3OPT(CREATELOD)) {
        fn_1053f420(c);
        return;
    }
    const uint32_t outer = g3n_createlod_begin(rd32(c->r[4] + 8u));
    fn_1053f420(c);
    g3n_createlod_end(outer);
}

/* ---- FUN_10548ad0 ---------------------------------------------------------------------------------------------- */

static const uint32_t at_ahead_idx[3] = {0x10548b51u, 0x10548b65u, 0x10548b6fu};
static const uint32_t at_ahead_rec[3] = {0x10548bacu, 0x10548bbau, 0x10548bbeu};
static const uint32_t at_ahead_bone[3] = {0x10548bf2u, 0x10548c00u, 0x10548c04u};
static const uint32_t at_hit_idx[3] = {0x10548c69u, 0x10548c7bu, 0x10548c83u};
static const uint32_t at_tmp_alloc[3] = {0x10548d05u, 0x10548d14u, 0x10548d1cu};
static const uint32_t at_bone[3] = {0x10548da2u, 0x10548db4u, 0x10548dbcu};
static const uint32_t at_tmp_add[3] = {0x10548e01u, 0x10548e14u, 0x10548e1cu};
static const uint32_t at_tmp_pad[3] = {0x10548e81u, 0x10548e94u, 0x10548e9cu};
static const uint32_t at_rec[3] = {0x10548ed4u, 0x10548ee6u, 0x10548eeeu};
static const uint32_t at_miss_idx[3] = {0x10548f1fu, 0x10548f31u, 0x10548f39u};

static inline void cl_xmm0(X86 *c, uint32_t bits) {
    c->xmm[0][0] = bits;
    c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0;
}

/* The template record's influences {local bone, weight} grown by one (count at tmp, capacity tmp+4, array tmp+8). */
static void cl_tmp_add(X86 *c, uint32_t tmp, uint32_t bone, uint32_t weight, const uint32_t at[3]) {
    const uint32_t n = rd32(tmp) + 1u;
    wr32(tmp, n);
    if ((int32_t)rd32(tmp + 4u) < (int32_t)n) {
        const uint32_t cap = n + 1u;
        const uint32_t p = cl_realloc(c, rd32(tmp + 8u), cap * 8u, at);
        wr32(tmp + 8u, p);
        wr32(tmp + 4u, cap);
    }
    const uint32_t a = rd32(tmp + 8u) + n * 8u - 8u;
    if (a) {
        wr32(a, bone);
        wr32(a + 4u, weight);
    }
}

void g3n_10548ad0(X86 *c) {
    if (!G3OPT(CREATELOD)) {
        fn_10548ad0(c);
        return;
    }
    const uint32_t esp = c->r[4], ret = rd32(esp), mesh = rd32(esp + 4u), pos = rd32(esp + 8u);
    const uint32_t ebx = c->r[3], ebp = c->r[5], edi = c->r[7], g = c->r[6];
    /* A frame as deep as the original's; the template record where it keeps it. */
    c->r[4] = esp - 0x5cu;
    const uint32_t tmp = c->r[4] + 0x3cu;
    const uint32_t indices = rd32(mesh + 0x20u), vmap = rd32(mesh + 0x1cu);

    cl_push(c, mesh);
    c->r[7] = mesh;
    cl_call(c, 0x10548b0eu, 0x105461e0u, fn_105461e0);
    const uint32_t li = c->r[0];
    const uint32_t layer = li == ~0u ? 0u : rd32(rd32(mesh + 0x3cu) + li * 4u);

    cl_ahead(c, g + 0x24u, 100u, 4u, at_ahead_idx);
    cl_ahead(c, g, 100u, 0x14u, at_ahead_rec);
    cl_ahead(c, g + 0x18u, 20u, 4u, at_ahead_bone);

    for (uint32_t corner = 0; corner < 3u; ++corner) {
        const uint32_t key = rd32(indices + (pos + corner) * 4u);
        const uint32_t count = rd32(g), start = rd32(g + 0x3cu);
        uint32_t hit = ~0u;
        if ((int32_t)start < (int32_t)count) {
            ClMap *m = cl_map(g, start, count);
            if (m)
                hit = cl_find(m, key);
            else {
                const uint32_t recs = rd32(g + 8u);
                for (uint32_t i = start; (int32_t)i < (int32_t)count; ++i)
                    if (rd32(recs + i * 0x14u + 0x10u) == key) {
                        hit = i;
                        break;
                    }
            }
        }
        if (hit != ~0u) {
            cl_add(c, g + 0x24u, 4u, at_hit_idx);
            cl_store_last(g + 0x24u, 4u, hit);
            continue;
        }

        /* The template: no influences yet, the mesh and the key. */
        wr32(tmp, 0);
        wr32(tmp + 4u, 0);
        wr32(tmp + 8u, 0);
        wr32(tmp + 0xcu, mesh);
        wr32(tmp + 0x10u, key);
        if (layer) {
            const uint32_t v = rd32(vmap + key * 4u);
            const uint32_t n = rd32(rd32(layer + 0xcu) + v * 8u + 4u);
            const int32_t per = (int32_t)rd32(g + 0x34u);
            if (per > 0) {
                const uint32_t p = cl_realloc(c, 0, (uint32_t)per * 8u, at_tmp_alloc);
                wr32(tmp + 8u, p);
                wr32(tmp + 4u, (uint32_t)per);
            }
            for (uint32_t j = 0; (int32_t)j < (int32_t)n; ++j) {
                const uint32_t e = rd32(rd32(layer + 0xcu) + v * 8u) + j;
                const uint32_t inf = rd32(layer + 0x18u) + e * 12u;
                const uint32_t weight = rd32(inf + 4u), bone = rd32(inf);
                cl_xmm0(c, weight);
                const int32_t nb = (int32_t)rd32(g + 0x18u);
                uint32_t local = ~0u;
                const uint32_t bones = rd32(g + 0x20u);
                for (int32_t k = 0; k < nb; ++k)
                    if (rd32(bones + (uint32_t)k * 4u) == bone) {
                        local = (uint32_t)k;
                        break;
                    }
                if (local == ~0u) {
                    cl_add(c, g + 0x18u, 4u, at_bone);
                    cl_store_last(g + 0x18u, 4u, bone);
                    local = rd32(g + 0x18u) - 1u;
                }
                cl_xmm0(c, weight);
                cl_tmp_add(c, tmp, local, weight, at_tmp_add);
            }
            int32_t pad = (int32_t)rd32(g + 0x34u) - (int32_t)rd32(tmp);
            if (pad > 0) {
                cl_xmm0(c, 0);
                for (; pad > 0; --pad)
                    cl_tmp_add(c, tmp, 0, 0, at_tmp_pad);
            }
        }
        cl_add(c, g, 0x14u, at_rec);
        cl_push(c, rd32(g) - 1u);
        cl_push(c, g);
        c->r[7] = tmp;
        cl_call(c, 0x10548f04u, 0x105416c0u, fn_105416c0);
        const uint32_t rec = rd32(g) - 1u;
        cl_add(c, g + 0x24u, 4u, at_miss_idx);
        cl_store_last(g + 0x24u, 4u, rec);
        cl_call(c, 0x10548f5eu, 0x10295e20u, fn_10295e20);
        cl_push(c, rd32(tmp + 8u));
        cl_icall(c, 0x10548f69u, IAT_GETINSTANCE, &ic_getinstance);
        c->r[1] = c->r[0];
        cl_icall(c, 0x10548f71u, IAT_FREE, &ic_free);
    }

    c->r[3] = ebx;
    c->r[5] = ebp;
    c->r[6] = g;
    c->r[7] = edi;
    c->eip = ret;
    c->r[4] = esp + 12u;
    recomp_return(c);
}
