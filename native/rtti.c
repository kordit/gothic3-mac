/* native/rtti.c - dynamic_cast with its answers remembered (group rtti).
 *
 * A quick save (F5) stops the game for about 3 s (docs/perf/wielordzeniowosc.md, "Rewizja"): inside
 * gCWorld::SaveGameWorld (Game 10aac6b0) ___RTDynamicCast (10ae9c77), its FindMITargetTypeInstance (10ae9a21) and
 * the _strcmp of type names under it (10aed020) take about 15% of the samples (work/sample-f5-1.txt). Each call walks
 * the class hierarchy of the object's complete type and compares type names, and the save makes the same few casts on
 * thousands of objects.
 *
 * ___RTDynamicCast(inptr, VfDelta, SrcType, TargetType, isReference), cdecl, EAX = the cast pointer or 0. The
 * object's complete object locator is [[inptr] - 4] (+4 offset, +8 cdOffset, +0x10 the class hierarchy, whose +4
 * attributes have bit 0 for multiple and bit 1 for virtual inheritance). For a given locator, VfDelta, SrcType and
 * TargetType the answer is fixed: the target base at a constant offset from the complete object (inptr - offset), or
 * none. Only locators without virtual inheritance and with cdOffset 0 are remembered, which keeps the complete object
 * at inptr - offset and every base at a constant place in it. The first call of each combination runs the original
 * and its result is kept; a later one is answered from the table. A failed cast with isReference set (which throws
 * bad_cast) always runs the original.
 *
 * G3_RTTI_CHECK=1 runs the original for every call too and logs any answer that differs. Each module that links its own
 * copy (the exe, Engine, SharedBase, Game, GUI, Script, Script_Game, Importer: the VS2005 CRT's) has a
 * g3n_ of its own, with the same arguments. Callers were guest code under the baton; with the logic on a thread of
 * its own beside the render (stage 5, pipe.c) two can cast at once, so each entry has a sequence count: odd while it is
 * written, read twice around the key. A reader that sees it change takes the original's way; a writer that finds it
 * odd leaves it to the other. */
#include "chunk.h"
#include "g3_opt.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

void fn_10ae9c77(X86 *c); /* Game ___RTDynamicCast */
void fn_004058e9(X86 *c); /* Gothic3Final.exe ___RTDynamicCast */
void fn_10e2e179(X86 *c); /* GUI ___RTDynamicCast */
void fn_1106dd34(X86 *c); /* Script_Game ___RTDynamicCast */
void fn_105639b8(X86 *c); /* Engine ___RTDynamicCast */
void fn_1006f1f7(X86 *c); /* SharedBase ___RTDynamicCast */
void fn_10f4d4c2(X86 *c); /* Script ___RTDynamicCast */
void fn_10ec70a4(X86 *c); /* Importer ___RTDynamicCast */

#define RT_SLOTS 8192u

struct rt_entry {
    _Atomic uint32_t seq;              /* even: stable */
    uint32_t locator, delta, src, tgt; /* key; locator 0 = empty */
    int32_t at;                        /* target from the complete object */
    uint32_t found;
};
static struct rt_entry rt_table[RT_SLOTS];
static int rt_check = -1;
uint64_t g3_rtti_hits, g3_rtti_misses;

static inline int rt_code(uint32_t a) {
    return a >= 0x00400000u && a < 0x12000000u;
}

static void rt_return(X86 *c, uint32_t esp, uint32_t eax) {
    c->r[R_EAX] = eax;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4u;
    recomp_return(c);
}

static void rt_cast(X86 *c, void (*orig)(X86 *)) {
    const uint32_t esp = c->r[R_ESP];
    const uint32_t inptr = rd32(esp + 4u), delta = rd32(esp + 8u), src = rd32(esp + 12u), tgt = rd32(esp + 16u);
    const uint32_t is_ref = rd32(esp + 20u);
    if (!G3OPT(RTTI) || inptr == 0u) {
        orig(c);
        return;
    }
    const uint32_t vf = rd32(inptr);
    const uint32_t loc = rt_code(vf) ? rd32(vf - 4u) : 0u;
    if (!rt_code(loc) || rd32(loc + 8u) != 0u || !rt_code(rd32(loc + 0x10u)) ||
        (rd32(rd32(loc + 0x10u) + 4u) & 2u)) {
        orig(c);
        return;
    }
    const uint32_t complete = inptr - rd32(loc + 4u);
    uint32_t h = (loc * 2654435761u ^ src * 40503u ^ tgt * 2246822519u ^ delta) % RT_SLOTS;
    struct rt_entry *e = &rt_table[h];
    const uint32_t s0 = atomic_load_explicit(&e->seq, memory_order_acquire);
    const int hit = !(s0 & 1u) && e->locator == loc && e->delta == delta && e->src == src && e->tgt == tgt &&
                    (e->found || !is_ref);
    const uint32_t e_found = e->found;
    const int32_t e_at = e->at;
    atomic_thread_fence(memory_order_acquire);
    if (hit && atomic_load_explicit(&e->seq, memory_order_relaxed) == s0) {
        const uint32_t mine = e_found ? complete + (uint32_t)e_at : 0u;
        if (rt_check < 0) {
            const char *v = getenv("G3_RTTI_CHECK");
            rt_check = v && *v == '1';
        }
        if (rt_check) {
            orig(c);
            static int told;
            if (c->r[R_EAX] != mine && told < 50) {
                ++told;
                fprintf(stderr, "[g3] rtti: cast of %08x (locator %08x, delta %u, %08x -> %08x) gave %08x, the table %08x\n",
                        inptr, loc, delta, src, tgt, c->r[R_EAX], mine);
            }
            return;
        }
        ++g3_rtti_hits;
        rt_return(c, esp, mine);
        return;
    }
    ++g3_rtti_misses;
    orig(c);
    const uint32_t got = c->r[R_EAX];
    uint32_t s = atomic_load_explicit(&e->seq, memory_order_relaxed);
    if ((s & 1u) || !atomic_compare_exchange_strong_explicit(&e->seq, &s, s + 1u, memory_order_acquire,
                                                             memory_order_relaxed))
        return;
    atomic_thread_fence(memory_order_release);
    e->locator = loc;
    e->delta = delta;
    e->src = src;
    e->tgt = tgt;
    e->found = got != 0u;
    e->at = (int32_t)(got - complete);
    atomic_store_explicit(&e->seq, s + 2u, memory_order_release);
}

void g3n_10ae9c77(X86 *c) {
    rt_cast(c, fn_10ae9c77);
}
void g3n_004058e9(X86 *c) {
    rt_cast(c, fn_004058e9);
}
void g3n_10e2e179(X86 *c) {
    rt_cast(c, fn_10e2e179);
}
void g3n_1106dd34(X86 *c) {
    rt_cast(c, fn_1106dd34);
}
void g3n_105639b8(X86 *c) {
    rt_cast(c, fn_105639b8);
}
void g3n_1006f1f7(X86 *c) {
    rt_cast(c, fn_1006f1f7);
}
void g3n_10f4d4c2(X86 *c) {
    rt_cast(c, fn_10f4d4c2);
}
void g3n_10ec70a4(X86 *c) {
    rt_cast(c, fn_10ec70a4);
}
