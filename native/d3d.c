/* native/d3d.c - stage 5: the D3D9 draw path (vertex declarations, redundant state).
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h) and runs its
 * native version only under G3OPT(D3D); otherwise it calls the original. A native version must
 * end as the translated function does (see its RET in build/recomp/gen/aux-engine/).
 *
 * eCDX9Primitive::Execute runs once per draw (about 1400 a frame in Ardea, docs/perf/d3d-path.md).
 * It sets the four
 * vertex streams, cuts the elements of an absent stream or of an offset past its stride out of
 * the declaration it copied with GetDeclaration, sets the index buffer and FVF, then either the
 * declaration as it is or, when it cut something (or added the white COLOR0 stream for a
 * declaration with COLOR1 alone), the engine's cached copy of the cut declaration
 * (CreateVertexDeclaration, SetVertexDeclaration, DestroyVertexDeclaration on the eCDX9Device
 * wrapper), and draws. Every device call is a COM call into the kit (CALL [reg+off] -> the kit's
 * import shim through the call site's inline cache); the work in Engine around them is the three
 * element loops and the bookkeeping, which is what the native version does without the
 * translation's flag and register traffic.
 *
 * The rule for all three: the original's instructions with their registers, pushes, stack slots
 * and guest calls (same order, same arguments, same return addresses), the values after a call
 * read back from memory and from ESP as the callee left it. EFLAGS are written where the
 * translation writes them and a call or the return can still see them: before every guest call
 * and at the RET they hold what the translation holds. A flag the translation computes and then
 * overwrites before any call (the loop compares) is not computed. The error paths (a failing
 * HRESULT: the DXGetErrorString / bCString / log calls) are the original's instructions one for
 * one, generated from the listing, cold.
 *
 * The engine's waste, measured in the headless profile (work/hostprof-h3-1.txt): the
 * declaration lookup CreateVertexDeclaration runs on every draw whose declaration was cut, and
 * always hits (the kit's Dev_CreateVertexDeclaration shows only under d3dx9, never under it);
 * 0.78% of the samples are its linear search in translated code, against 0.02% for the AddRef
 * the hit makes. The native lookup does the same search (first entry with the same element count and
 * the same bytes) with memcmp and makes the same AddRef; a miss runs the original, which then
 * searches again and creates. The AddRef/Release pair around SetVertexDeclaration and the
 * second SetFVF(0) stay: they are guest calls into the kit and their order is guest-visible.
 *
 * native/tests/d3d_test.c runs each against the translation. */
#include "chunk.h"
#include "g3_opt.h"

#include <stdlib.h>
#include <string.h>

void fn_10311240(X86 *c); /* Engine eCDX9Primitive::Execute */
void fn_10304020(X86 *c); /* Engine SetVertexDeclaration */
void fn_10303bd0(X86 *c); /* Engine CreateVertexDeclaration */

/* The direct calls the three make. */
void fn_10564220(X86 *c); /* _memmove (cdecl) */
void fn_1056a710(X86 *c); /* _memset (cdecl) */
void fn_1059225a(X86 *c); /* DXGetErrorString9A (stdcall, 1) */
void fn_105a40c6(X86 *c); /* DXGetErrorDescription9A (stdcall, 1) */
#ifndef FN_10564220
#define FN_10564220 fn_10564220
#endif
#ifndef FN_1056a710
#define FN_1056a710 fn_1056a710
#endif
#ifndef FN_1059225a
#define FN_1059225a fn_1059225a
#endif
#ifndef FN_105a40c6
#define FN_105a40c6 fn_105a40c6
#endif

static inline void d3_push(X86 *c, uint32_t v) {
    c->r[4] -= 4u;
    wr32(c->r[4], v);
}

static inline uint32_t d3_pop(X86 *c) {
    const uint32_t v = rd32(c->r[4]);
    c->r[4] += 4u;
    return v;
}

/* CALL with the continuation `ret` on the guest stack. An indirect CALL (register or memory
 * operand) goes through its own inline cache, as the translation's call site does; the target
 * is read before the push. A direct CALL takes the translation's CALL_FN path (the profiler's
 * recomp_call included; mod hooks are not consulted, as in vector.c and scene.c). */
#define D3_CALLI(target, ret)                                                                      \
    do {                                                                                           \
        static uint64_t ic_;                                                                       \
        const uint32_t t_ = (target);                                                              \
        d3_push(c, (ret));                                                                         \
        recomp_call_cached(c, t_, &ic_);                                                           \
    } while (0)
#define D3_CALLD(addr, fn, ret)                                                                    \
    do {                                                                                           \
        d3_push(c, (ret));                                                                         \
        if (recomp_profile_enabled)                                                                \
            recomp_call(c, (addr));                                                                \
        else                                                                                       \
            fn(c);                                                                                 \
    } while (0)

/* The flags as the translation writes them. TEST/AND/OR/XOR: CF OF ZF SF PF (AF untouched).
 * CMP/SUB and ADD: all six, at the operand width. */
static inline void fl_logic(X86 *c, uint32_t r, unsigned bits) {
    c->eflags_cf = 0;
    c->eflags_of = 0;
    c->eflags_zf = r == 0;
    c->eflags_sf = (r >> (bits - 1u)) & 1u;
    c->eflags_pf = parity8(r);
}
static inline void fl_sub(X86 *c, uint32_t a, uint32_t b, unsigned bits) {
    const uint32_t m = bits == 32u ? 0xffffffffu : (1u << bits) - 1u;
    const uint32_t r = (a - b) & m;
    c->eflags_cf = (a & m) < (b & m);
    c->eflags_of = (((a ^ b) & (a ^ r)) >> (bits - 1u)) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = r == 0;
    c->eflags_sf = (r >> (bits - 1u)) & 1u;
    c->eflags_pf = parity8(r);
}
static inline void fl_add32(X86 *c, uint32_t a, uint32_t b) {
    const uint32_t r = a + b;
    c->eflags_cf = r < a;
    c->eflags_of = ((~(a ^ b) & (a ^ r)) >> 31) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = r == 0;
    c->eflags_sf = r >> 31;
    c->eflags_pf = parity8(r);
}

/* ---- CreateVertexDeclaration ------------------------------------------------------------ */

/* bool eCDX9Device::CreateVertexDeclaration(D3DVERTEXELEMENT9 const *elems,
 * IDirect3DVertexDeclaration9 **out), thiscall, RET 8. this+8 = the IDirect3DDevice9 (null:
 * false), this+0x54 = the cache, 12-byte entries {decl, element count, elements}, this+0x58 its
 * count. The element count is that of `elems` without its D3DDECL_END (stream 0xff). The first
 * entry with that count and the same 8*count bytes is the hit: decl->AddRef (vtable +4),
 * *out = decl, true. A miss creates the declaration, copies the elements into a new block and
 * appends the entry (the original does that, from its entry state: the search has no side
 * effects).
 *
 * The hit as the original leaves it: locals [ESP0-0x1c] = the entry index, -0x18 this, -0x14
 * the count of entries, -0x10 the cache; EBP EDI EBX ESI pushed; at the AddRef EAX = vtable,
 * ECX = AddRef, EDX = the cache, EBX = &entry.elements, EBP = elems, ESI = decl, EDI = count,
 * and the flags of the compare loop's end (TEST EAX,EAX with EAX = 0 after CMP EAX,4: AF set). */
void g3n_10303bd0(X86 *c) {
    if (!G3OPT(D3D)) {
        fn_10303bd0(c);
        return;
    }
    uint32_t *const r = c->r;
    const uint32_t esp0 = r[4], self = r[1];
    if (rd32(self + 8u) == 0u) {
        wr32(esp0 - 0x18u, self);
        r[0] = 0;
        fl_add32(c, esp0 - 0x1cu, 0x1cu);
        c->eip = rd32(esp0);
        r[4] = esp0 + 12u;
        recomp_return(c);
        return;
    }
    const uint32_t elems = rd32(esp0 + 4u);
    uint32_t n = 0;
    while (rd16(elems + 8u * n) != 0xffu)
        ++n;
    const uint32_t count = rd32(self + 0x58u);
    uint32_t list = 0, i = 0;
    int hit = 0;
    if (count != 0u) {
        list = rd32(self + 0x54u);
        const uint8_t *const want = g_mem + elems;
        for (; i < count; ++i) {
            const uint32_t e = list + 12u * i;
            if (rd32(e + 4u) == n && memcmp(want, g_mem + rd32(e + 8u), 8u * (size_t)n) == 0) {
                hit = 1;
                break;
            }
        }
    }
    if (!hit) {
        fn_10303bd0(c);
        return;
    }
    wr32(esp0 - 0x18u, self);
    wr32(esp0 - 0x20u, r[5]);
    wr32(esp0 - 0x24u, r[7]);
    wr32(esp0 - 0x28u, r[3]);
    wr32(esp0 - 0x2cu, r[6]);
    wr32(esp0 - 0x1cu, i);
    wr32(esp0 - 0x14u, count);
    wr32(esp0 - 0x10u, list);
    r[4] = esp0 - 0x2cu;
    r[5] = elems;
    r[7] = n;
    r[3] = list + 12u * i + 8u;
    r[2] = list;
    r[6] = rd32(list + 12u * i);
    r[0] = rd32(r[6]);
    r[1] = rd32(r[0] + 4u);
    fl_sub(c, 0u, 4u, 32u); /* CMP EAX,4 */
    fl_logic(c, 0u, 32u);   /* TEST EAX,EAX */
    d3_push(c, r[6]);
    D3_CALLI(r[1], 0x10303d97u);
    r[2] = rd32(r[4] + 0x34u);
    wr32(r[2], r[6]);
    r[6] = d3_pop(c);
    r[3] = d3_pop(c);
    r[7] = d3_pop(c);
    r[0] = 1u;
    r[5] = d3_pop(c);
    fl_add32(c, r[4], 0x1cu);
    r[4] += 0x1cu;
    c->eip = rd32(r[4]);
    r[4] += 12u;
    recomp_return(c);
}

/* ---- SetVertexDeclaration --------------------------------------------------------------- */

/* The original's error blocks: a failing HRESULT in ESI, logged with its name and description. */
static __attribute__((noinline, cold)) void svd_fail(X86 *c, uint32_t at) {
    uint32_t *const r = c->r;
    switch (at) {
    case 0x10304052u: goto L_10304052;
    case 0x10304122u: goto L_10304122;
    default: abort();
    }
L_10304052:
    d3_push(c, r[6]); /* 10304052 PUSH ESI */
    D3_CALLD(0x1059225au, FN_1059225a, 0x10304058u); /* 10304053 CALL 0x1059225a */
    r[7] = rd32(0x105f72c0u); /* 10304058 MOV EDI,dword ptr [0x105f72c0] */
    d3_push(c, r[0]); /* 1030405e PUSH EAX */
    r[1] = r[4] + 0x14u; /* 1030405f LEA ECX,[ESP + 0x14] */
    D3_CALLI(r[7], 0x10304065u); /* 10304063 CALL EDI */
    r[0] = rd32(0x105f7290u); /* 10304065 MOV EAX,[0x105f7290] */
    r[1] = rd32(r[0]); /* 1030406a MOV ECX,dword ptr [EAX] */
    r[3] = rd32(0x105f6670u); /* 1030406c MOV EBX,dword ptr [0x105f6670] */
    d3_push(c, r[1]); /* 10304072 PUSH ECX */
    d3_push(c, 0xffffffffu); /* 10304073 PUSH -0x1 */
    d3_push(c, 0x0u); /* 10304075 PUSH 0x0 */
    d3_push(c, 0x0u); /* 10304077 PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 10304079 PUSH 0x1068c5b0 */
    r[2] = r[4] + 0x28u; /* 1030407e LEA EDX,[ESP + 0x28] */
    d3_push(c, r[2]); /* 10304082 PUSH EDX */
    r[0] = r[4] + 0x28u; /* 10304083 LEA EAX,[ESP + 0x28] */
    d3_push(c, r[0]); /* 10304087 PUSH EAX */
    d3_push(c, 0x1069fc68u); /* 10304088 PUSH 0x1069fc68 */
    r[1] = r[4] + 0x38u; /* 1030408d LEA ECX,[ESP + 0x38] */
    d3_push(c, r[1]); /* 10304091 PUSH ECX */
    D3_CALLI(r[3], 0x10304094u); /* 10304092 CALL EBX */
    r[5] = rd32(0x105f71f0u); /* 10304094 MOV EBP,dword ptr [0x105f71f0] */
    r[1] = r[0]; /* 1030409a MOV ECX,EAX */
    D3_CALLI(r[5], 0x1030409eu); /* 1030409c CALL EBP */
    r[1] = r[0]; /* 1030409e MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x103040a6u); /* 103040a0 CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 103040a6 PUSH EAX */
    d3_push(c, 0x3u); /* 103040a7 PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x103040afu); /* 103040a9 CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 103040af MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x103040b7u); /* 103040b1 CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x14u; /* 103040b7 LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x103040c1u); /* 103040bb CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x18u; /* 103040c1 LEA ECX,[ESP + 0x18] */
    D3_CALLI(rd32(0x105f722cu), 0x103040cbu); /* 103040c5 CALL dword ptr [0x105f722c] */
    d3_push(c, r[6]); /* 103040cb PUSH ESI */
    D3_CALLD(0x105a40c6u, FN_105a40c6, 0x103040d1u); /* 103040cc CALL 0x105a40c6 */
    d3_push(c, r[0]); /* 103040d1 PUSH EAX */
    r[1] = r[4] + 0x24u; /* 103040d2 LEA ECX,[ESP + 0x24] */
    D3_CALLI(r[7], 0x103040d8u); /* 103040d6 CALL EDI */
    r[2] = rd32(0x105f7290u); /* 103040d8 MOV EDX,dword ptr [0x105f7290] */
    r[0] = rd32(r[2]); /* 103040de MOV EAX,dword ptr [EDX] */
    d3_push(c, r[0]); /* 103040e0 PUSH EAX */
    d3_push(c, 0xffffffffu); /* 103040e1 PUSH -0x1 */
    d3_push(c, 0x0u); /* 103040e3 PUSH 0x0 */
    d3_push(c, 0x0u); /* 103040e5 PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 103040e7 PUSH 0x1068c5b0 */
    r[1] = r[4] + 0x2cu; /* 103040ec LEA ECX,[ESP + 0x2c] */
    d3_push(c, r[1]); /* 103040f0 PUSH ECX */
    r[2] = r[4] + 0x38u; /* 103040f1 LEA EDX,[ESP + 0x38] */
    d3_push(c, r[2]); /* 103040f5 PUSH EDX */
    d3_push(c, 0x1069e280u); /* 103040f6 PUSH 0x1069e280 */
    r[0] = r[4] + 0x34u; /* 103040fb LEA EAX,[ESP + 0x34] */
    d3_push(c, r[0]); /* 103040ff PUSH EAX */
    goto L_103041d0; /* 10304100 JMP 0x103041d0 */
L_10304122:
    d3_push(c, r[6]); /* 10304122 PUSH ESI */
    D3_CALLD(0x1059225au, FN_1059225a, 0x10304128u); /* 10304123 CALL 0x1059225a */
    r[7] = rd32(0x105f72c0u); /* 10304128 MOV EDI,dword ptr [0x105f72c0] */
    d3_push(c, r[0]); /* 1030412e PUSH EAX */
    r[1] = r[4] + 0x14u; /* 1030412f LEA ECX,[ESP + 0x14] */
    D3_CALLI(r[7], 0x10304135u); /* 10304133 CALL EDI */
    r[1] = rd32(0x105f7290u); /* 10304135 MOV ECX,dword ptr [0x105f7290] */
    r[2] = rd32(r[1]); /* 1030413b MOV EDX,dword ptr [ECX] */
    r[3] = rd32(0x105f6670u); /* 1030413d MOV EBX,dword ptr [0x105f6670] */
    d3_push(c, r[2]); /* 10304143 PUSH EDX */
    d3_push(c, 0xffffffffu); /* 10304144 PUSH -0x1 */
    d3_push(c, 0x0u); /* 10304146 PUSH 0x0 */
    d3_push(c, 0x0u); /* 10304148 PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 1030414a PUSH 0x1068c5b0 */
    r[0] = r[4] + 0x2cu; /* 1030414f LEA EAX,[ESP + 0x2c] */
    d3_push(c, r[0]); /* 10304153 PUSH EAX */
    r[1] = r[4] + 0x28u; /* 10304154 LEA ECX,[ESP + 0x28] */
    d3_push(c, r[1]); /* 10304158 PUSH ECX */
    d3_push(c, 0x1069fcb0u); /* 10304159 PUSH 0x1069fcb0 */
    r[2] = r[4] + 0x34u; /* 1030415e LEA EDX,[ESP + 0x34] */
    d3_push(c, r[2]); /* 10304162 PUSH EDX */
    D3_CALLI(r[3], 0x10304165u); /* 10304163 CALL EBX */
    r[5] = rd32(0x105f71f0u); /* 10304165 MOV EBP,dword ptr [0x105f71f0] */
    r[1] = r[0]; /* 1030416b MOV ECX,EAX */
    D3_CALLI(r[5], 0x1030416fu); /* 1030416d CALL EBP */
    r[1] = r[0]; /* 1030416f MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x10304177u); /* 10304171 CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 10304177 PUSH EAX */
    d3_push(c, 0x3u); /* 10304178 PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x10304180u); /* 1030417a CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 10304180 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x10304188u); /* 10304182 CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x18u; /* 10304188 LEA ECX,[ESP + 0x18] */
    D3_CALLI(rd32(0x105f722cu), 0x10304192u); /* 1030418c CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x14u; /* 10304192 LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x1030419cu); /* 10304196 CALL dword ptr [0x105f722c] */
    d3_push(c, r[6]); /* 1030419c PUSH ESI */
    D3_CALLD(0x105a40c6u, FN_105a40c6, 0x103041a2u); /* 1030419d CALL 0x105a40c6 */
    d3_push(c, r[0]); /* 103041a2 PUSH EAX */
    r[1] = r[4] + 0x24u; /* 103041a3 LEA ECX,[ESP + 0x24] */
    D3_CALLI(r[7], 0x103041a9u); /* 103041a7 CALL EDI */
    r[0] = rd32(0x105f7290u); /* 103041a9 MOV EAX,[0x105f7290] */
    r[1] = rd32(r[0]); /* 103041ae MOV ECX,dword ptr [EAX] */
    d3_push(c, r[1]); /* 103041b0 PUSH ECX */
    d3_push(c, 0xffffffffu); /* 103041b1 PUSH -0x1 */
    d3_push(c, 0x0u); /* 103041b3 PUSH 0x0 */
    d3_push(c, 0x0u); /* 103041b5 PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 103041b7 PUSH 0x1068c5b0 */
    r[2] = r[4] + 0x2cu; /* 103041bc LEA EDX,[ESP + 0x2c] */
    d3_push(c, r[2]); /* 103041c0 PUSH EDX */
    r[0] = r[4] + 0x38u; /* 103041c1 LEA EAX,[ESP + 0x38] */
    d3_push(c, r[0]); /* 103041c5 PUSH EAX */
    d3_push(c, 0x1069e280u); /* 103041c6 PUSH 0x1069e280 */
    r[1] = r[4] + 0x34u; /* 103041cb LEA ECX,[ESP + 0x34] */
    d3_push(c, r[1]); /* 103041cf PUSH ECX */
L_103041d0:
    D3_CALLI(r[3], 0x103041d2u); /* 103041d0 CALL EBX */
    r[1] = r[0]; /* 103041d2 MOV ECX,EAX */
    D3_CALLI(r[5], 0x103041d6u); /* 103041d4 CALL EBP */
    r[1] = r[0]; /* 103041d6 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x103041deu); /* 103041d8 CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 103041de PUSH EAX */
    d3_push(c, 0x3u); /* 103041df PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x103041e7u); /* 103041e1 CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 103041e7 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x103041efu); /* 103041e9 CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x18u; /* 103041ef LEA ECX,[ESP + 0x18] */
    D3_CALLI(rd32(0x105f722cu), 0x103041f9u); /* 103041f3 CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x14u; /* 103041f9 LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x10304203u); /* 103041fd CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x20u; /* 10304203 LEA ECX,[ESP + 0x20] */
    D3_CALLI(rd32(0x105f722cu), 0x1030420du); /* 10304207 CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x10u; /* 1030420d LEA ECX,[ESP + 0x10] */
    D3_CALLI(rd32(0x105f722cu), 0x10304217u); /* 10304211 CALL dword ptr [0x105f722c] */
    goto L_10304217;
L_10304217:
    r[6] = d3_pop(c);
    r[5] = d3_pop(c);
    r[3] = d3_pop(c);
    r[0] = 0;
    r[7] = d3_pop(c);
    fl_add32(c, r[4], 0xcu);
    r[4] += 0xcu;
    c->eip = rd32(r[4]);
    r[4] += 8u;
    recomp_return(c);
}

/* bool eCDX9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *), thiscall, RET 4:
 * SetFVF(0) (device vtable +0x164) then SetVertexDeclaration (+0x15c) on this+8, true when both
 * succeed; null device false. */
void g3n_10304020(X86 *c) {
    if (!G3OPT(D3D)) {
        fn_10304020(c);
        return;
    }
    uint32_t *const r = c->r;
    r[4] -= 0xcu;
    d3_push(c, r[7]);
    r[7] = r[1];
    const uint32_t dev = rd32(r[7] + 8u);
    if (dev == 0u) {
        r[0] = 0;
        r[7] = d3_pop(c);
        fl_add32(c, r[4], 0xcu);
        r[4] += 0xcu;
        c->eip = rd32(r[4]);
        r[4] += 8u;
        recomp_return(c);
        return;
    }
    fl_sub(c, dev, 0u, 32u);
    r[0] = dev;
    r[1] = rd32(r[0]);
    r[2] = rd32(r[1] + 0x164u);
    d3_push(c, r[3]);
    d3_push(c, r[5]);
    d3_push(c, r[6]);
    d3_push(c, 0u);
    d3_push(c, r[0]);
    D3_CALLI(r[2], 0x10304048u);
    r[6] = r[0];
    fl_logic(c, r[6], 32u);
    if ((int32_t)r[6] < 0) {
        svd_fail(c, 0x10304052u);
        return;
    }
    r[0] = rd32(r[7] + 8u);
    r[2] = rd32(r[4] + 0x20u);
    r[1] = rd32(r[0]);
    d3_push(c, r[2]);
    d3_push(c, r[0]);
    r[0] = rd32(r[1] + 0x15cu);
    D3_CALLI(r[0], 0x10304118u);
    r[6] = r[0];
    if ((int32_t)r[6] < 0) {
        fl_logic(c, r[6], 32u);
        svd_fail(c, 0x10304122u);
        return;
    }
    r[6] = d3_pop(c);
    r[5] = d3_pop(c);
    r[3] = d3_pop(c);
    r[0] = 1u;
    r[7] = d3_pop(c);
    fl_add32(c, r[4], 0xcu);
    r[4] += 0xcu;
    c->eip = rd32(r[4]);
    r[4] += 8u;
    recomp_return(c);
}

/* ---- eCDX9Primitive::Execute ------------------------------------------------------------ */

/* The common end: XOR AL,AL (or MOV AL,1 after the draw counter) and the epilogue. */
static inline void ex_out(X86 *c, int ok) {
    uint32_t *const r = c->r;
    if (ok) {
        const uint32_t a = rd32(0x10862d28u);
        wr32(0x10862d28u, a + 1u);
        fl_add32(c, a, 1u);
        r[0] = (r[0] & 0xffffff00u) | 1u;
    } else {
        r[0] &= 0xffffff00u;
        fl_logic(c, 0u, 8u);
    }
    r[7] = d3_pop(c);
    r[6] = d3_pop(c);
    r[5] = d3_pop(c);
    r[3] = d3_pop(c);
    r[4] = r[5];
    r[5] = d3_pop(c);
    c->eip = rd32(r[4]);
    r[4] += 4u;
    recomp_return(c);
}

/* The error blocks from the listing (the failing HRESULT in EDI, EBP or ESI, the message by
 * address), shared tails and all; they end in the false return. */
static __attribute__((noinline, cold)) void ex_fail(X86 *c, uint32_t at) {
    uint32_t *const r = c->r;
    switch (at) {
    case 0x10311288u: goto L_10311288;
    case 0x103113f5u: goto L_103113f5;
    case 0x1031150eu: goto L_1031150e;
    case 0x10311587u: goto L_10311587;
    case 0x10311640u: goto L_10311640;
    case 0x1031171au: goto L_1031171a;
    case 0x10311b05u: goto L_10311b05;
    case 0x10311badu: goto L_10311bad;
    case 0x10311c73u: goto L_10311c73;
    default: abort();
    }
L_10311288:
    d3_push(c, r[7]); /* 10311288 PUSH EDI */
    D3_CALLD(0x1059225au, FN_1059225a, 0x1031128eu); /* 10311289 CALL 0x1059225a */
    r[6] = rd32(0x105f72c0u); /* 1031128e MOV ESI,dword ptr [0x105f72c0] */
    d3_push(c, r[0]); /* 10311294 PUSH EAX */
    r[1] = r[4] + 0x24u; /* 10311295 LEA ECX,[ESP + 0x24] */
    D3_CALLI(r[6], 0x1031129bu); /* 10311299 CALL ESI */
    r[1] = rd32(0x105f7290u); /* 1031129b MOV ECX,dword ptr [0x105f7290] */
    r[2] = rd32(r[1]); /* 103112a1 MOV EDX,dword ptr [ECX] */
    r[3] = rd32(0x105f6670u); /* 103112a3 MOV EBX,dword ptr [0x105f6670] */
    d3_push(c, r[2]); /* 103112a9 PUSH EDX */
    d3_push(c, 0xffffffffu); /* 103112aa PUSH -0x1 */
    d3_push(c, 0x0u); /* 103112ac PUSH 0x0 */
    d3_push(c, 0x0u); /* 103112ae PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 103112b0 PUSH 0x1068c5b0 */
    r[0] = r[4] + 0x2cu; /* 103112b5 LEA EAX,[ESP + 0x2c] */
    d3_push(c, r[0]); /* 103112b9 PUSH EAX */
    r[1] = r[4] + 0x38u; /* 103112ba LEA ECX,[ESP + 0x38] */
    d3_push(c, r[1]); /* 103112be PUSH ECX */
    d3_push(c, 0x106a1d68u); /* 103112bf PUSH 0x106a1d68 */
    r[2] = r[4] + 0x34u; /* 103112c4 LEA EDX,[ESP + 0x34] */
    d3_push(c, r[2]); /* 103112c8 PUSH EDX */
    D3_CALLI(r[3], 0x103112cbu); /* 103112c9 CALL EBX */
    r[5] = rd32(0x105f71f0u); /* 103112cb MOV EBP,dword ptr [0x105f71f0] */
    r[1] = r[0]; /* 103112d1 MOV ECX,EAX */
    D3_CALLI(r[5], 0x103112d5u); /* 103112d3 CALL EBP */
    r[1] = r[0]; /* 103112d5 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x103112ddu); /* 103112d7 CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 103112dd PUSH EAX */
    d3_push(c, 0x3u); /* 103112de PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x103112e6u); /* 103112e0 CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 103112e6 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x103112eeu); /* 103112e8 CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x18u; /* 103112ee LEA ECX,[ESP + 0x18] */
    D3_CALLI(rd32(0x105f722cu), 0x103112f8u); /* 103112f2 CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x14u; /* 103112f8 LEA ECX,[ESP + 0x14] */
L_103112fc:
    D3_CALLI(rd32(0x105f722cu), 0x10311302u); /* 103112fc CALL dword ptr [0x105f722c] */
    d3_push(c, r[7]); /* 10311302 PUSH EDI */
    D3_CALLD(0x105a40c6u, FN_105a40c6, 0x10311308u); /* 10311303 CALL 0x105a40c6 */
    d3_push(c, r[0]); /* 10311308 PUSH EAX */
    r[1] = r[4] + 0x20u; /* 10311309 LEA ECX,[ESP + 0x20] */
    D3_CALLI(r[6], 0x1031130fu); /* 1031130d CALL ESI */
L_1031130f:
    r[0] = rd32(0x105f7290u); /* 1031130f MOV EAX,[0x105f7290] */
    r[1] = rd32(r[0]); /* 10311314 MOV ECX,dword ptr [EAX] */
    d3_push(c, r[1]); /* 10311316 PUSH ECX */
    d3_push(c, 0xffffffffu); /* 10311317 PUSH -0x1 */
    d3_push(c, 0x0u); /* 10311319 PUSH 0x0 */
    d3_push(c, 0x0u); /* 1031131b PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 1031131d PUSH 0x1068c5b0 */
    r[2] = r[4] + 0x28u; /* 10311322 LEA EDX,[ESP + 0x28] */
    d3_push(c, r[2]); /* 10311326 PUSH EDX */
    r[0] = r[4] + 0x34u; /* 10311327 LEA EAX,[ESP + 0x34] */
    d3_push(c, r[0]); /* 1031132b PUSH EAX */
    d3_push(c, 0x1069e280u); /* 1031132c PUSH 0x1069e280 */
    r[1] = r[4] + 0x38u; /* 10311331 LEA ECX,[ESP + 0x38] */
    d3_push(c, r[1]); /* 10311335 PUSH ECX */
L_10311336:
    D3_CALLI(r[3], 0x10311338u); /* 10311336 CALL EBX */
    r[1] = r[0]; /* 10311338 MOV ECX,EAX */
    D3_CALLI(r[5], 0x1031133cu); /* 1031133a CALL EBP */
L_1031133c:
    r[1] = r[0]; /* 1031133c MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x10311344u); /* 1031133e CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 10311344 PUSH EAX */
    d3_push(c, 0x3u); /* 10311345 PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x1031134du); /* 10311347 CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 1031134d MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x10311355u); /* 1031134f CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x14u; /* 10311355 LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x1031135fu); /* 10311359 CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x18u; /* 1031135f LEA ECX,[ESP + 0x18] */
    D3_CALLI(rd32(0x105f722cu), 0x10311369u); /* 10311363 CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x1cu; /* 10311369 LEA ECX,[ESP + 0x1c] */
    D3_CALLI(rd32(0x105f722cu), 0x10311373u); /* 1031136d CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x20u; /* 10311373 LEA ECX,[ESP + 0x20] */
    D3_CALLI(rd32(0x105f722cu), 0x1031137du); /* 10311377 CALL dword ptr [0x105f722c] */
    goto L_1031137d;
L_103113f5:
    d3_push(c, r[7]); /* 103113f5 PUSH EDI */
    D3_CALLD(0x1059225au, FN_1059225a, 0x103113fbu); /* 103113f6 CALL 0x1059225a */
    r[6] = rd32(0x105f72c0u); /* 103113fb MOV ESI,dword ptr [0x105f72c0] */
    d3_push(c, r[0]); /* 10311401 PUSH EAX */
    r[1] = r[4] + 0x24u; /* 10311402 LEA ECX,[ESP + 0x24] */
    D3_CALLI(r[6], 0x10311408u); /* 10311406 CALL ESI */
    r[1] = rd32(0x105f7290u); /* 10311408 MOV ECX,dword ptr [0x105f7290] */
    r[2] = rd32(r[1]); /* 1031140e MOV EDX,dword ptr [ECX] */
    r[3] = rd32(0x105f6670u); /* 10311410 MOV EBX,dword ptr [0x105f6670] */
    d3_push(c, r[2]); /* 10311416 PUSH EDX */
    d3_push(c, 0xffffffffu); /* 10311417 PUSH -0x1 */
    d3_push(c, 0x0u); /* 10311419 PUSH 0x0 */
    d3_push(c, 0x0u); /* 1031141b PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 1031141d PUSH 0x1068c5b0 */
    r[0] = r[4] + 0x28u; /* 10311422 LEA EAX,[ESP + 0x28] */
    d3_push(c, r[0]); /* 10311426 PUSH EAX */
    r[1] = r[4] + 0x38u; /* 10311427 LEA ECX,[ESP + 0x38] */
    d3_push(c, r[1]); /* 1031142b PUSH ECX */
    d3_push(c, 0x106a1df0u); /* 1031142c PUSH 0x106a1df0 */
    r[2] = r[4] + 0x38u; /* 10311431 LEA EDX,[ESP + 0x38] */
    d3_push(c, r[2]); /* 10311435 PUSH EDX */
    D3_CALLI(r[3], 0x10311438u); /* 10311436 CALL EBX */
    r[5] = rd32(0x105f71f0u); /* 10311438 MOV EBP,dword ptr [0x105f71f0] */
    r[1] = r[0]; /* 1031143e MOV ECX,EAX */
    D3_CALLI(r[5], 0x10311442u); /* 10311440 CALL EBP */
    r[1] = r[0]; /* 10311442 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x1031144au); /* 10311444 CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 1031144a PUSH EAX */
    d3_push(c, 0x3u); /* 1031144b PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x10311453u); /* 1031144d CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 10311453 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x1031145bu); /* 10311455 CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x14u; /* 1031145b LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x10311465u); /* 1031145f CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x18u; /* 10311465 LEA ECX,[ESP + 0x18] */
    goto L_103112fc; /* 10311469 JMP 0x103112fc */
L_1031150e:
    d3_push(c, r[7]); /* 1031150e PUSH EDI */
    D3_CALLD(0x1059225au, FN_1059225a, 0x10311514u); /* 1031150f CALL 0x1059225a */
    r[6] = rd32(0x105f72c0u); /* 10311514 MOV ESI,dword ptr [0x105f72c0] */
    d3_push(c, r[0]); /* 1031151a PUSH EAX */
    r[1] = r[4] + 0x24u; /* 1031151b LEA ECX,[ESP + 0x24] */
    D3_CALLI(r[6], 0x10311521u); /* 1031151f CALL ESI */
    r[1] = rd32(0x105f7290u); /* 10311521 MOV ECX,dword ptr [0x105f7290] */
    r[2] = rd32(r[1]); /* 10311527 MOV EDX,dword ptr [ECX] */
    r[3] = rd32(0x105f6670u); /* 10311529 MOV EBX,dword ptr [0x105f6670] */
    d3_push(c, r[2]); /* 1031152f PUSH EDX */
    d3_push(c, 0xffffffffu); /* 10311530 PUSH -0x1 */
    d3_push(c, 0x0u); /* 10311532 PUSH 0x0 */
    d3_push(c, 0x0u); /* 10311534 PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 10311536 PUSH 0x1068c5b0 */
    r[0] = r[4] + 0x28u; /* 1031153b LEA EAX,[ESP + 0x28] */
    d3_push(c, r[0]); /* 1031153f PUSH EAX */
    r[1] = r[4] + 0x38u; /* 10311540 LEA ECX,[ESP + 0x38] */
    d3_push(c, r[1]); /* 10311544 PUSH ECX */
    d3_push(c, 0x106a1e34u); /* 10311545 PUSH 0x106a1e34 */
    r[2] = r[4] + 0x38u; /* 1031154a LEA EDX,[ESP + 0x38] */
    d3_push(c, r[2]); /* 1031154e PUSH EDX */
    D3_CALLI(r[3], 0x10311551u); /* 1031154f CALL EBX */
    r[5] = rd32(0x105f71f0u); /* 10311551 MOV EBP,dword ptr [0x105f71f0] */
    r[1] = r[0]; /* 10311557 MOV ECX,EAX */
    D3_CALLI(r[5], 0x1031155bu); /* 10311559 CALL EBP */
    r[1] = r[0]; /* 1031155b MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x10311563u); /* 1031155d CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 10311563 PUSH EAX */
    d3_push(c, 0x3u); /* 10311564 PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x1031156cu); /* 10311566 CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 1031156c MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x10311574u); /* 1031156e CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x14u; /* 10311574 LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x1031157eu); /* 10311578 CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x18u; /* 1031157e LEA ECX,[ESP + 0x18] */
    goto L_103112fc; /* 10311582 JMP 0x103112fc */
L_10311587:
    d3_push(c, r[5]); /* 10311587 PUSH EBP */
    D3_CALLD(0x1059225au, FN_1059225a, 0x1031158du); /* 10311588 CALL 0x1059225a */
    r[6] = rd32(0x105f72c0u); /* 1031158d MOV ESI,dword ptr [0x105f72c0] */
    d3_push(c, r[0]); /* 10311593 PUSH EAX */
    r[1] = r[4] + 0x24u; /* 10311594 LEA ECX,[ESP + 0x24] */
    D3_CALLI(r[6], 0x1031159au); /* 10311598 CALL ESI */
    r[0] = rd32(0x105f7290u); /* 1031159a MOV EAX,[0x105f7290] */
    r[1] = rd32(r[0]); /* 1031159f MOV ECX,dword ptr [EAX] */
    r[7] = rd32(0x105f6670u); /* 103115a1 MOV EDI,dword ptr [0x105f6670] */
    d3_push(c, r[1]); /* 103115a7 PUSH ECX */
    d3_push(c, 0xffffffffu); /* 103115a8 PUSH -0x1 */
    d3_push(c, 0x0u); /* 103115aa PUSH 0x0 */
    d3_push(c, 0x0u); /* 103115ac PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 103115ae PUSH 0x1068c5b0 */
    r[2] = r[4] + 0x28u; /* 103115b3 LEA EDX,[ESP + 0x28] */
    d3_push(c, r[2]); /* 103115b7 PUSH EDX */
    r[0] = r[4] + 0x38u; /* 103115b8 LEA EAX,[ESP + 0x38] */
    d3_push(c, r[0]); /* 103115bc PUSH EAX */
    d3_push(c, 0x106a1db0u); /* 103115bd PUSH 0x106a1db0 */
    r[1] = r[4] + 0x38u; /* 103115c2 LEA ECX,[ESP + 0x38] */
    d3_push(c, r[1]); /* 103115c6 PUSH ECX */
    D3_CALLI(r[7], 0x103115c9u); /* 103115c7 CALL EDI */
    r[3] = rd32(0x105f71f0u); /* 103115c9 MOV EBX,dword ptr [0x105f71f0] */
    r[1] = r[0]; /* 103115cf MOV ECX,EAX */
    D3_CALLI(r[3], 0x103115d3u); /* 103115d1 CALL EBX */
    r[1] = r[0]; /* 103115d3 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x103115dbu); /* 103115d5 CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 103115db PUSH EAX */
    d3_push(c, 0x3u); /* 103115dc PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x103115e4u); /* 103115de CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 103115e4 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x103115ecu); /* 103115e6 CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x14u; /* 103115ec LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x103115f6u); /* 103115f0 CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x18u; /* 103115f6 LEA ECX,[ESP + 0x18] */
    D3_CALLI(rd32(0x105f722cu), 0x10311600u); /* 103115fa CALL dword ptr [0x105f722c] */
    d3_push(c, r[5]); /* 10311600 PUSH EBP */
    D3_CALLD(0x105a40c6u, FN_105a40c6, 0x10311606u); /* 10311601 CALL 0x105a40c6 */
    d3_push(c, r[0]); /* 10311606 PUSH EAX */
    r[1] = r[4] + 0x20u; /* 10311607 LEA ECX,[ESP + 0x20] */
    D3_CALLI(r[6], 0x1031160du); /* 1031160b CALL ESI */
    r[2] = rd32(0x105f7290u); /* 1031160d MOV EDX,dword ptr [0x105f7290] */
    r[0] = rd32(r[2]); /* 10311613 MOV EAX,dword ptr [EDX] */
    d3_push(c, r[0]); /* 10311615 PUSH EAX */
    d3_push(c, 0xffffffffu); /* 10311616 PUSH -0x1 */
    d3_push(c, 0x0u); /* 10311618 PUSH 0x0 */
    d3_push(c, 0x0u); /* 1031161a PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 1031161c PUSH 0x1068c5b0 */
    r[1] = r[4] + 0x28u; /* 10311621 LEA ECX,[ESP + 0x28] */
    d3_push(c, r[1]); /* 10311625 PUSH ECX */
    r[2] = r[4] + 0x34u; /* 10311626 LEA EDX,[ESP + 0x34] */
    d3_push(c, r[2]); /* 1031162a PUSH EDX */
    d3_push(c, 0x1069e280u); /* 1031162b PUSH 0x1069e280 */
    r[0] = r[4] + 0x38u; /* 10311630 LEA EAX,[ESP + 0x38] */
    d3_push(c, r[0]); /* 10311634 PUSH EAX */
    D3_CALLI(r[7], 0x10311637u); /* 10311635 CALL EDI */
    r[1] = r[0]; /* 10311637 MOV ECX,EAX */
    D3_CALLI(r[3], 0x1031163bu); /* 10311639 CALL EBX */
    goto L_1031133c; /* 1031163b JMP 0x1031133c */
L_10311640:
    d3_push(c, r[7]); /* 10311640 PUSH EDI */
    D3_CALLD(0x1059225au, FN_1059225a, 0x10311646u); /* 10311641 CALL 0x1059225a */
    r[6] = rd32(0x105f72c0u); /* 10311646 MOV ESI,dword ptr [0x105f72c0] */
    d3_push(c, r[0]); /* 1031164c PUSH EAX */
    r[1] = r[4] + 0x24u; /* 1031164d LEA ECX,[ESP + 0x24] */
    D3_CALLI(r[6], 0x10311653u); /* 10311651 CALL ESI */
    r[2] = rd32(0x105f7290u); /* 10311653 MOV EDX,dword ptr [0x105f7290] */
    r[0] = rd32(r[2]); /* 10311659 MOV EAX,dword ptr [EDX] */
    r[3] = rd32(0x105f6670u); /* 1031165b MOV EBX,dword ptr [0x105f6670] */
    d3_push(c, r[0]); /* 10311661 PUSH EAX */
    d3_push(c, 0xffffffffu); /* 10311662 PUSH -0x1 */
    d3_push(c, 0x0u); /* 10311664 PUSH 0x0 */
    d3_push(c, 0x0u); /* 10311666 PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 10311668 PUSH 0x1068c5b0 */
    r[1] = r[4] + 0x28u; /* 1031166d LEA ECX,[ESP + 0x28] */
    d3_push(c, r[1]); /* 10311671 PUSH ECX */
    r[2] = r[4] + 0x38u; /* 10311672 LEA EDX,[ESP + 0x38] */
    d3_push(c, r[2]); /* 10311676 PUSH EDX */
    d3_push(c, 0x106a1db0u); /* 10311677 PUSH 0x106a1db0 */
    r[0] = r[4] + 0x38u; /* 1031167c LEA EAX,[ESP + 0x38] */
    d3_push(c, r[0]); /* 10311680 PUSH EAX */
    D3_CALLI(r[3], 0x10311683u); /* 10311681 CALL EBX */
    r[5] = rd32(0x105f71f0u); /* 10311683 MOV EBP,dword ptr [0x105f71f0] */
    r[1] = r[0]; /* 10311689 MOV ECX,EAX */
    D3_CALLI(r[5], 0x1031168du); /* 1031168b CALL EBP */
    r[1] = r[0]; /* 1031168d MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x10311695u); /* 1031168f CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 10311695 PUSH EAX */
    d3_push(c, 0x3u); /* 10311696 PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x1031169eu); /* 10311698 CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 1031169e MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x103116a6u); /* 103116a0 CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x14u; /* 103116a6 LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x103116b0u); /* 103116aa CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x18u; /* 103116b0 LEA ECX,[ESP + 0x18] */
    D3_CALLI(rd32(0x105f722cu), 0x103116bau); /* 103116b4 CALL dword ptr [0x105f722c] */
    d3_push(c, r[7]); /* 103116ba PUSH EDI */
    D3_CALLD(0x105a40c6u, FN_105a40c6, 0x103116c0u); /* 103116bb CALL 0x105a40c6 */
    d3_push(c, r[0]); /* 103116c0 PUSH EAX */
    r[1] = r[4] + 0x20u; /* 103116c1 LEA ECX,[ESP + 0x20] */
    D3_CALLI(r[6], 0x103116c7u); /* 103116c5 CALL ESI */
    r[1] = rd32(0x105f7290u); /* 103116c7 MOV ECX,dword ptr [0x105f7290] */
    r[2] = rd32(r[1]); /* 103116cd MOV EDX,dword ptr [ECX] */
    d3_push(c, r[2]); /* 103116cf PUSH EDX */
    d3_push(c, 0xffffffffu); /* 103116d0 PUSH -0x1 */
    d3_push(c, 0x0u); /* 103116d2 PUSH 0x0 */
    d3_push(c, 0x0u); /* 103116d4 PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 103116d6 PUSH 0x1068c5b0 */
    r[0] = r[4] + 0x28u; /* 103116db LEA EAX,[ESP + 0x28] */
    d3_push(c, r[0]); /* 103116df PUSH EAX */
    r[1] = r[4] + 0x34u; /* 103116e0 LEA ECX,[ESP + 0x34] */
    d3_push(c, r[1]); /* 103116e4 PUSH ECX */
    d3_push(c, 0x1069e280u); /* 103116e5 PUSH 0x1069e280 */
    r[2] = r[4] + 0x38u; /* 103116ea LEA EDX,[ESP + 0x38] */
    d3_push(c, r[2]); /* 103116ee PUSH EDX */
    goto L_10311336; /* 103116ef JMP 0x10311336 */
L_1031171a:
    d3_push(c, r[7]); /* 1031171a PUSH EDI */
    D3_CALLD(0x1059225au, FN_1059225a, 0x10311720u); /* 1031171b CALL 0x1059225a */
    r[6] = rd32(0x105f72c0u); /* 10311720 MOV ESI,dword ptr [0x105f72c0] */
    d3_push(c, r[0]); /* 10311726 PUSH EAX */
    r[1] = r[4] + 0x24u; /* 10311727 LEA ECX,[ESP + 0x24] */
    D3_CALLI(r[6], 0x1031172du); /* 1031172b CALL ESI */
    r[0] = rd32(0x105f7290u); /* 1031172d MOV EAX,[0x105f7290] */
    r[1] = rd32(r[0]); /* 10311732 MOV ECX,dword ptr [EAX] */
    r[3] = rd32(0x105f6670u); /* 10311734 MOV EBX,dword ptr [0x105f6670] */
    d3_push(c, r[1]); /* 1031173a PUSH ECX */
    d3_push(c, 0xffffffffu); /* 1031173b PUSH -0x1 */
    d3_push(c, 0x0u); /* 1031173d PUSH 0x0 */
    d3_push(c, 0x0u); /* 1031173f PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 10311741 PUSH 0x1068c5b0 */
    r[2] = r[4] + 0x28u; /* 10311746 LEA EDX,[ESP + 0x28] */
    d3_push(c, r[2]); /* 1031174a PUSH EDX */
    r[0] = r[4] + 0x38u; /* 1031174b LEA EAX,[ESP + 0x38] */
    d3_push(c, r[0]); /* 1031174f PUSH EAX */
    d3_push(c, 0x106a1e70u); /* 10311750 PUSH 0x106a1e70 */
    r[1] = r[4] + 0x38u; /* 10311755 LEA ECX,[ESP + 0x38] */
    d3_push(c, r[1]); /* 10311759 PUSH ECX */
    D3_CALLI(r[3], 0x1031175cu); /* 1031175a CALL EBX */
    r[5] = rd32(0x105f71f0u); /* 1031175c MOV EBP,dword ptr [0x105f71f0] */
    r[1] = r[0]; /* 10311762 MOV ECX,EAX */
    D3_CALLI(r[5], 0x10311766u); /* 10311764 CALL EBP */
    r[1] = r[0]; /* 10311766 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x1031176eu); /* 10311768 CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 1031176e PUSH EAX */
    d3_push(c, 0x3u); /* 1031176f PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x10311777u); /* 10311771 CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 10311777 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x1031177fu); /* 10311779 CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x14u; /* 1031177f LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x10311789u); /* 10311783 CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x18u; /* 10311789 LEA ECX,[ESP + 0x18] */
    D3_CALLI(rd32(0x105f722cu), 0x10311793u); /* 1031178d CALL dword ptr [0x105f722c] */
    d3_push(c, r[7]); /* 10311793 PUSH EDI */
    D3_CALLD(0x105a40c6u, FN_105a40c6, 0x10311799u); /* 10311794 CALL 0x105a40c6 */
    d3_push(c, r[0]); /* 10311799 PUSH EAX */
    r[1] = r[4] + 0x20u; /* 1031179a LEA ECX,[ESP + 0x20] */
    D3_CALLI(r[6], 0x103117a0u); /* 1031179e CALL ESI */
    r[2] = rd32(0x105f7290u); /* 103117a0 MOV EDX,dword ptr [0x105f7290] */
    r[0] = rd32(r[2]); /* 103117a6 MOV EAX,dword ptr [EDX] */
    d3_push(c, r[0]); /* 103117a8 PUSH EAX */
    d3_push(c, 0xffffffffu); /* 103117a9 PUSH -0x1 */
    d3_push(c, 0x0u); /* 103117ab PUSH 0x0 */
    d3_push(c, 0x0u); /* 103117ad PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 103117af PUSH 0x1068c5b0 */
    r[1] = r[4] + 0x28u; /* 103117b4 LEA ECX,[ESP + 0x28] */
    d3_push(c, r[1]); /* 103117b8 PUSH ECX */
    r[2] = r[4] + 0x34u; /* 103117b9 LEA EDX,[ESP + 0x34] */
    d3_push(c, r[2]); /* 103117bd PUSH EDX */
    d3_push(c, 0x1069e280u); /* 103117be PUSH 0x1069e280 */
    r[0] = r[4] + 0x38u; /* 103117c3 LEA EAX,[ESP + 0x38] */
    d3_push(c, r[0]); /* 103117c7 PUSH EAX */
    goto L_10311336; /* 103117c8 JMP 0x10311336 */
L_10311b05:
    d3_push(c, r[6]); /* 10311b05 PUSH ESI */
    D3_CALLD(0x1059225au, FN_1059225a, 0x10311b0bu); /* 10311b06 CALL 0x1059225a */
    r[7] = rd32(0x105f72c0u); /* 10311b0b MOV EDI,dword ptr [0x105f72c0] */
    d3_push(c, r[0]); /* 10311b11 PUSH EAX */
    r[1] = r[4] + 0x24u; /* 10311b12 LEA ECX,[ESP + 0x24] */
    D3_CALLI(r[7], 0x10311b18u); /* 10311b16 CALL EDI */
    r[1] = rd32(0x105f7290u); /* 10311b18 MOV ECX,dword ptr [0x105f7290] */
    r[2] = rd32(r[1]); /* 10311b1e MOV EDX,dword ptr [ECX] */
    r[3] = rd32(0x105f6670u); /* 10311b20 MOV EBX,dword ptr [0x105f6670] */
    d3_push(c, r[2]); /* 10311b26 PUSH EDX */
    d3_push(c, 0xffffffffu); /* 10311b27 PUSH -0x1 */
    d3_push(c, 0x0u); /* 10311b29 PUSH 0x0 */
    d3_push(c, 0x0u); /* 10311b2b PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 10311b2d PUSH 0x1068c5b0 */
    r[0] = r[4] + 0x28u; /* 10311b32 LEA EAX,[ESP + 0x28] */
    d3_push(c, r[0]); /* 10311b36 PUSH EAX */
    r[1] = r[4] + 0x38u; /* 10311b37 LEA ECX,[ESP + 0x38] */
    d3_push(c, r[1]); /* 10311b3b PUSH ECX */
    d3_push(c, 0x106a1ef0u); /* 10311b3c PUSH 0x106a1ef0 */
    r[2] = r[4] + 0x38u; /* 10311b41 LEA EDX,[ESP + 0x38] */
    d3_push(c, r[2]); /* 10311b45 PUSH EDX */
    D3_CALLI(r[3], 0x10311b48u); /* 10311b46 CALL EBX */
    r[5] = rd32(0x105f71f0u); /* 10311b48 MOV EBP,dword ptr [0x105f71f0] */
    r[1] = r[0]; /* 10311b4e MOV ECX,EAX */
    D3_CALLI(r[5], 0x10311b52u); /* 10311b50 CALL EBP */
    r[1] = r[0]; /* 10311b52 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x10311b5au); /* 10311b54 CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 10311b5a PUSH EAX */
    d3_push(c, 0x3u); /* 10311b5b PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x10311b63u); /* 10311b5d CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 10311b63 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x10311b6bu); /* 10311b65 CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x14u; /* 10311b6b LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x10311b75u); /* 10311b6f CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x18u; /* 10311b75 LEA ECX,[ESP + 0x18] */
    D3_CALLI(rd32(0x105f722cu), 0x10311b7fu); /* 10311b79 CALL dword ptr [0x105f722c] */
    d3_push(c, r[6]); /* 10311b7f PUSH ESI */
    D3_CALLD(0x105a40c6u, FN_105a40c6, 0x10311b85u); /* 10311b80 CALL 0x105a40c6 */
    d3_push(c, r[0]); /* 10311b85 PUSH EAX */
    r[1] = r[4] + 0x20u; /* 10311b86 LEA ECX,[ESP + 0x20] */
    D3_CALLI(r[7], 0x10311b8cu); /* 10311b8a CALL EDI */
    goto L_1031130f; /* 10311b8c JMP 0x1031130f */
L_10311bad:
    d3_push(c, r[7]); /* 10311bad PUSH EDI */
    D3_CALLD(0x1059225au, FN_1059225a, 0x10311bb3u); /* 10311bae CALL 0x1059225a */
    r[6] = rd32(0x105f72c0u); /* 10311bb3 MOV ESI,dword ptr [0x105f72c0] */
    d3_push(c, r[0]); /* 10311bb9 PUSH EAX */
    r[1] = r[4] + 0x24u; /* 10311bba LEA ECX,[ESP + 0x24] */
    D3_CALLI(r[6], 0x10311bc0u); /* 10311bbe CALL ESI */
    r[1] = rd32(0x105f7290u); /* 10311bc0 MOV ECX,dword ptr [0x105f7290] */
    r[2] = rd32(r[1]); /* 10311bc6 MOV EDX,dword ptr [ECX] */
    r[3] = rd32(0x105f6670u); /* 10311bc8 MOV EBX,dword ptr [0x105f6670] */
    d3_push(c, r[2]); /* 10311bce PUSH EDX */
    d3_push(c, 0xffffffffu); /* 10311bcf PUSH -0x1 */
    d3_push(c, 0x0u); /* 10311bd1 PUSH 0x0 */
    d3_push(c, 0x0u); /* 10311bd3 PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 10311bd5 PUSH 0x1068c5b0 */
    r[0] = r[4] + 0x28u; /* 10311bda LEA EAX,[ESP + 0x28] */
    d3_push(c, r[0]); /* 10311bde PUSH EAX */
    r[1] = r[4] + 0x38u; /* 10311bdf LEA ECX,[ESP + 0x38] */
    d3_push(c, r[1]); /* 10311be3 PUSH ECX */
    d3_push(c, 0x106a1ea8u); /* 10311be4 PUSH 0x106a1ea8 */
    r[2] = r[4] + 0x38u; /* 10311be9 LEA EDX,[ESP + 0x38] */
    d3_push(c, r[2]); /* 10311bed PUSH EDX */
    D3_CALLI(r[3], 0x10311bf0u); /* 10311bee CALL EBX */
    r[5] = rd32(0x105f71f0u); /* 10311bf0 MOV EBP,dword ptr [0x105f71f0] */
    r[1] = r[0]; /* 10311bf6 MOV ECX,EAX */
    D3_CALLI(r[5], 0x10311bfau); /* 10311bf8 CALL EBP */
    r[1] = r[0]; /* 10311bfa MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x10311c02u); /* 10311bfc CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 10311c02 PUSH EAX */
    d3_push(c, 0x3u); /* 10311c03 PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x10311c0bu); /* 10311c05 CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 10311c0b MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x10311c13u); /* 10311c0d CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x14u; /* 10311c13 LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x10311c1du); /* 10311c17 CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x18u; /* 10311c1d LEA ECX,[ESP + 0x18] */
    goto L_103112fc; /* 10311c21 JMP 0x103112fc */
L_10311c73:
    d3_push(c, r[6]); /* 10311c73 PUSH ESI */
    D3_CALLD(0x1059225au, FN_1059225a, 0x10311c79u); /* 10311c74 CALL 0x1059225a */
    r[7] = rd32(0x105f72c0u); /* 10311c79 MOV EDI,dword ptr [0x105f72c0] */
    d3_push(c, r[0]); /* 10311c7f PUSH EAX */
    r[1] = r[4] + 0x24u; /* 10311c80 LEA ECX,[ESP + 0x24] */
    D3_CALLI(r[7], 0x10311c86u); /* 10311c84 CALL EDI */
    r[1] = rd32(0x105f7290u); /* 10311c86 MOV ECX,dword ptr [0x105f7290] */
    r[2] = rd32(r[1]); /* 10311c8c MOV EDX,dword ptr [ECX] */
    r[3] = rd32(0x105f6670u); /* 10311c8e MOV EBX,dword ptr [0x105f6670] */
    d3_push(c, r[2]); /* 10311c94 PUSH EDX */
    d3_push(c, 0xffffffffu); /* 10311c95 PUSH -0x1 */
    d3_push(c, 0x0u); /* 10311c97 PUSH 0x0 */
    d3_push(c, 0x0u); /* 10311c99 PUSH 0x0 */
    d3_push(c, 0x1068c5b0u); /* 10311c9b PUSH 0x1068c5b0 */
    r[0] = r[4] + 0x28u; /* 10311ca0 LEA EAX,[ESP + 0x28] */
    d3_push(c, r[0]); /* 10311ca4 PUSH EAX */
    r[1] = r[4] + 0x38u; /* 10311ca5 LEA ECX,[ESP + 0x38] */
    d3_push(c, r[1]); /* 10311ca9 PUSH ECX */
    d3_push(c, 0x106a1f38u); /* 10311caa PUSH 0x106a1f38 */
    r[2] = r[4] + 0x38u; /* 10311caf LEA EDX,[ESP + 0x38] */
    d3_push(c, r[2]); /* 10311cb3 PUSH EDX */
    D3_CALLI(r[3], 0x10311cb6u); /* 10311cb4 CALL EBX */
    r[5] = rd32(0x105f71f0u); /* 10311cb6 MOV EBP,dword ptr [0x105f71f0] */
    r[1] = r[0]; /* 10311cbc MOV ECX,EAX */
    D3_CALLI(r[5], 0x10311cc0u); /* 10311cbe CALL EBP */
    r[1] = r[0]; /* 10311cc0 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f7210u), 0x10311cc8u); /* 10311cc2 CALL dword ptr [0x105f7210] */
    d3_push(c, r[0]); /* 10311cc8 PUSH EAX */
    d3_push(c, 0x3u); /* 10311cc9 PUSH 0x3 */
    D3_CALLI(rd32(0x105f71f4u), 0x10311cd1u); /* 10311ccb CALL dword ptr [0x105f71f4] */
    r[1] = r[0]; /* 10311cd1 MOV ECX,EAX */
    D3_CALLI(rd32(0x105f71f8u), 0x10311cd9u); /* 10311cd3 CALL dword ptr [0x105f71f8] */
    r[1] = r[4] + 0x14u; /* 10311cd9 LEA ECX,[ESP + 0x14] */
    D3_CALLI(rd32(0x105f722cu), 0x10311ce3u); /* 10311cdd CALL dword ptr [0x105f722c] */
    r[1] = r[4] + 0x18u; /* 10311ce3 LEA ECX,[ESP + 0x18] */
    D3_CALLI(rd32(0x105f722cu), 0x10311cedu); /* 10311ce7 CALL dword ptr [0x105f722c] */
    d3_push(c, r[6]); /* 10311ced PUSH ESI */
    D3_CALLD(0x105a40c6u, FN_105a40c6, 0x10311cf3u); /* 10311cee CALL 0x105a40c6 */
    d3_push(c, r[0]); /* 10311cf3 PUSH EAX */
    r[1] = r[4] + 0x20u; /* 10311cf4 LEA ECX,[ESP + 0x20] */
    D3_CALLI(r[7], 0x10311cfau); /* 10311cf8 CALL EDI */
    goto L_1031130f; /* 10311cfa JMP 0x1031130f */
L_1031137d:
    ex_out(c, 0);
}

#define EX_WHITE_VB 0x1086db98u   /* the engine's white COLOR0 vertex buffer */
#define EX_WHITE_LEN 0x1086db94u  /* its length in vertices (4 bytes each) */

/* 10311898..10311a40, taken when the declaration has COLOR1 but no COLOR0 (usage 10, index 1
 * without index 0): the stream of the last element plus one gets the white buffer, (re)made
 * when it is shorter than the vertex count of that stream, and a COLOR0 D3DCOLOR element at
 * offset 0 is inserted before D3DDECL_END. Cold, the original's instructions and flags one for
 * one. Returns 1 to go on at 10311a4d (the element was added), 0 at 10311a42. EDX = the element
 * count, EBX = 0 on entry. */
static __attribute__((noinline, cold)) int ex_color(X86 *c) {
    uint32_t *const r = c->r;
    r[7] = rd16(r[4] + r[2] * 8u + 0x38u);
    r[0] = rd32(r[6] + 8u);
    r[1] = rd32(r[0]);
    r[2] = r[4] + 0x14u;
    d3_push(c, r[2]);
    r[2] = r[4] + 0x30u;
    d3_push(c, r[2]);
    r[2] = r[4] + 0x28u;
    d3_push(c, r[2]);
    r[7] += 1u;
    r[2] = r[7] - 1u;
    d3_push(c, r[2]);
    d3_push(c, r[0]);
    r[0] = rd32(r[1] + 0x194u);
    r[5] = 0;
    fl_logic(c, 0u, 32u);
    D3_CALLI(r[0], 0x103118c3u); /* GetStreamSource(dev, stream, &vb, &offset, &stride) */
    fl_logic(c, r[0], 32u);
    if ((int32_t)r[0] >= 0) {
        r[0] = rd32(r[4] + 0x20u);
        r[1] = rd32(r[0]);
        r[2] = r[4] + 0x30u;
        d3_push(c, r[2]);
        d3_push(c, r[0]);
        r[0] = rd32(r[1] + 0x34u);
        D3_CALLI(r[0], 0x103118d8u); /* vb->GetDesc(&desc) */
        fl_logic(c, r[0], 32u);
        if ((int32_t)r[0] >= 0) {
            r[0] = rd32(r[4] + 0x40u);
            r[2] = 0;
            div32(c, rd32(r[4] + 0x14u), 0x103118e2u); /* size / stride */
            r[5] = r[0];
        }
        r[0] = rd32(r[4] + 0x20u);
        r[1] = rd32(r[0]);
        r[2] = rd32(r[1] + 8u);
        d3_push(c, r[0]);
        D3_CALLI(r[2], 0x103118f4u); /* vb->Release */
        wr32(r[4] + 0x20u, r[3]);
    }
    /* 103118f8 */
    r[1] = rd32(EX_WHITE_VB);
    r[0] = 0;
    fl_sub(c, r[1], r[3], 32u);
    wr32(r[4] + 0x1cu, r[0]);
    if (r[1] != r[3]) {
        r[0] = rd32(r[1]);
        r[0] = rd32(r[0] + 0xcu);
        r[2] = r[4] + 0x1cu;
        d3_push(c, r[2]);
        d3_push(c, r[1]);
        D3_CALLI(r[0], 0x10311915u); /* white->GetDevice(&dev) */
        r[0] = rd32(r[4] + 0x1cu);
    }
    /* 10311919 */
    fl_sub(c, r[5], r[3], 32u);
    if (r[5] <= r[3])
        goto l_19c9;
    {
        const uint32_t len = rd32(EX_WHITE_LEN);
        fl_sub(c, r[5], len, 32u);
        if (r[5] <= len) {
            const uint32_t dev = rd32(r[6] + 8u);
            fl_sub(c, r[0], dev, 32u);
            if (r[0] == dev)
                goto l_19c9;
        }
    }
    fl_sub(c, r[5], 0x800u, 32u);
    if (r[5] < 0x800u)
        r[5] = 0x800u;
    r[0] = rd32(r[6] + 8u);
    r[1] = rd32(r[0]);
    d3_push(c, 0u);
    r[2] = r[4] + 0x24u;
    d3_push(c, r[2]);
    d3_push(c, 1u);
    d3_push(c, 0u);
    d3_push(c, 0u);
    r[3] = r[5] * 4u;
    d3_push(c, r[3]);
    d3_push(c, r[0]);
    r[0] = rd32(r[1] + 0x68u);
    D3_CALLI(r[0], 0x1031195fu); /* CreateVertexBuffer(dev, len*4, 0, 0, MANAGED, &vb, 0) */
    fl_logic(c, r[0], 32u);
    if ((int32_t)r[0] < 0)
        goto l_19c5;
    r[0] = rd32(EX_WHITE_VB);
    fl_logic(c, r[0], 32u);
    if (r[0] != 0u) {
        r[1] = rd32(r[0]);
        r[2] = rd32(r[1] + 8u);
        d3_push(c, r[0]);
        D3_CALLI(r[2], 0x10311974u); /* old white->Release */
    }
    r[0] = rd32(r[4] + 0x20u);
    r[1] = rd32(r[0]);
    d3_push(c, 0u);
    r[2] = r[4] + 0x1cu;
    d3_push(c, r[2]);
    d3_push(c, r[3]);
    d3_push(c, 0u);
    wr32(EX_WHITE_VB, r[0]);
    d3_push(c, r[0]);
    r[0] = rd32(r[1] + 0x2cu);
    wr32(EX_WHITE_LEN, r[5]);
    D3_CALLI(r[0], 0x10311995u); /* vb->Lock(0, len*4, &p, 0) */
    fl_logic(c, r[0], 32u);
    if ((int32_t)r[0] < 0)
        goto l_19c5;
    r[1] = rd32(EX_WHITE_LEN);
    r[0] = rd32(r[4] + 0x18u);
    r[2] = r[1] * 4u;
    d3_push(c, r[2]);
    d3_push(c, 0xffu);
    d3_push(c, r[0]);
    D3_CALLD(0x1056a710u, FN_1056a710, 0x103119b6u); /* memset(p, 0xff, len*4) */
    r[0] = rd32(r[4] + 0x2cu);
    r[1] = rd32(r[0]);
    r[2] = rd32(r[1] + 0x30u);
    fl_add32(c, r[4], 0xcu);
    r[4] += 0xcu;
    d3_push(c, r[0]);
    D3_CALLI(r[2], 0x103119c5u); /* vb->Unlock */
l_19c5:
    r[0] = rd32(r[4] + 0x1cu);
l_19c9:
    r[3] = 0;
    fl_sub(c, r[0], 0u, 32u);
    if (r[0] != 0u) {
        r[1] = rd32(r[0]);
        r[2] = rd32(r[1] + 8u);
        d3_push(c, r[0]);
        D3_CALLI(r[2], 0x103119d7u); /* dev->Release */
    }
    {
        const uint32_t len = rd32(EX_WHITE_LEN);
        fl_sub(c, r[5], len, 32u);
        if (r[5] > len)
            return 0;
    }
    fl_sub(c, r[5], 0u, 32u);
    if (r[5] == 0u)
        return 0;
    r[2] = rd32(EX_WHITE_VB);
    r[0] = rd32(r[6] + 8u);
    r[1] = rd32(r[0]);
    d3_push(c, 4u);
    d3_push(c, 0u);
    d3_push(c, r[2]);
    d3_push(c, r[7]);
    d3_push(c, r[0]);
    r[0] = rd32(r[1] + 0x190u);
    D3_CALLI(r[0], 0x103119fcu); /* SetStreamSource(dev, stream, white, 0, 4) */
    fl_logic(c, r[0], 32u);
    if ((int32_t)r[0] < 0)
        return 0;
    r[0] = rd32(r[4] + 0x28u);
    r[0] = r[4] + r[0] * 8u + 0x48u;
    d3_push(c, 8u);
    r[1] = r[0] - 8u;
    d3_push(c, r[1]);
    d3_push(c, r[0]);
    D3_CALLD(0x10564220u, FN_10564220, 0x10311a14u); /* memmove(&e[n], &e[n-1], 8) */
    r[0] = rd32(r[4] + 0x34u);
    wr16(r[4] + r[0] * 8u + 0x4cu, (uint16_t)r[7]);
    wr16(r[4] + r[0] * 8u + 0x4eu, (uint16_t)r[3]);
    wr8(r[4] + r[0] * 8u + 0x50u, 4u);
    wr8(r[4] + r[0] * 8u + 0x51u, 0u);
    wr8(r[4] + r[0] * 8u + 0x52u, 0xau);
    r[4] += 0xcu;
    wr8(r[4] + r[0] * 8u + 0x47u, 0u);
    fl_add32(c, r[0], 1u);
    r[0] += 1u;
    wr32(r[4] + 0x28u, r[0]);
    return 1;
}

/* bool eCDX9Primitive::Execute(), thiscall, RET. this: +4 the eCDX9Device wrapper, +8 the
 * IDirect3DDevice9, +0xc..+0x18 the four vertex buffers, +0x1c+16s stream s in use (byte),
 * +0x20+16s its offset, +0x24+16s its stride, +0x28+16s its frequency, +0x5c the index buffer,
 * +0x60 FVF mode (byte), +0x64 the FVF or the declaration, +0x68 indexed (byte), +0x6c the
 * primitive type, +0x70 the primitive count, +0x74/+0x78.. the draw arguments. The frame:
 * EBP-aligned to 8, 0x238 bytes; [ESP+0x26] the stream counter, +0x27 "cut", +0x28 the element
 * count, +0x48 the elements (GetDeclaration's copy). */
void g3n_10311240(X86 *c) {
    if (!G3OPT(D3D)) {
        fn_10311240(c);
        return;
    }
    uint32_t *const r = c->r;
    d3_push(c, r[5]);
    r[5] = r[4];
    r[4] = (r[4] & 0xfffffff8u) - 0x238u;
    d3_push(c, r[3]);
    d3_push(c, r[5]);
    d3_push(c, r[6]);
    r[6] = r[1];
    const uint8_t mode = rd8(r[6] + 0x60u);
    fl_sub(c, mode, 0u, 8u);
    d3_push(c, r[7]);
    wr32(r[4] + 0x28u, 0u);
    wr8(r[4] + 0x27u, 0u);
    if (mode == 0u) {
        r[0] = rd32(r[6] + 0x64u);
        r[1] = rd32(r[0]);
        r[2] = r[4] + 0x28u;
        d3_push(c, r[2]);
        r[2] = r[4] + 0x4cu;
        d3_push(c, r[2]);
        d3_push(c, r[0]);
        r[0] = rd32(r[1] + 0x10u);
        D3_CALLI(r[0], 0x1031127eu); /* decl->GetDeclaration(elems, &n) */
        r[7] = r[0];
        if ((int32_t)r[7] < 0) {
            fl_logic(c, r[7], 32u);
            ex_fail(c, 0x10311288u);
            return;
        }
    }
    /* 10311387: the four streams */
    wr8(r[4] + 0x26u, 0u);
    for (;;) {
        r[7] = rd8(r[4] + 0x26u);
        r[1] = rd32(r[6] + r[7] * 4u + 0xcu);
        int bound = 0;
        if (r[1] == 0u) {
            fl_logic(c, 0u, 32u);
        } else {
            r[2] = r[7] << 4;
            const uint8_t used = rd8(r[2] + r[6] + 0x1cu);
            r[3] = r[2] + r[6];
            if (used == 0u) {
                fl_sub(c, 0u, 0u, 8u);
            } else {
                c->eflags_af = 0; /* the CMP's; SHL writes the others */
                bound = 1;
            }
        }
        if (bound) {
            r[5] = rd32(r[3] + 0x24u);
            r[0] = rd32(r[6] + 8u);
            r[2] = rd32(r[0]);
            d3_push(c, r[5]);
            r[5] = shl32_f(c, r[7] + 2u, 4u);
            r[5] = rd32(r[6] + r[5]);
            d3_push(c, r[5]);
            d3_push(c, r[1]);
            d3_push(c, r[7]);
            d3_push(c, r[0]);
            r[0] = rd32(r[2] + 0x190u);
            D3_CALLI(r[0], 0x103113ceu); /* SetStreamSource(dev, s, vb, offset, stride) */
            r[5] = r[0];
            fl_logic(c, r[5], 32u);
            if ((int32_t)r[5] < 0) {
                ex_fail(c, 0x10311587u);
                return;
            }
            r[2] = rd32(r[3] + 0x28u);
            r[0] = rd32(r[6] + 8u);
            r[1] = rd32(r[0]);
            d3_push(c, r[2]);
            d3_push(c, r[7]);
            d3_push(c, r[0]);
            r[0] = rd32(r[1] + 0x198u);
            D3_CALLI(r[0], 0x103113ebu); /* SetStreamSourceFreq(dev, s, freq) */
            r[7] = r[0];
            if ((int32_t)r[7] < 0) {
                fl_logic(c, r[7], 32u);
                ex_fail(c, 0x103113f5u);
                return;
            }
        } else {
            r[0] = rd32(r[6] + 8u);
            r[1] = rd32(r[0]);
            r[2] = rd32(r[1] + 0x190u);
            d3_push(c, 0u);
            d3_push(c, 0u);
            d3_push(c, 0u);
            d3_push(c, r[7]);
            d3_push(c, r[0]);
            D3_CALLI(r[2], 0x10311483u); /* SetStreamSource(dev, s, NULL, 0, 0) */
            r[7] = r[0];
            if ((int32_t)r[7] < 0) {
                fl_logic(c, r[7], 32u);
                ex_fail(c, 0x10311640u);
                return;
            }
            /* 1031148d: cut the elements of stream s */
            uint32_t ecx = rd32(r[4] + 0x28u), edi = 0;
            if (ecx != 0u) {
                r[5] = (r[5] & 0xffff0000u) | rd8(r[4] + 0x26u);
                uint32_t ebx = r[4] + 0x48u;
                do {
                    if (rd16(ebx) == (uint16_t)r[5]) {
                        const uint32_t left = ecx - edi;
                        r[0] = left - 1u;
                        wr8(r[4] + 0x27u, 1u);
                        if (r[0] != 0u) {
                            fl_add32(c, left, 0xffffffffu);
                            r[1] = r[0] * 8u;
                            d3_push(c, r[1]);
                            r[2] = ebx + 8u;
                            d3_push(c, r[2]);
                            d3_push(c, ebx);
                            r[3] = ebx;
                            r[7] = edi;
                            D3_CALLD(0x10564220u, FN_10564220, 0x103114ceu);
                            ecx = rd32(r[4] + 0x34u);
                            r[4] += 0xcu;
                            ebx = r[3];
                            edi = r[7];
                        }
                        ecx -= 1u;
                        wr32(r[4] + 0x28u, ecx);
                    } else {
                        edi += 1u;
                        ebx += 8u;
                    }
                } while (edi < ecx);
                r[3] = ebx;
            }
            r[1] = ecx;
            r[7] = edi;
        }
        /* 103114e0 */
        const uint8_t s = (uint8_t)(rd8(r[4] + 0x26u) + 1u);
        r[0] = (r[0] & 0xffffff00u) | s;
        fl_sub(c, s, 4u, 8u);
        wr8(r[4] + 0x26u, s);
        if (s >= 4u)
            break;
    }
    r[0] = rd32(r[6] + 8u);
    r[2] = rd32(r[6] + 0x5cu);
    r[1] = rd32(r[0]);
    d3_push(c, r[2]);
    d3_push(c, r[0]);
    r[0] = rd32(r[1] + 0x1a0u);
    D3_CALLI(r[0], 0x10311504u); /* SetIndices(dev, ib) */
    r[7] = r[0];
    if ((int32_t)r[7] < 0) {
        fl_logic(c, r[7], 32u);
        ex_fail(c, 0x1031150eu);
        return;
    }
    const uint8_t mode2 = rd8(r[6] + 0x60u);
    fl_sub(c, mode2, 0u, 8u);
    r[0] = rd32(r[6] + 8u);
    if (mode2 != 0u) {
        r[1] = rd32(r[6] + 0x64u);
        r[2] = rd32(r[0]);
        r[2] = rd32(r[2] + 0x164u);
        d3_push(c, r[1]);
        d3_push(c, r[0]);
        D3_CALLI(r[2], 0x10311710u); /* SetFVF(dev, fvf) */
        r[7] = r[0];
        if ((int32_t)r[7] < 0) {
            fl_logic(c, r[7], 32u);
            ex_fail(c, 0x1031171au);
            return;
        }
        goto draw;
    }
    r[1] = rd32(r[0]);
    r[2] = rd32(r[1] + 0x164u);
    d3_push(c, 0u);
    d3_push(c, r[0]);
    D3_CALLI(r[2], 0x103117dau); /* SetFVF(dev, 0) */
    r[7] = r[0];
    if ((int32_t)r[7] < 0) {
        fl_logic(c, r[7], 32u);
        ex_fail(c, 0x1031171au);
        return;
    }
    /* 103117e4: cut the elements whose offset is not inside their stream's stride */
    {
        const uint32_t e0 = r[4];
        uint32_t edx = rd32(e0 + 0x28u), ebp = 0;
        if (edx != 0u) {
            uint32_t ebx = e0 + 0x42u, edi = e0 + 0x48u, eax = e0 + 0x50u, ecx;
            do {
                ecx = rd16(edi);
                edi += 8u;
                ebp += 1u;
                ebx += 8u;
                eax += 8u;
                if (ecx < 4u) {
                    edx = rd16(ebx);
                    ecx <<= 4;
                    if (edx < rd32(ecx + r[6] + 0x24u)) {
                        edx = rd32(r[4] + 0x28u);
                    } else {
                        ecx = rd32(r[4] + 0x28u);
                        ebp -= 1u;
                        ecx -= ebp;
                        edx = ecx * 8u - 8u;
                        eax -= 8u;
                        d3_push(c, edx);
                        d3_push(c, eax);
                        edi -= 8u;
                        d3_push(c, edi);
                        fl_sub(c, ebx, 8u, 32u);
                        ebx -= 8u;
                        wr32(r[4] + 0x20u, eax);
                        r[0] = eax;
                        r[1] = ecx;
                        r[2] = edx;
                        r[3] = ebx;
                        r[5] = ebp;
                        r[7] = edi;
                        D3_CALLD(0x10564220u, FN_10564220, 0x10311846u);
                        edx = rd32(r[4] + 0x34u);
                        eax = rd32(r[4] + 0x20u);
                        r[4] += 0xcu;
                        edx -= 1u;
                        wr32(r[4] + 0x28u, edx);
                        wr8(r[4] + 0x27u, 1u);
                        ecx = r[1];
                        ebx = r[3];
                        ebp = r[5];
                        edi = r[7];
                    }
                }
            } while (ebp < edx);
            r[0] = eax;
            r[1] = ecx;
            r[3] = ebx;
            r[7] = edi;
        }
        r[2] = edx;
        r[5] = ebp;
    }
    /* 10311867: which COLOR usage indices the declaration has */
    {
        const uint32_t e0 = r[4], n = r[2];
        uint32_t mask = 0, i = 0, ecx = r[1], ebp = r[5];
        for (; i < n; ++i)
            if (rd8(e0 + i * 8u + 0x4eu) == 0xau) {
                ecx = (ecx & 0xffffff00u) | rd8(e0 + i * 8u + 0x4fu);
                ebp = shl32(1u, ecx & 0xffu);
                mask |= ebp;
            }
        r[0] = i;
        r[1] = ecx;
        r[3] = 0;
        r[5] = ebp;
        r[7] = mask;
    }
    r[0] = r[7];
    r[0] = (r[0] & 0xffffff00u) | (r[0] & 3u);
    if ((uint8_t)r[0] == 2u) {
        fl_sub(c, 2u, 2u, 8u);
        if (ex_color(c))
            goto set_cut;
    }
    /* 10311a42 */
    {
        const uint8_t cut = rd8(r[4] + 0x27u);
        fl_sub(c, cut, 0u, 8u);
        if (cut == 0u) {
            r[0] = rd32(r[6] + 8u);
            r[2] = rd32(r[6] + 0x64u);
            r[1] = rd32(r[0]);
            d3_push(c, r[2]);
            d3_push(c, r[0]);
            r[0] = rd32(r[1] + 0x15cu);
            D3_CALLI(r[0], 0x10311ba3u); /* SetVertexDeclaration(dev, decl) */
            r[7] = r[0];
            fl_sub(c, r[7], r[3], 32u);
            if (c->eflags_sf != c->eflags_of) {
                ex_fail(c, 0x10311badu);
                return;
            }
            goto draw;
        }
    }
set_cut:
    /* 10311a4d: the engine's copy of the cut declaration */
    r[1] = rd32(r[6] + 4u);
    r[0] = r[4] + 0x1cu;
    d3_push(c, r[0]);
    wr32(r[4] + 0x20u, r[3]);
    r[2] = rd32(r[1]);
    r[2] = rd32(r[2] + 0x170u);
    r[0] = r[4] + 0x4cu;
    d3_push(c, r[0]);
    D3_CALLI(r[2], 0x10311a68u); /* wrapper->CreateVertexDeclaration(elems, &decl) */
    fl_sub(c, r[0], 1u, 32u);
    if (r[0] != 1u) {
        ex_out(c, 0);
        return;
    }
    r[0] = rd32(r[4] + 0x1cu);
    fl_sub(c, r[0], r[3], 32u);
    if (r[0] == r[3]) {
        ex_out(c, 0);
        return;
    }
    r[1] = rd32(r[6] + 4u);
    r[2] = rd32(r[1]);
    d3_push(c, r[0]);
    r[0] = rd32(r[2] + 0x17cu);
    D3_CALLI(r[0], 0x10311a8bu); /* wrapper->SetVertexDeclaration(decl) */
    r[1] = rd32(r[6] + 4u);
    r[2] = rd32(r[1]);
    r[2] = rd32(r[2] + 0x174u);
    r[7] = r[0];
    r[0] = rd32(r[4] + 0x1cu);
    d3_push(c, r[0]);
    D3_CALLI(r[2], 0x10311a9fu); /* wrapper->DestroyVertexDeclaration(decl) */
    fl_sub(c, r[7], 1u, 32u);
    if (r[7] != 1u) {
        ex_out(c, 0);
        return;
    }
draw: {
    /* 10311aa8 */
    const uint8_t indexed = rd8(r[6] + 0x68u);
    if (indexed == 1u) {
        r[2] = rd32(r[6] + 0x7cu);
        const uint32_t a = rd32(0x10862d20u);
        wr32(0x10862d20u, a + r[2]);
        fl_add32(c, a, r[2]);
        r[0] = rd32(r[6] + 0x70u);
        r[1] = rd32(0x10862d24u);
        r[2] = r[0] + r[1];
        r[0] = r[2] + r[0] * 2u;
        wr32(0x10862d24u, r[0]);
        r[2] = rd32(r[6] + 0x70u);
        r[0] = rd32(r[6] + 8u);
        r[1] = rd32(r[0]);
        d3_push(c, r[2]);
        r[2] = rd32(r[6] + 0x80u);
        d3_push(c, r[2]);
        r[2] = rd32(r[6] + 0x7cu);
        d3_push(c, r[2]);
        r[2] = rd32(r[6] + 0x78u);
        d3_push(c, r[2]);
        r[2] = rd32(r[6] + 0x84u);
        d3_push(c, r[2]);
        r[2] = rd32(r[6] + 0x6cu);
        d3_push(c, r[2]);
        d3_push(c, r[0]);
        r[0] = rd32(r[1] + 0x148u);
        D3_CALLI(r[0], 0x10311afbu); /* DrawIndexedPrimitive */
        r[6] = r[0];
        if ((int32_t)r[6] < 0) {
            fl_logic(c, r[6], 32u);
            ex_fail(c, 0x10311b05u);
            return;
        }
    } else {
        fl_sub(c, indexed, 1u, 8u);
        r[0] = rd32(r[6] + 0x70u);
        r[2] = rd32(0x10862d20u);
        r[1] = r[0] + r[2];
        r[2] = r[1] + r[0] * 2u;
        r[1] = rd32(0x10862d24u);
        wr32(0x10862d20u, r[2]);
        r[0] = rd32(r[6] + 0x70u);
        r[2] = r[0] + r[1];
        r[0] = r[2] + r[0] * 2u;
        wr32(0x10862d24u, r[0]);
        r[2] = rd32(r[6] + 0x70u);
        r[0] = rd32(r[6] + 8u);
        r[1] = rd32(r[0]);
        d3_push(c, r[2]);
        r[2] = rd32(r[6] + 0x74u);
        d3_push(c, r[2]);
        r[2] = rd32(r[6] + 0x6cu);
        d3_push(c, r[2]);
        d3_push(c, r[0]);
        r[0] = rd32(r[1] + 0x144u);
        D3_CALLI(r[0], 0x10311c69u); /* DrawPrimitive */
        r[6] = r[0];
        if ((int32_t)r[6] < 0) {
            fl_logic(c, r[6], 32u);
            ex_fail(c, 0x10311c73u);
            return;
        }
    }
    ex_out(c, 1);
}
}
