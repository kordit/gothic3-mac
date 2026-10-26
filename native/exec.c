/* native/exec.c - pieces of the render list's execution (eCFrustumDatabase::Execute 10400530 and
 * what it draws), natively. Group `exec` (G3OPT_EXEC).
 *
 * docs/perf/exec.md has the profile. The list's cost is spread over a hundred functions; this file
 * takes the ones that are self-contained and called many times per frame:
 *
 *   103af9c0  the particles' depth comparator (DepthSortParticles 103afa70 sorts every visible
 *             emitter's particles back to front with Engine's qsort every frame) was here as a
 *             comparison at a time; it is native/pdo.c's now (one key per element for the whole
 *             sort, the same import check), which shadow.c's qsort runs under G3OPT(EXEC) or
 *             G3OPT(PDO). exec_test still tests the particle sort.
 *
 * Exactness as in vector.c and cull.c: the original's x87 arithmetic step for step with the
 * kit's helpers (fx87 per FSUB/FMUL/FADD, fto_float per FSTP to a float, IE from FCOMI on any
 * NaN), the x87 stack left as the original leaves it (the slots it pushed into tagged empty),
 * EAX, ECX, EDX and XMM0 as its last writes leave them. Stack bytes below the final ESP (its
 * locals, the argument slots it spills into) are dead once it returns and are not written.
 * The SharedBase calls go through Engine's import table; the native checks that the slots hold
 * the exports it computes in place, and otherwise the guest call is made. */
#include "chunk.h"
#include "g3_opt.h"

#include <string.h>

/* ---- FUN_10494fc0: a shader's parameters, per draw ----------------------------------------- */

static inline void ex_push(X86 *c, uint32_t v) {
    c->r[4] -= 4u;
    wr32(c->r[4], v);
}

static inline uint32_t ex_pop(X86 *c) {
    const uint32_t v = rd32(c->r[4]);
    c->r[4] += 4u;
    return v;
}

/* CALL target with the continuation `ret`, through the call site's inline cache. */
static inline void ex_icall(X86 *c, uint32_t target, uint32_t ret, uint64_t *ic) {
    ex_push(c, ret);
    recomp_call_cached(c, target, ic);
}

/* One of the three constant ranges, {lo, hi} at ESI+lo_off, flushed when lo < hi through the
 * device wrapper's vtable slot `slot`: (lo, ESI + base + lo * 16 or * 4, hi - lo), thiscall on
 * [[ESI+8]+0xc]. */
static inline void ex_flush(X86 *c, uint32_t lo_off, uint32_t base, int shift4, uint32_t slot, uint32_t ret,
                            uint64_t *ic) {
    uint32_t *const r = c->r;
    r[0] = rd32(r[6] + lo_off);
    r[2] = rd32(r[6] + lo_off + 4u);
    if (r[0] >= r[2])
        return;
    r[1] = rd32(rd32(r[6] + 8u) + 0xcu);
    r[7] = rd32(r[1]);
    r[2] -= r[0];
    ex_push(c, r[2]);
    r[2] = shift4 ? (r[0] << 4) + r[6] + base : r[6] + r[0] * 4u + base;
    ex_push(c, r[2]);
    ex_push(c, r[0]);
    r[0] = rd32(r[7] + slot);
    ex_icall(c, r[0], ret, ic);
}

/* bool FUN_10494fc0(list = ECX), ESI = the shader state: {+4 the device state (+0x7bc the state
 * bound per stage), +8 the renderer (+0xc the device wrapper), +0xc the stage, +0x14/+0x18 the
 * parameter entries and their count, +0x20..+0x34 three constant ranges, +0x38 / +0x1038 /
 * +0x1138 their shadow copies}. Binds the state to its stage when it is not the bound one,
 * resets the ranges, lets each component of the list (0x38-byte entries, vtable +8) and each
 * parameter entry (vtable +4; a newly bound state first clears the entry's cached value at
 * +0x34) write its constants, then flushes the three ranges through the wrapper (vtable
 * +0x18c/+0x190/+0x194, or +0x1b4/+0x1b8/+0x1bc for a stage other than 0). AL = 1.
 *
 * The original's instructions minus EFLAGS: the same pushes, registers and continuations at
 * every guest call, the counts and pointers read back from the registers the callee left
 * (they are callee-saved), the ranges read from memory after the components wrote them.
 * scene.c's g3n_10494fc0 calls this under G3OPT(EXEC). */
void g3x_10494fc0(X86 *c) {
    static uint64_t ic_comp, ic_par1, ic_par0, ic_a1, ic_a2, ic_a3, ic_b1, ic_b2, ic_b3;
    uint32_t *const r = c->r;
    r[0] = rd32(r[6] + 0xcu);
    r[2] = rd32(r[6] + 4u);
    const uint32_t bound = rd32(r[2] + r[0] * 4u + 0x7bcu);
    r[0] = r[2] + r[0] * 4u + 0x7bcu;
    ex_push(c, r[3]);
    r[3] = (r[3] & 0xffffff00u) | (bound != r[6] ? 1u : 0u);
    ex_push(c, r[5]);
    ex_push(c, r[7]);
    if ((uint8_t)r[3] == 1u)
        wr32(r[0], r[6]);
    r[2] = 0u;
    r[0] = 0x10u;
    wr32(r[6] + 0x20u, 0x100u);
    wr32(r[6] + 0x24u, 0u);
    wr32(r[6] + 0x28u, 0x10u);
    wr32(r[6] + 0x2cu, 0u);
    wr32(r[6] + 0x30u, 0x10u);
    wr32(r[6] + 0x34u, 0u);
    r[0] = rd32(r[1] + 4u);
    r[1] = rd32(r[1]);
    if ((int32_t)r[0] > 0) {
        r[7] = r[1];
        r[5] = r[0];
        do {
            r[1] = rd32(r[7]);
            r[0] = rd32(rd32(r[1]) + 8u);
            r[2] = r[7] + 4u;
            ex_push(c, r[2]);
            ex_push(c, r[6]);
            ex_icall(c, r[0], 0x1049501eu, &ic_comp);
            r[7] += 0x38u;
            r[5] -= 1u;
        } while (r[5] != 0u);
    }
    r[7] = rd32(r[6] + 0x14u);
    const int fresh = (uint8_t)r[3] == 1u;
    r[3] = rd32(r[6] + 0x18u);
    if (fresh) {
        if ((int32_t)r[3] > 0) {
            r[7] += 0x34u;
            do {
                r[1] = rd32(r[7] - 0x34u);
                ex_push(c, r[7]);
                r[0] = r[7] - 0x30u;
                wr32(r[7], 0xffffffffu);
                r[2] = rd32(rd32(r[1]) + 4u);
                ex_push(c, r[0]);
                ex_push(c, r[6]);
                ex_icall(c, r[2], 0x1049504eu, &ic_par1);
                r[7] += 0x38u;
                r[3] -= 1u;
            } while (r[3] != 0u);
        }
    } else if ((int32_t)r[3] > 0) {
        do {
            r[1] = rd32(r[7]);
            r[2] = rd32(r[1]);
            r[0] = r[7] + 4u;
            r[5] = r[0] + 0x30u;
            ex_push(c, r[5]);
            ex_push(c, r[0]);
            r[0] = rd32(r[2] + 4u);
            ex_push(c, r[6]);
            ex_icall(c, r[0], 0x10495072u, &ic_par0);
            r[7] += 0x38u;
            r[3] -= 1u;
        } while (r[3] != 0u);
    }
    if (rd32(r[6] + 0xcu) == 0u) {
        ex_flush(c, 0x20u, 0x38u, 1, 0x18cu, 0x104950a8u, &ic_a1);
        ex_flush(c, 0x28u, 0x1038u, 1, 0x190u, 0x104950d3u, &ic_a2);
        ex_flush(c, 0x30u, 0x1138u, 0, 0x194u, 0x104950fdu, &ic_a3);
    } else {
        ex_flush(c, 0x20u, 0x38u, 1, 0x1b4u, 0x10495125u, &ic_b1);
        ex_flush(c, 0x28u, 0x1038u, 1, 0x1b8u, 0x10495150u, &ic_b2);
        ex_flush(c, 0x30u, 0x1138u, 0, 0x1bcu, 0x10495176u, &ic_b3);
    }
    r[7] = ex_pop(c);
    r[5] = ex_pop(c);
    r[0] = (r[0] & 0xffffff00u) | 1u;
    r[3] = ex_pop(c);
    c->eip = ex_pop(c);
    recomp_return(c);
}
