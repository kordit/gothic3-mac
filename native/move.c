/* native/move.c - NPC movement and motion in a fight, outside PhysX.
 *
 * In a fight (work/sample-cr2.txt, main thread 5691 samples) Game's OnProcess 109f2120 holds 484,
 * of which 313 are PhysX itself (character controller moves and ray casts under
 * ProcessControledMovements 109f2390 and FindFloorWaterCeiling 109fb030). Of the rest, the one
 * function with a cost of its own is Engine's FUN_1054a160 (42 samples on the whole thread, 29 of
 * them under OnProcess): starting a motion (PlayMotion 102a76f0 -> 1053eac0 -> 1055a6e0 ->
 * 105499e0 / 1054d2b0) it matches every node of the actor's skeleton to the motion's track with
 * the same id, a search over all tracks for every node. The other non-PhysX self time is spread
 * over dozens of Game, Engine and SharedBase functions at 1 to 5 samples each.
 *
 * g3n_1054a160 runs it natively as the translation does: the same SEH frame and cookie, the same
 * guest calls with the same registers and stack (FUN_1054a3d0, FUN_10295e20, the SharedBase
 * memory admin through Engine's imports, _memset), and the double loop in host registers, its
 * writes in the original's order. Runs under G3OPT(MOVE), otherwise the original. */
#include "chunk.h"
#include "g3_opt.h"

#include <string.h>

void fn_1054a160(X86 *c); /* Engine FUN_1054a160, fastcall (ECX = the motion instance), RET */
void fn_1054a3d0(X86 *c); /* Engine FUN_1054a3d0, the node array's resize */
void fn_10295e20(X86 *c); /* Engine FUN_10295e20, a lazily made singleton */
void fn_1056a710(X86 *c); /* Engine _memset */
void fn_1000c800(X86 *c); /* SharedBase bCMemoryAdmin::GetInstance */
void fn_1000c9c0(X86 *c); /* SharedBase bCMemoryAdmin::Realloc */
void fn_1000c9e0(X86 *c); /* SharedBase bCMemoryAdmin::Free */
#ifndef FN_1054a3d0
#define FN_1054a3d0 fn_1054a3d0
#endif
#ifndef FN_10295e20
#define FN_10295e20 fn_10295e20
#endif
#ifndef FN_1056a710
#define FN_1056a710 fn_1056a710
#endif
#ifndef FN_1000c800
#define FN_1000c800 fn_1000c800
#endif
#ifndef FN_1000c9c0
#define FN_1000c9c0 fn_1000c9c0
#endif
#ifndef FN_1000c9e0
#define FN_1000c9e0 fn_1000c9e0
#endif

#define EN_COOKIE 0x10835674u       /* Engine's __security_cookie */
#define EN_SEH_HANDLER 0x105adefau
#define EN_IAT_GETINSTANCE 0x105f7230u
#define EN_IAT_REALLOC 0x105f71d4u
#define EN_IAT_FREE 0x105f64b0u
#define EN_SINGLETON 0x1086d988u    /* FUN_10295e20's object; +8 the pointer it returns */

enum { AX, CX, DX, BX, SP, BP, SI, DI };

static inline void mv_push(uint32_t *r, uint32_t v) {
    r[SP] -= 4u;
    wr32(r[SP], v);
}
static inline uint32_t mv_pop(uint32_t *r) {
    const uint32_t v = rd32(r[SP]);
    r[SP] += 4u;
    return v;
}

/* CALL target from `ret` with the registers in r; they come back as the callee leaves them. */
static inline void mv_call(X86 *c, uint32_t *r, uint32_t ret, uint32_t target, void (*f)(X86 *)) {
    mv_push(r, ret);
    memcpy(c->r, r, 8 * sizeof r[0]);
    if (recomp_profile_enabled)
        recomp_call(c, target);
    else
        f(c);
    memcpy(r, c->r, 8 * sizeof r[0]);
}

/* CALL 10295e20 from `ret`: in place while the singleton and its pointer exist (CMP [obj],0 /
 * PUSH ESI / PUSH EDI / JNZ / MOV EDI,[obj] / ADD EDI,8 / CMP [EDI],0 / JNZ / MOV EAX,[EDI] /
 * POP EDI / POP ESI / RET), the guest otherwise. */
static inline void mv_singleton(X86 *c, uint32_t *r, uint32_t ret) {
    const uint32_t obj = rd32(EN_SINGLETON);
    if (obj != 0u && rd32(obj + 8u) != 0u) {
        wr32(r[SP] - 4u, ret);
        wr32(r[SP] - 8u, r[SI]);
        wr32(r[SP] - 12u, r[DI]);
        r[AX] = rd32(obj + 8u);
        return;
    }
    mv_call(c, r, ret, 0x10295e20u, FN_10295e20);
}

/* void FUN_1054a160(motion instance = ECX). this+0x44 the actor (+0x1b4 node count, +0x1bc
 * nodes, 400 bytes each, the id at +0x188), this+0x60 the motion (+0x1c track count, +0x24
 * tracks, 0x9c bytes each, the id at +0x10), this+0x64 the per-node array (FUN_1054a3d0 sizes it;
 * +0x6c its 0x3c-byte entries). A byte per track (Realloc'd, zeroed) marks the tracks taken. For
 * each node, the first free track with its id: entry {flags (+0x38, +0x18, +0x58 counts > 0: 1,
 * 2, 4), track index, ten floats from the track}; none: {0, -1}. */
void g3n_1054a160(X86 *c) {
    if (!G3OPT(MOVE) || rd32(EN_IAT_GETINSTANCE) != 0x1000c800u || rd32(EN_IAT_REALLOC) != 0x1000c9c0u ||
        rd32(EN_IAT_FREE) != 0x1000c9e0u) {
        fn_1054a160(c);
        return;
    }
    uint32_t r[8];
    memcpy(r, c->r, sizeof r);
    const uint32_t esp0 = r[SP];
    mv_push(r, 0xffffffffu);
    mv_push(r, EN_SEH_HANDLER);
    r[AX] = rd32(c->fs_base);
    mv_push(r, r[AX]);
    r[SP] -= 0x18u;
    mv_push(r, r[BX]);
    mv_push(r, r[BP]);
    mv_push(r, r[SI]);
    mv_push(r, r[DI]);
    r[AX] = rd32(EN_COOKIE) ^ r[SP];
    mv_push(r, r[AX]);
    const uint32_t t = r[SP]; /* ESP from here on, between the calls */
    r[AX] = t + 0x2cu;
    wr32(c->fs_base, r[AX]);
    r[BX] = r[CX];
    r[AX] = rd32(r[BX] + 0x44u);
    r[SI] = rd32(r[AX] + 0x1b4u);
    r[CX] = rd32(r[BX] + 0x60u);
    r[BP] = rd32(r[CX] + 0x1cu);
    mv_push(r, r[SI]);
    r[DX] = r[BX] + 0x64u;
    mv_push(r, r[DX]);
    mv_call(c, r, 0x1054a1a2u, 0x1054a3d0u, FN_1054a3d0);
    const uint32_t s = r[SP]; /* the callee's RET 8 leaves ESP at t; read it back as the original does */
    r[DI] = 0u;
    wr32(s + 0x20u, 0u);
    wr32(s + 0x24u, 0u);
    wr32(s + 0x28u, 0u);
    wr32(s + 0x34u, 0u);
    wr32(s + 0x20u, r[BP]);
    if ((int32_t)r[BP] > 0) {
        mv_singleton(c, r, 0x1054a1c1u);
        mv_push(r, r[BP]);
        mv_push(r, r[DI]);
        mv_call(c, r, 0x1054a1c9u, 0x1000c800u, FN_1000c800);
        r[CX] = r[AX];
        mv_call(c, r, 0x1054a1d1u, 0x1000c9c0u, FN_1000c9c0);
        wr32(r[SP] + 0x28u, r[AX]);
        wr32(r[SP] + 0x24u, r[BP]);
    }
    r[AX] = rd32(r[SP] + 0x28u);
    mv_push(r, r[BP]);
    mv_push(r, r[DI]);
    mv_push(r, r[AX]);
    mv_call(c, r, 0x1054a1e5u, 0x1056a710u, FN_1056a710);
    r[SP] += 0xcu;
    if ((int32_t)r[SI] > (int32_t)r[DI]) {
        const uint32_t sp = r[SP];
        uint32_t out_off = 0u, node_off = 0u, left = r[SI];
        uint32_t eax = r[AX], ecx = r[CX], edx = r[DX], esi = r[SI], edi = r[DI];
        const uint32_t ebx = r[BX], ebp = r[BP];
        int copied = 0;
        do {
            eax = rd32(ebx + 0x44u);
            esi = rd32(eax + 0x1bcu) + node_off;
            const uint32_t used = rd32(sp + 0x28u); /* the inner loop writes nothing it reads */
            edx = 0u;
            int matched = 0;
            if ((int32_t)ebp > (int32_t)edi) {
                const uint32_t node_id = rd32(esi + 0x188u);
                const uint32_t tracks = rd32(rd32(ebx + 0x60u) + 0x24u);
                do {
                    ecx = used;
                    if (rd8(ecx + edx) != 1u) {
                        eax = tracks + edi;
                        ecx = node_id;
                        if (ecx == rd32(eax + 0x10u)) {
                            matched = 1;
                            break;
                        }
                    }
                    edx += 1u;
                    edi += 0x9cu;
                } while ((int32_t)edx < (int32_t)ebp);
                if (!matched)
                    edi = 0u;
            }
            if (matched) {
                ecx = rd32(ebx + 0x6cu) + out_off;
                wr32(ecx + 4u, edx);
                if ((int32_t)rd32(eax + 0x38u) > 0)
                    wr32(ecx, rd32(ecx) | 1u);
                if ((int32_t)rd32(eax + 0x18u) > 0)
                    wr32(ecx, rd32(ecx) | 2u);
                if ((int32_t)rd32(eax + 0x58u) > 0)
                    wr32(ecx, rd32(ecx) | 4u);
                edi = 0u;
                static const uint8_t from[10] = {0x84, 0x88, 0x8c, 0x74, 0x78, 0x7c, 0x80, 0x90, 0x94, 0x98};
                static const uint8_t to[10] = {0x24, 0x28, 0x2c, 0x14, 0x18, 0x1c, 0x20, 0x30, 0x34, 0x38};
                for (int k = 0; k < 10; ++k)
                    wrf32(ecx + to[k], fto_float(c, (double)rdf32(eax + from[k])));
                copied = 1;
                ecx = used;
                wr8(ecx + edx, 1u);
            } else {
                eax = rd32(ebx + 0x6cu) + out_off;
                wr32(eax + 4u, 0xffffffffu);
                wr32(eax, edi);
            }
            node_off += 0x190u;
            out_off += 0x3cu;
            left -= 1u;
        } while (left != 0u);
        /* the loop's locals, as the original leaves them in its frame */
        wr32(sp + 0x14u, out_off);
        wr32(sp + 0x18u, node_off);
        wr32(sp + 0x1cu, 0u);
        if (copied) { /* FLD / FSTP: the slot under the entry top used and emptied */
            const unsigned p = (c->fpu_top - 1u) & 7u;
            ftag_put(c, p, FTAG_EMPTY);
            c->st_exact[p] = 0;
        }
        r[AX] = eax;
        r[CX] = ecx;
        r[DX] = edx;
        r[SI] = esi;
        r[DI] = edi;
    }
    wr32(r[SP] + 0x34u, 0xffffffffu);
    mv_singleton(c, r, 0x1054a270u);
    r[DX] = rd32(r[SP] + 0x28u);
    mv_push(r, r[DX]);
    mv_call(c, r, 0x1054a27bu, 0x1000c800u, FN_1000c800);
    r[CX] = r[AX];
    mv_call(c, r, 0x1054a283u, 0x1000c9e0u, FN_1000c9e0);
    r[CX] = rd32(r[SP] + 0x2cu);
    wr32(c->fs_base, r[CX]);
    r[CX] = mv_pop(r);
    r[DI] = mv_pop(r);
    r[SI] = mv_pop(r);
    r[BP] = mv_pop(r);
    r[BX] = mv_pop(r);
    r[SP] += 0x24u;
    (void)t;
    (void)esp0;
    memcpy(c->r, r, sizeof r);
    c->eip = rd32(c->r[SP]);
    c->r[SP] += 4u;
    recomp_return(c);
}
