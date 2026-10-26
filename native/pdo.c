/* native/pdo.c - the particle system's processing (eCParticle_PS::OnProcess 103aed10 ->
 * DoProcessV2 103b52f0) and its depth sort, group "pdo" (G3OPT_PDO).
 *
 * docs/perf/particles-pdo.md has the profile this follows. The particle leaves TickParticles,
 * UpdateVertices and the scales are native already (native/particles.c, group "particles"); what
 * was left of the subsystem in the translation and hot was:
 *
 *  - Engine's own static CRT rand 10564592 (EmitParticles, 15-20 calls per new particle). It reads
 *    its seed through Engine's _getptd 1056bee7 -> _getptd_noexit 1056be70, three import shims
 *    (GetLastError, TlsGetValue for the decoded FlsGetValue, SetLastError) and the CRT's pointer
 *    decoding. In the combat profile (work/sample-ng2-a.txt) 64 of the 65 samples of _rand were
 *    in _getptd, 49 of them waiting for the baton the TLS shim yields. native/rand.c does the same
 *    for SharedBase's copy of the CRT; this is Engine's copy, a separate per-thread block found
 *    through Engine's own __flsindex 10835690, so the sequence is the one Engine's rand always
 *    had, per guest thread, shared with srand and everything else that reads the field.
 *  - The depth sort's comparator 103af9c0 under Engine's qsort (native/shadow.c), called once per
 *    comparison through the guest and itself three SharedBase calls per particle. It runs inline
 *    in the native qsort now (g3n_pdo_cmp_depth below), ending in the state the translation leaves,
 *    with one key per element for the whole sort.
 *  - CalcExtents 103afae0 (the particles' box, under the entity's UpdateLocalBoundary), its
 *    particle loop of SharedBase leaves done inline.
 *  - eCVectorScale::CalcValue 103c00c0, CalcVelocityFactor 103af640 and CalcRevolutionFactor
 *    103af6b0, thin translated wrappers TickParticles calls per particle on the way to the native
 *    CalcValueLinear.
 *  - PhysXCore's ray against its box trees, the particles' collision ray (63% of TickParticles):
 *    the walks 11289670 and 11289e10 and the per-node leaves 1122a320, 11163d50, 1117a160,
 *    1118b390, 1118b3d0. Every raycast on the main thread goes through them.
 * DoProcessV2 103b52f0, ProcessFrame 103b5570 and ProcessTicks 103b5610 stay translated: two
 * samples of their own each in the profile.
 *
 * Each g3n_<addr> runs its native version only under G3OPT(PDO); otherwise it calls the
 * original. native/tests/pdo_test.c compares each with the translation, registers, x87 state,
 * XMM and memory, bit for bit. */
#include "chunk.h"
#include "g3_opt.h"

#include <math.h>
#include <stdio.h>

void fn_10564592(X86 *c); /* Engine _rand */
void fn_1056bee7(X86 *c); /* Engine __getptd */
void fn_1056be70(X86 *c); /* Engine _getptd_noexit */

/* ------------------------------------------------------------------ Engine's CRT rand */

#define ENG_FLSINDEX 0x10835690u        /* Engine's __flsindex, pushed raw to "FlsGetValue" */
#define TEB_TLS_ARRAY 0x1000u           /* the kit's TLS slot array, after the TEB page */
#define TLS_SLOTS_KIT 64u               /* kit/runtime/guest.h TLS_SLOTS */
#define ENG_RAND_GETPTD_RETURN 0x10564597u /* _rand's call-return site after CALL __getptd */

/* 0: not yet checked, 1: the direct TLS read matched Engine's _getptd, -1: it did not. Guest
 * threads run one at a time under the baton, so a plain int is enough. */
int g3n_pdo_ptd_direct; /* extern for native/tests/pdo_test.c, which resets it */

static inline uint32_t eng_ptd_direct(const X86 *c) {
    const uint32_t idx = rd32(ENG_FLSINDEX);
    if (idx >= TLS_SLOTS_KIT)
        return 0;
    return rd32(c->fs_base + TEB_TLS_ARRAY + 4u * idx);
}

static void eng_ptd_check(const X86 *c, uint32_t ptd) {
    if (g3n_pdo_ptd_direct == 0 && ptd != 0) {
        g3n_pdo_ptd_direct = eng_ptd_direct(c) == ptd ? 1 : -1;
        if (g3n_pdo_ptd_direct < 0)
            fprintf(stderr, "g3n pdo: Engine TLS slot %08x != _getptd %08x, native ptd read off\n",
                    eng_ptd_direct(c), ptd);
    }
}

/* The original __getptd, entered as _rand enters it. Leaves EAX = ptd and whatever ECX/EDX the
 * original call leaves. */
static __attribute__((noinline)) uint32_t eng_ptd_slow(X86 *c) {
    c->r[4] -= 4;
    wr32(c->r[4], ENG_RAND_GETPTD_RETURN);
    fn_1056bee7(c);
    const uint32_t ptd = c->r[0];
    eng_ptd_check(c, ptd);
    return ptd;
}

/* int __cdecl rand(void): holdrand (ptd+0x14) = holdrand * 214013 + 2531011, ECX = holdrand,
 * EAX = (holdrand >> 16) & 0x7fff. */
void g3n_10564592(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_10564592(c);
        return;
    }
    uint32_t ptd = 0;
    if (__builtin_expect(g3n_pdo_ptd_direct > 0, 1))
        ptd = eng_ptd_direct(c);
    if (__builtin_expect(ptd == 0, 0))
        ptd = eng_ptd_slow(c);
    const uint32_t s = rd32(ptd + 0x14u) * 0x343fdu + 0x269ec3u;
    wr32(ptd + 0x14u, s);
    c->r[1] = s;
    c->r[0] = (s >> 16) & 0x7fffu;
    /* AND EAX,0x7fff leaves these; nothing reads them after a call, set for parity. */
    c->eflags_cf = 0;
    c->eflags_of = 0;
    c->eflags_zf = c->r[0] == 0;
    c->eflags_sf = 0;
    c->eflags_pf = parity8(c->r[0]);
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u;
    recomp_return(c);
}

/* With the direct read checked and the slot set, EAX = ptd and nothing else changes: the
 * original leaves ESI/EDI as they were and LastError as it was (it saves and restores it); ECX
 * and EDX are what the import shims leave, which no caller relies on after a call. */
static inline int eng_ptd_ret_direct(X86 *c) {
    if (__builtin_expect(g3n_pdo_ptd_direct > 0, 1)) {
        const uint32_t ptd = eng_ptd_direct(c);
        if (__builtin_expect(ptd != 0, 1)) {
            c->r[0] = ptd;
            c->eip = rd32(c->r[4]);
            c->r[4] += 4u;
            recomp_return(c);
            return 1;
        }
    }
    return 0;
}

/* _ptd *__cdecl _getptd(void): its original calls _getptd_noexit through g3n_1056be70, which
 * does the check. */
void g3n_1056bee7(X86 *c) {
    if (G3OPT(PDO) && eng_ptd_ret_direct(c))
        return;
    fn_1056bee7(c);
}

/* _ptd *__cdecl _getptd_noexit(void) */
void g3n_1056be70(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_1056be70(c);
        return;
    }
    if (eng_ptd_ret_direct(c))
        return;
    fn_1056be70(c);
    eng_ptd_check(c, c->r[0]);
}

/* ------------------------------------------------------------------ the depth comparator */

#include <stdlib.h>

#define ENG_DEPTH_EYE 0x10868b6cu /* the camera position DepthSortParticles 103afa70 stores */

/* int FUN_103af9c0(eCParticle2 const *a, eCParticle2 const *b), cdecl, the depth sort's
 * comparator: d(p) = |eye - p.position (+0xc)|^2, -1 when d(b) < d(a), 1 when d(a) < d(b), else 0
 * (far to near). The original builds eye - p in a stack vector with SharedBase's SetVector
 * (100138d0, an FLD/FSTP copy; for b a MOVSS copy) and InvTranslate (1001ae70), squares it with
 * GetSquareMagnitude (10010e20) and stores the result to a float; native/vector.c has those three
 * step for step and depth_key is the same arithmetic: each difference fx87 then a float store, the
 * sum as (y*y + x*x) + z*z, each fx87, stored to a float. The FLD/FSTP copy and the MOVSS copy of
 * the eye are the same float (only a signalling NaN would differ, and a NaN eye makes every
 * difference the x87 indefinite either way), so d(p) does not depend on which side p is on.
 *
 * d(p) depends only on the eye, p's position and the control word, none of which changes while
 * qsort runs (it moves whole elements, and a key moves with its element). So native/shadow.c's
 * qsort, sorting with this comparator under G3OPT(PDO), brackets the sort with
 * g3n_pdo_sort_begin / _end and tells g3n_pdo_sort_swap about every element swap, and each key
 * is computed once, the first time its element is compared; every element of a sort of two or
 * more is compared at least once, so IE (sticky) is raised from the same keys as in the
 * original. g3n_pdo_cmp_depth then ends in the state the translation leaves: EAX the result, ECX
 * the bits of d(b) (GetSquareMagnitude's POP ECX), EDX untouched, XMM0 the eye's z loaded by
 * MOVSS (upper lanes zero), the x87 top as on entry with ST(-1)..ST(-4) empty (GetSquareMagnitude's
 * four pushes), IE from FCOMI on a NaN, EFLAGS of the last FCOMI. The stack vector and the
 * argument slots are below the qsort's ESP, dead once it returns. */
static inline float depth_key(X86 *c, uint32_t p) {
    const double vx = fto_float(c, fx87(c, (double)rdf32(ENG_DEPTH_EYE) - (double)rdf32(p)));
    const double vy = fto_float(c, fx87(c, (double)rdf32(ENG_DEPTH_EYE + 4u) - (double)rdf32(p + 4u)));
    const double vz = fto_float(c, fx87(c, (double)rdf32(ENG_DEPTH_EYE + 8u) - (double)rdf32(p + 8u)));
    const double xx = fx87(c, vx * vx);
    const double yy = fx87(c, vy * vy);
    return fto_float(c, fx87(c, fx87(c, yy + xx) + fx87(c, vz * vz)));
}

/* The sort in progress (one at a time: the comparator makes no guest calls, so no qsort nests
 * inside it, and guest threads run one at a time). key[i] belongs to the element now at
 * base + i * width; have[i] says it has been computed. */
static struct {
    int on;
    uint32_t base, width, num, cap;
    float *key;
    uint8_t *have;
    int compared;      /* a comparison ran: its x87 and XMM end state is still to be written */
    float last_a, last_b;
} sort_keys;

/* The comparator's SharedBase imports, which depth_key computes in place (exec.c had the check
 * first): a mod hooking one of them gets the guest comparator. */
#define IAT_DEPTH_SETVECTOR 0x105f7158u /* ?SetVector@bCVector@@QAEXABV1@@Z */
#define IAT_DEPTH_INVTRANSLATE 0x105f66ecu
#define IAT_DEPTH_SQMAG 0x105f677cu

/* Returns 1 when the sort uses the inline comparator: pdo or exec on (exec had it first, as a
 * comparison at a time; the two are one now) and the imports the SharedBase functions. */
int g3n_pdo_sort_begin(uint32_t base, uint32_t num, uint32_t width) {
    sort_keys.on = 0;
    if (!(G3OPT(PDO) || G3OPT(EXEC)) || rd32(IAT_DEPTH_SETVECTOR) != 0x100138d0u ||
        rd32(IAT_DEPTH_INVTRANSLATE) != 0x1001ae70u || rd32(IAT_DEPTH_SQMAG) != 0x10010e20u)
        return 0;
    if (num < 2u || width == 0u)
        return 1;
    if (num > sort_keys.cap) {
        float *k = realloc(sort_keys.key, (size_t)num * sizeof *k);
        uint8_t *h = k ? realloc(sort_keys.have, num) : NULL;
        if (k)
            sort_keys.key = k;
        if (!h)
            return 1; /* every comparison computes both keys then */
        sort_keys.have = h;
        sort_keys.cap = num;
    }
    memset(sort_keys.have, 0, num);
    sort_keys.base = base;
    sort_keys.width = width;
    sort_keys.num = num;
    sort_keys.compared = 0;
    sort_keys.on = 1;
    return 1;
}


static inline uint32_t key_index(uint32_t a) {
    return (a - sort_keys.base) / sort_keys.width;
}

/* Elements at a and b (distinct, from the array being sorted) were swapped. */
void g3n_pdo_sort_swap(uint32_t a, uint32_t b) {
    if (!sort_keys.on)
        return;
    const uint32_t i = key_index(a), j = key_index(b);
    const float k = sort_keys.key[i];
    const uint8_t h = sort_keys.have[i];
    sort_keys.key[i] = sort_keys.key[j];
    sort_keys.have[i] = sort_keys.have[j];
    sort_keys.key[j] = k;
    sort_keys.have[j] = h;
}

static inline float key_of(X86 *c, uint32_t elem) {
    if (sort_keys.on) {
        const uint32_t i = key_index(elem);
        if (__builtin_expect(i < sort_keys.num && sort_keys.base + i * sort_keys.width == elem, 1)) {
            if (!sort_keys.have[i]) {
                sort_keys.key[i] = depth_key(c, elem + 0xcu);
                sort_keys.have[i] = 1;
            }
            return sort_keys.key[i];
        }
    }
    return depth_key(c, elem + 0xcu);
}

/* What the comparator leaves besides EAX and ECX, for the comparison of keys a and b: XMM0, the
 * four x87 slots below TOP empty (two of them holding the keys FCOMI compared), EFLAGS of the last
 * FCOMI. Within a sort only the last comparison's survives (qsort itself touches neither the x87
 * nor XMM0, and the comparisons in between make no guest call), so g3n_pdo_sort_end writes it once. */
static void cmp_end_state(X86 *c, double a, double b) {
    c->xmm[0][0] = rd32(ENG_DEPTH_EYE + 8u);
    c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0u;
    for (unsigned i = 1; i <= 4; ++i) {
        const unsigned ph = (c->fpu_top - i) & 7u;
        ftag_put(c, ph, FTAG_EMPTY);
        c->st_exact[ph] = 0;
    }
    /* FLD d(b) / FLD d(a) / FCOMI: ST(-1) = d(b), ST(-2) = d(a); after the FXCH the other way. */
    const unsigned p1 = (c->fpu_top - 1u) & 7u, p2 = (c->fpu_top - 2u) & 7u;
    c->eflags_of = c->eflags_sf = c->eflags_af = 0;
    if (a > b) {
        c->st[p1] = b;
        c->st[p2] = a;
        c->eflags_pf = c->eflags_zf = c->eflags_cf = 0;
        return;
    }
    c->st[p1] = a;
    c->st[p2] = b;
    if (a != a || b != b)
        c->eflags_pf = c->eflags_zf = c->eflags_cf = 1;
    else {
        c->eflags_pf = c->eflags_cf = 0;
        c->eflags_zf = !(b > a);
    }
}

void g3n_pdo_sort_end(X86 *c) {
    if (sort_keys.on && sort_keys.compared)
        cmp_end_state(c, sort_keys.last_a, sort_keys.last_b);
    sort_keys.on = 0;
}

uint32_t g3n_pdo_cmp_depth(X86 *c, uint32_t first, uint32_t second, uint32_t *ecx) {
    const float fa = key_of(c, first), fb = key_of(c, second);
    uint32_t bits;
    memcpy(&bits, &fb, 4);
    *ecx = bits;
    if (sort_keys.on) {
        sort_keys.compared = 1;
        sort_keys.last_a = fa;
        sort_keys.last_b = fb;
    } else
        cmp_end_state(c, fa, fb);
    if (fa > fb)
        return 0xffffffffu;
    if (fa != fa || fb != fb) {
        c->fpu_sw |= 0x0001u; /* FCOMI on a NaN */
        return 0u;
    }
    return fb > fa ? 1u : 0u;
}

/* ------------------------------------------------------------------ CalcExtents */

void fn_103afae0(X86 *c); /* Engine eCParticle_PS::CalcExtents */

/* SharedBase imports (Engine's IAT) CalcExtents calls outside its particle loop. */
#define IAT_BOX_INVALIDATE 0x105f66a8u  /* bCBox::Invalidate() */
#define IAT_BOX_ISVALID 0x105f66b4u     /* bCBox::IsValid() const, AL */
#define IAT_SETCOORDS 0x105f649cu       /* bCVector::SetCoordinates(x, y, z), RET 0xc */
#define IAT_SETVECTOR 0x105f7158u       /* bCVector::SetVector(bCVector const &), RET 4 */
#define IAT_MATRIX_INVERTED 0x105f68a8u /* bCMatrix::GetInverted(bCMatrix &) const, RET 4 */
#define IAT_BOX_TRANSFORM 0x105f6a64u   /* bCBox::Transform(bCMatrix const &), RET 4 */

static uint64_t ic_ce_invalidate, ic_ce_isvalid, ic_ce_setcoords, ic_ce_setvec1, ic_ce_setvec2, ic_ce_vt,
    ic_ce_inverted, ic_ce_transform;

/* CALL dword ptr [slot] / CALL reg, returning to `ret`. */
static inline void ce_call(X86 *c, uint32_t target, uint64_t *ic, uint32_t ret) {
    c->r[4] -= 4u;
    wr32(c->r[4], ret);
    recomp_call_cached(c, target, ic);
}

static inline void ce_push(X86 *c, uint32_t v) {
    c->r[4] -= 4u;
    wr32(c->r[4], v);
}

/* FCOMI/FCOMIP a, b: IE on any NaN; returns a > b (JA / not JBE). */
static inline int ce_fgt(X86 *c, double a, double b) {
    if (a != a || b != b)
        c->fpu_sw |= 0x0001u;
    return a > b;
}

/* SharedBase GetSquareMagnitude 10010e20 as native/vector.c has it: the float result. */
static inline float ce_sqmag(X86 *c, uint32_t v) {
    const double y = (double)rdf32(v + 4u), x = (double)rdf32(v), z = (double)rdf32(v + 8u);
    const double xx = fx87(c, x * x);
    const double yy = fx87(c, y * y);
    return fto_float(c, fx87(c, fx87(c, yy + xx) + fx87(c, z * z)));
}

/* FLDZ / FUCOMIP ST0,ST1 / FSTP ST0 / LAHF / TEST AH,0x44 / JNP: AH = ZF PF CF of 0 against f
 * (FUCOMI clears SF and AF; bit 1 reads 1); taken (the particle skipped) when f == 0. f comes
 * from fx87, so it is never a signalling NaN and FUCOMI raises nothing. */
static inline int ce_zero_test(X86 *c, float f) {
    const double d = f;
    uint32_t ah;
    if (d != d)
        ah = 0x47u;
    else
        ah = (d == 0.0 ? 0x40u : 0u) | (0.0 < d ? 1u : 0u) | 0x02u;
    c->r[0] = (c->r[0] & 0xffff00ffu) | (ah << 8);
    return d == 0.0;
}

/* SharedBase bCBox::Merge(bCVector const &p) 1001b9e0 as native/vector.c has it, with p's three
 * floats already read: max first, then min, a store through XMM0 (MOVSS) where p wins. */
static inline void ce_merge(X86 *c, uint32_t box, const float p[3]) {
    for (uint32_t i = 0; i < 3u; ++i) {
        const float pf = fto_float(c, (double)p[i]);
        if (ce_fgt(c, (double)pf, (double)rdf32(box + 0xcu + 4u * i))) {
            uint32_t b;
            memcpy(&b, &pf, 4);
            c->xmm[0][0] = b;
            c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0u;
            wrf32(box + 0xcu + 4u * i, pf);
        }
    }
    for (uint32_t i = 0; i < 3u; ++i) {
        const float pf = fto_float(c, (double)p[i]);
        if (ce_fgt(c, (double)rdf32(box + 4u * i), (double)pf)) {
            uint32_t b;
            memcpy(&b, &pf, 4);
            c->xmm[0][0] = b;
            c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0u;
            wrf32(box + 4u * i, pf);
        }
    }
}

/* void eCParticle_PS::CalcExtents(bCBox &box) const, RET 4: the box around every live, visible
 * particle with a size. box.Invalidate(), then for each particle p (0xb4 bytes from this+0x318,
 * this+0x31c of them) with p+0xa0 <= p+0xa4 (age within life), bit 0 of p+0xb0 clear,
 * 0 < p+0x88, 0 < p+0x9c and both vectors p+0x60 (the extent) and p+0x6c of non-zero square
 * magnitude: box.Merge(pos - ext), box.Merge(ext + pos) with pos = p+0xc, counted in this+0x394.
 * Then, if the box is valid and this+0x84 == 0, box.Transform(the inverse of the matrix at +0x40
 * of what vtable +0x6c returns); if it is not valid, both corners (0, 0, 0). A master instance
 * (this+0x22d == 1) copies its master's box instead.
 *
 * The particle loop is native: the original's calls in it (GetSquareMagnitude twice,
 * GetInvTranslated, Merge, GetTranslated, Merge) are SharedBase leaves, done here with the same
 * arithmetic (native/vector.c and leaf.c have them step for step, tested against the
 * translation), leaving EAX..EDX, EBX, ESI, XMM0, the x87 tags and IE as the loop leaves them.
 * The calls before and after it (Invalidate, IsValid, SetCoordinates, SetVector, the vtable call,
 * GetInverted, Transform) go to the guest with the original's frame, registers and return
 * addresses; the master copy runs the original. The loop counters and the two corner vectors in
 * the frame are not written: they are below the final ESP, and no guest call reads them (the cold
 * path builds its corner with its own SetCoordinates). */
void g3n_103afae0(X86 *c) {
    if (!G3OPT(PDO) || rd8(c->r[1] + 0x22du) == 1u) {
        fn_103afae0(c);
        return;
    }
    const uint32_t e = c->r[4];
    /* PUSH EBP / MOV EBP,ESP / AND ESP,-8 / SUB ESP,0x64 / PUSH EBX / PUSH ESI / PUSH EDI */
    const uint32_t ebp = e - 4u;
    wr32(ebp, c->r[5]);
    c->r[5] = ebp;
    const uint32_t sp = ((ebp & 0xfffffff8u) - 0x64u) - 12u;
    wr32(sp + 8u, c->r[3]);
    wr32(sp + 4u, c->r[6]);
    wr32(sp, c->r[7]);
    c->r[4] = sp;
    const uint32_t self = c->r[1];
    c->r[7] = self;
    const uint32_t box = rd32(ebp + 8u);
    c->r[6] = box;
    c->r[1] = box;
    ce_call(c, rd32(IAT_BOX_INVALIDATE), &ic_ce_invalidate, 0x103afb46u);

    const int32_t count = (int32_t)rd32(self + 0x31cu);
    c->r[0] = (uint32_t)count;
    c->r[1] = 0u;
    wr32(self + 0x394u, 0u);
    if (count > 0) {
        unsigned depth = 0; /* x87 slots below TOP the loop pushed into */
        uint32_t off = 0;
        for (int32_t left = count; left != 0; --left, off += 0xb4u) {
            const uint32_t p = rd32(self + 0x318u) + off;
            c->r[6] = p;
            c->xmm[0][0] = c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0u; /* XORPS XMM0,XMM0 */
            if (depth < 2u)
                depth = 2u;
            if (ce_fgt(c, (double)rdf32(p + 0xa0u), (double)rdf32(p + 0xa4u)))
                continue;
            c->r[1] = rd32(p + 0xb0u) & 1u;
            if (c->r[1] == 1u)
                continue;
            /* COMISS XMM0(0), v / JNC: on when 0 < v or unordered */
            const float v88 = rdf32(p + 0x88u), v9c = rdf32(p + 0x9cu);
            if (!(v88 != v88 || 0.0f < v88))
                continue;
            if (!(v9c != v9c || 0.0f < v9c))
                continue;
            const uint32_t ext = p + 0x60u;
            c->r[3] = ext;
            depth = 4u;
            float f = ce_sqmag(c, ext);
            memcpy(&c->r[1], &f, 4); /* GetSquareMagnitude's POP ECX */
            if (ce_zero_test(c, f))
                continue;
            f = ce_sqmag(c, p + 0x6cu);
            memcpy(&c->r[1], &f, 4);
            if (ce_zero_test(c, f))
                continue;
            const uint32_t pos = p + 0xcu;
            c->r[6] = pos;
            /* GetInvTranslated(&lo, ext): lo = pos - ext (FLD pos / FSUB ext) */
            float lo[3], hi[3];
            for (uint32_t i = 0; i < 3u; ++i)
                lo[i] = fto_float(c, fx87(c, (double)rdf32(pos + 4u * i) - (double)rdf32(ext + 4u * i)));
            ce_merge(c, box, lo);
            /* GetTranslated(&hi, ext): hi = ext + pos (FLD ext / FADD pos) */
            for (uint32_t i = 0; i < 3u; ++i)
                hi[i] = fto_float(c, fx87(c, (double)rdf32(ext + 4u * i) + (double)rdf32(pos + 4u * i)));
            ce_merge(c, box, hi);
            c->r[0] = sp + 0x24u; /* GetTranslated's out */
            c->r[2] = sp + 0x24u; /* the second Merge's argument, which it leaves in EDX */
            c->r[1] = box;
            wr32(self + 0x394u, rd32(self + 0x394u) + 1u);
        }
        for (unsigned i = 1; i <= depth; ++i) {
            const unsigned ph = (c->fpu_top - i) & 7u;
            ftag_put(c, ph, FTAG_EMPTY);
            c->st_exact[ph] = 0;
        }
        c->r[6] = box;
    }
    c->r[1] = box;
    ce_call(c, rd32(IAT_BOX_ISVALID), &ic_ce_isvalid, 0x103afc43u);
    if ((c->r[0] & 0xffu) == 0u) {
        /* FLDZ / SUB ESP,0xc / FST, FST, FSTP: SetCoordinates(0, 0, 0) into the corner at
         * ESP+0x18, then both corners from it */
        const unsigned ph = (c->fpu_top - 1u) & 7u;
        ftag_put(c, ph, FTAG_EMPTY);
        c->st_exact[ph] = 0;
        c->r[4] -= 0xcu;
        wr32(c->r[4] + 8u, 0u);
        wr32(c->r[4] + 4u, 0u);
        wr32(c->r[4], 0u);
        c->r[1] = c->r[4] + 0x24u;
        ce_call(c, rd32(IAT_SETCOORDS), &ic_ce_setcoords, 0x103afc61u);
        c->r[0] = c->r[4] + 0x18u;
        ce_push(c, c->r[0]);
        c->r[1] = c->r[6];
        ce_call(c, rd32(IAT_SETVECTOR), &ic_ce_setvec1, 0x103afc6eu);
        c->r[1] = c->r[4] + 0x18u;
        ce_push(c, c->r[1]);
        c->r[1] = c->r[6] + 0xcu;
        ce_call(c, rd32(IAT_SETVECTOR), &ic_ce_setvec2, 0x103afc7cu);
    } else if (rd32(c->r[7] + 0x84u) == 0u) {
        c->r[2] = rd32(c->r[7]);
        c->r[0] = rd32(c->r[2] + 0x6cu);
        c->r[1] = c->r[7];
        ce_call(c, c->r[0], &ic_ce_vt, 0x103afc97u);
        c->r[1] = c->r[4] + 0x30u;
        ce_push(c, c->r[1]);
        c->r[1] = c->r[0] + 0x40u;
        ce_call(c, rd32(IAT_MATRIX_INVERTED), &ic_ce_inverted, 0x103afca5u);
        c->r[2] = c->r[4] + 0x30u;
        ce_push(c, c->r[2]);
        c->r[1] = c->r[6];
        ce_call(c, rd32(IAT_BOX_TRANSFORM), &ic_ce_transform, 0x103afcb2u);
    }
    /* POP EDI / POP ESI / POP EBX / MOV ESP,EBP / POP EBP / RET 4 */
    c->r[7] = rd32(c->r[4]);
    c->r[6] = rd32(c->r[4] + 4u);
    c->r[3] = rd32(c->r[4] + 8u);
    c->r[4] = c->r[5];
    c->r[5] = rd32(c->r[4]);
    c->r[4] += 4u;
    c->eip = rd32(c->r[4]);
    c->r[4] += 8u;
    recomp_return(c);
}

/* ------------------------------------------------------------------ the vector scales TickParticles reads */

void fn_103c00c0(X86 *c); /* Engine eCVectorScale::CalcValue */
void fn_103af640(X86 *c); /* Engine eCParticle_PS::CalcVelocityFactor */
void fn_103af6b0(X86 *c); /* Engine eCParticle_PS::CalcRevolutionFactor */
void fn_103c02b0(X86 *c); /* Engine eCVectorScale::CalcValueBinary */
void fn_103c0140(X86 *c); /* Engine eCVectorScale::CalcValueLinear (native in particles.c) */
#ifndef FN_103c02b0
#define FN_103c02b0 fn_103c02b0
#endif
#ifndef FN_103c0140
#define FN_103c0140 fn_103c0140
#endif
#ifndef FN_103c00c0
#define FN_103c00c0 fn_103c00c0
#endif

#define ENG_ONE_F 0x1068612cu /* float 1.0f */
#define ENG_ONE_D 0x10686158u /* double 1.0 */

/* FLD float [a] with the widening kept (particles.c fld32): clang would fold (float)(double)x back
 * to x, and an FLD/FSTP pair quiets a signalling NaN. */
static inline double vs_fld32(uint32_t a) {
    double d = (double)rdf32(a);
#if defined(__aarch64__)
    __asm__("" : "+w"(d));
#else
    __asm__("" : "+m"(d));
#endif
    return d;
}

static inline void vs_free1(X86 *c) {
    const unsigned ph = (c->fpu_top - 1u) & 7u;
    ftag_put(c, ph, FTAG_EMPTY);
    c->st_exact[ph] = 0;
}

/* MOVSS XMM0,[1.0f] and three MOVSS stores: out = (1, 1, 1). */
static inline void vs_ones(X86 *c, uint32_t out) {
    const uint32_t one = rd32(ENG_ONE_F);
    c->xmm[0][0] = one;
    c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0u;
    wr32(out, one);
    wr32(out + 4u, one);
    wr32(out + 8u, one);
}

/* void eCVectorScale::CalcValue(float t, bCVector &out) const, RET 8: this = {key *keys, int
 * count}. No keys: out = (1, 1, 1); one: out = its value (FLD/FSTP); two to seven (or a negative
 * count): CalcValueLinear 103c0140, eight or more: CalcValueBinary 103c02b0, each called with t
 * (FLD/FSTP into the slot PUSH ECX made) and out, from the original's frame, and the RET 8 after
 * it. EAX and EDX as the original leaves them before the call. */
void g3n_103c00c0(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_103c00c0(c);
        return;
    }
    const uint32_t e = c->r[4], self = c->r[1];
    const int32_t count = (int32_t)rd32(self + 4u);
    c->r[2] = (uint32_t)count;
    if (count == 0) {
        const uint32_t out = rd32(e + 8u);
        c->r[0] = out;
        vs_ones(c, out);
    } else if (count == 1) {
        const uint32_t k = rd32(self) + 4u;
        const uint32_t out = rd32(e + 8u);
        c->r[0] = k;
        c->r[1] = out;
        wrf32(out, fto_float(c, vs_fld32(k)));
        wrf32(out + 4u, fto_float(c, vs_fld32(k + 4u)));
        wrf32(out + 8u, fto_float(c, vs_fld32(k + 8u)));
        vs_free1(c);
    } else {
        const uint32_t out = rd32(e + 8u);
        const float t = fto_float(c, vs_fld32(e + 4u));
        vs_free1(c);
        if (count >= 8)
            c->r[0] = out;
        else {
            c->r[0] = (uint32_t)(count - 1);
            c->r[2] = out;
        }
        wr32(e - 4u, out);
        wrf32(e - 8u, t);
        wr32(e - 12u, count >= 8 ? 0x103c00e6u : 0x103c00f7u);
        c->r[4] = e - 12u;
        if (count >= 8)
            FN_103c02b0(c);
        else
            FN_103c0140(c);
    }
    c->eip = rd32(c->r[4]);
    c->r[4] += 12u;
    recomp_return(c);
}

/* CalcVelocityFactor 103af640 / CalcRevolutionFactor 103af6b0 (void (float t, bCVector &out)
 * const, RET 8): with the flag at this+flag clear, out = (1, 1, 1); else the scale at this+scale
 * read at frac(t * (speed + 1)), speed at this+flag+4: FLD speed / FADD 1.0 (double) / FMUL t /
 * FSTP into t's slot / CVTTSS2SI / FISUB / FSTP into the slot again, then FLD/FSTP into the slot
 * PUSH ECX made, and CALL CalcValue with the return address `ret`. The truncation is the
 * translation's own conversion (a C cast of the float, as the generated code has it). */
static void velocity_factor(X86 *c, uint32_t flag, uint32_t scale, uint32_t ret) {
    const uint32_t e = c->r[4], self = c->r[1];
    const uint32_t out = rd32(e + 8u);
    c->r[0] = out;
    if (rd8(self + flag) == 0) {
        vs_ones(c, out);
        c->eip = rd32(e);
        c->r[4] = e + 12u;
        recomp_return(c);
        return;
    }
    double v = fx87(c, vs_fld32(self + flag + 4u) + rdf64(ENG_ONE_D));
    wr32(e - 4u, out);
    wr32(e - 8u, self);
    c->r[1] = self + scale;
    v = fx87(c, v * (double)rdf32(e + 4u));
    wrf32(e + 4u, fto_float(c, v));
    const double w = vs_fld32(e + 4u);
    c->r[2] = (uint32_t)(int32_t)rdf32(e + 4u);
    wr32(e + 4u, c->r[2]);
    const double fr = fx87(c, w - (double)(int32_t)rd32(e + 4u));
    wrf32(e + 4u, fto_float(c, fr));
    wrf32(e - 8u, fto_float(c, vs_fld32(e + 4u)));
    vs_free1(c);
    wr32(e - 12u, ret);
    c->r[4] = e - 12u;
    FN_103c00c0(c);
    c->eip = rd32(c->r[4]);
    c->r[4] += 12u;
    recomp_return(c);
}

void g3n_103af640(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_103af640(c);
        return;
    }
    velocity_factor(c, 0xd0u, 0x3c8u, 0x103af68bu);
}

void g3n_103af6b0(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_103af6b0(c);
        return;
    }
    velocity_factor(c, 0x194u, 0x3bcu, 0x103af6fbu);
}

/* ------------------------------------------------------------------ PhysX's ray against its box tree */

/* The particles' collision ray (TickParticles -> TraceRayNearestHit 103cd980 -> TraceRayAllHits
 * 103ce850 -> PhysX raycastAllShapes 111d7480) spends most of its time in PhysXCore's box-tree walk
 * (FUN_11289670, FUN_11289e10, FUN_11288f10) and the three-float leaves it calls for every node:
 * the node's centre and half extents (1118b390, 1118b3d0) built from a vector add (1122a320), a
 * subtract (11163d50) and a scale (1117a160). These are the leaves, natively; each FADD/FSUB/FMUL
 * through fx87 and each FSTP through fto_float as the translation does, every read before the
 * first store as in the original (the x87 holds all three results until the stores). */

void fn_1122a320(X86 *c); /* PhysXCore: out = b + this, thiscall (out, b), RET 8 */
void fn_11163d50(X86 *c); /* PhysXCore: out = this - b, thiscall (out, b), RET 8 */
void fn_1117a160(X86 *c); /* PhysXCore: out = this * s, thiscall (out, float s), RET 8 */
void fn_1118b390(X86 *c); /* PhysXCore: box centre, thiscall (out), RET 4 */
void fn_1118b3d0(X86 *c); /* PhysXCore: box half extents, thiscall (out), RET 4 */

/* The three slots below TOP the original pushed into and popped. */
static inline void px_free3(X86 *c) {
    for (unsigned i = 1; i <= 3; ++i) {
        const unsigned ph = (c->fpu_top - i) & 7u;
        ftag_put(c, ph, FTAG_EMPTY);
        c->st_exact[ph] = 0;
    }
}

static inline void px_ret(X86 *c, uint32_t args) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + args;
    recomp_return(c);
}

static inline void px_store3(X86 *c, uint32_t out, double x, double y, double z) {
    wrf32(out, fto_float(c, x));
    wrf32(out + 4u, fto_float(c, y));
    wrf32(out + 8u, fto_float(c, z));
}

void g3n_1122a320(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_1122a320(c);
        return;
    }
    const uint32_t a = c->r[1], b = rd32(c->r[4] + 8u), out = rd32(c->r[4] + 4u);
    const double z = fx87(c, (double)rdf32(b + 8u) + (double)rdf32(a + 8u));
    const double y = fx87(c, (double)rdf32(b + 4u) + (double)rdf32(a + 4u));
    const double x = fx87(c, (double)rdf32(b) + (double)rdf32(a));
    c->r[0] = out;
    px_store3(c, out, x, y, z);
    px_free3(c);
    px_ret(c, 8u);
}

void g3n_11163d50(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_11163d50(c);
        return;
    }
    const uint32_t a = c->r[1], b = rd32(c->r[4] + 8u), out = rd32(c->r[4] + 4u);
    const double z = fx87(c, (double)rdf32(a + 8u) - (double)rdf32(b + 8u));
    const double y = fx87(c, (double)rdf32(a + 4u) - (double)rdf32(b + 4u));
    const double x = fx87(c, (double)rdf32(a) - (double)rdf32(b));
    c->r[0] = out;
    px_store3(c, out, x, y, z);
    px_free3(c);
    px_ret(c, 8u);
}

void g3n_1117a160(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_1117a160(c);
        return;
    }
    const uint32_t a = c->r[1], out = rd32(c->r[4] + 4u);
    const double s = (double)rdf32(c->r[4] + 8u);
    const double z = fx87(c, s * (double)rdf32(a + 8u));
    const double y = fx87(c, s * (double)rdf32(a + 4u));
    const double x = fx87(c, s * (double)rdf32(a));
    c->r[0] = out;
    px_store3(c, out, x, y, z);
    px_free3(c);
    px_ret(c, 8u);
}

/* 1118b390 / 1118b3d0: box = {min[3], max[3]}; t = min + max (or max - min) into a stack vector,
 * then that times 0.5f (the pushed 0x3f000000) into a second one, copied to out by MOV. Ends
 * with EAX and EDX the copied z and y, ECX = out. The two stack vectors are below the final ESP;
 * they are not written. */
static inline void px_half(X86 *c, int sub) {
    const uint32_t box = c->r[1], out = rd32(c->r[4] + 4u);
    double t[3];
    if (sub) {
        t[2] = fx87(c, (double)rdf32(box + 0x14u) - (double)rdf32(box + 8u));
        t[1] = fx87(c, (double)rdf32(box + 0x10u) - (double)rdf32(box + 4u));
        t[0] = fx87(c, (double)rdf32(box + 0xcu) - (double)rdf32(box));
    } else {
        t[2] = fx87(c, (double)rdf32(box + 8u) + (double)rdf32(box + 0x14u));
        t[1] = fx87(c, (double)rdf32(box + 4u) + (double)rdf32(box + 0x10u));
        t[0] = fx87(c, (double)rdf32(box) + (double)rdf32(box + 0xcu));
    }
    float h[3];
    for (int i = 2; i >= 0; --i) {
        const double v = (double)fto_float(c, t[i]);
        h[i] = fto_float(c, fx87(c, 0.5 * v));
    }
    uint32_t hb[3];
    memcpy(hb, h, 12);
    wr32(out, hb[0]);
    wr32(out + 4u, hb[1]);
    wr32(out + 8u, hb[2]);
    c->r[0] = hb[2];
    c->r[1] = out;
    c->r[2] = hb[1];
    px_free3(c);
    px_ret(c, 4u);
}

void g3n_1118b390(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_1118b390(c);
        return;
    }
    px_half(c, 0);
}

void g3n_1118b3d0(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_1118b3d0(c);
        return;
    }
    px_half(c, 1);
}

void fn_11289670(X86 *c); /* PhysXCore: the ray (segment) against a box tree, collecting leaf primitives */
void fn_11279800(X86 *c); /* PhysXCore: the collecting array's grow, thiscall (count), RET 4 */
#ifndef FN_11279800
#define FN_11279800 fn_11279800
#endif

/* FCOMPP and FNSTSW AX, TEST AH,5 / JNP: C3 C2 C0 of a against b into the status word (C1
 * cleared, IE on any NaN, as the kit's fcom does), AX = the status word with TOP (`top`, where
 * the stack is after the two pops). Returns 1 when a < b (C0 alone), the branch that leaves the
 * node. */
static inline int px_fcompp_lt(X86 *c, double a, double b, unsigned top) {
    uint16_t sw = (uint16_t)(c->fpu_sw & (uint16_t)~0x4700u);
    int lt = 0;
    if (a != a || b != b)
        sw |= 0x4501u;
    else if (a < b) {
        sw |= 0x0100u;
        lt = 1;
    } else if (a == b)
        sw |= 0x4000u;
    c->fpu_sw = sw;
    c->r[0] = (c->r[0] & 0xffff0000u) | (uint32_t)((sw & (uint16_t)~0x3800u) | (uint16_t)((top & 7u) << 11));
    return lt;
}

static inline void px_free(X86 *c, unsigned depth) {
    for (unsigned i = 1; i <= depth; ++i) {
        const unsigned ph = (c->fpu_top - i) & 7u;
        ftag_put(c, ph, FTAG_EMPTY);
        c->st_exact[ph] = 0;
    }
}

/* One frame of FUN_11289670 (thiscall ray, (node, list), RET 8), its tail loop over the second
 * child included; `e` is the ESP it was entered with. The ray: +0x28 half extents E, +0x34
 * direction D, +0x40 centre C, +0x60 the node counter. A node (0x24 bytes): box {min, max},
 * +0x18 the first child (bit 0 a flag; the second child follows it), +0x1c / +0x20 a leaf's
 * primitive indices and their count. The list: {capacity, size, data}. Separating axes: the three
 * box axes (|C - centre| against extent + E) and the three cross products of D with them, one
 * FCOMPP each in the original's order, leaving the node on the first that separates. A leaf
 * appends its indices (REP MOVSD) after growing the list through the guest when it is full.
 *
 * The arithmetic is the original's x87 sequence through fx87 and fto_float: the centre and half
 * extents as 1118b390 / 1118b3d0 leave them (floats), dx and dy reloaded as the floats FST stored,
 * dz kept in the register. The frame's stores (the two vectors, dx/dy over the argument slots, the
 * saved registers) are below the final ESP and nothing reads them, so they are not made; the
 * guest call to the grow gets the original's ESP and registers. *depth: the x87 slots below TOP
 * the whole walk has used so far (the stack is at its entry depth between frames). */
static void px_walk(X86 *c, uint32_t ray, uint32_t node, uint32_t list, uint32_t e, unsigned *depth) {
    const uint32_t sp = e - 0x24u; /* ESP inside the frame, after SUB 0x18 and the three pushes */
    const unsigned t0 = c->fpu_top;
    if (*depth < 3u)
        *depth = 3u; /* the centre and extents leaves */
    for (;;) {
        /* 1118b390 / 1118b3d0 */
        float cen[3], ext[3];
        {
            double t[3];
            t[2] = fx87(c, (double)rdf32(node + 8u) + (double)rdf32(node + 0x14u));
            t[1] = fx87(c, (double)rdf32(node + 4u) + (double)rdf32(node + 0x10u));
            t[0] = fx87(c, (double)rdf32(node) + (double)rdf32(node + 0xcu));
            for (int i = 2; i >= 0; --i)
                cen[i] = fto_float(c, fx87(c, 0.5 * (double)fto_float(c, t[i])));
            t[2] = fx87(c, (double)rdf32(node + 0x14u) - (double)rdf32(node + 8u));
            t[1] = fx87(c, (double)rdf32(node + 0x10u) - (double)rdf32(node + 4u));
            t[0] = fx87(c, (double)rdf32(node + 0xcu) - (double)rdf32(node));
            for (int i = 2; i >= 0; --i)
                ext[i] = fto_float(c, fx87(c, 0.5 * (double)fto_float(c, t[i])));
        }
        c->r[1] = sp + 0xcu;          /* 1118b3d0 leaves ECX = its out */
        memcpy(&c->r[2], &ext[1], 4); /* and EDX = the copied y */
        /* 11289698: the x axis, the counter */
        const double dx = fx87(c, (double)rdf32(ray + 0x40u) - (double)cen[0]);
        c->r[0] = rd32(ray + 0x60u) + 1u;
        wr32(ray + 0x60u, c->r[0]);
        const double dxf = fto_float(c, dx);
        if (px_fcompp_lt(c, fx87(c, (double)ext[0] + (double)rdf32(ray + 0x28u)), fabs(dx), t0))
            return;
        const double dy = fx87(c, (double)rdf32(ray + 0x44u) - (double)cen[1]);
        const double dyf = fto_float(c, dy);
        if (px_fcompp_lt(c, fx87(c, (double)ext[1] + (double)rdf32(ray + 0x2cu)), fabs(dy), t0))
            return;
        const double dz = fx87(c, (double)rdf32(ray + 0x48u) - (double)cen[2]);
        if (px_fcompp_lt(c, fx87(c, (double)ext[2] + (double)rdf32(ray + 0x30u)), fabs(dz), t0 - 1u))
            return;
        *depth = 4u;
        {
            const double p = fx87(c, dz * (double)rdf32(ray + 0x38u));
            const double q = fx87(c, dyf * (double)rdf32(ray + 0x3cu));
            const double cr = fabs(fx87(c, p - q));
            const double a = fx87(c, (double)ext[1] * (double)rdf32(ray + 0x30u));
            const double b = fx87(c, (double)ext[2] * (double)rdf32(ray + 0x2cu));
            if (px_fcompp_lt(c, fx87(c, a + b), cr, t0 - 1u))
                return;
        }
        {
            const double p = fx87(c, dxf * (double)rdf32(ray + 0x3cu));
            const double q = fx87(c, dz * (double)rdf32(ray + 0x34u));
            const double cr = fabs(fx87(c, p - q));
            const double a = fx87(c, (double)ext[0] * (double)rdf32(ray + 0x30u));
            const double b = fx87(c, (double)ext[2] * (double)rdf32(ray + 0x28u));
            if (px_fcompp_lt(c, fx87(c, a + b), cr, t0))
                return;
        }
        {
            const double p = fx87(c, dyf * (double)rdf32(ray + 0x34u));
            const double q = fx87(c, dxf * (double)rdf32(ray + 0x38u));
            const double cr = fabs(fx87(c, p - q));
            const double a = fx87(c, (double)ext[0] * (double)rdf32(ray + 0x2cu));
            const double b = fx87(c, (double)ext[1] * (double)rdf32(ray + 0x28u));
            if (px_fcompp_lt(c, fx87(c, a + b), cr, t0))
                return;
        }
        /* 11289785 */
        c->r[0] = rd32(node + 0x18u);
        if (c->r[0] & 0xfffffffeu) {
            px_walk(c, ray, c->r[0] & 0xfffffffeu, list, sp - 12u, depth);
            c->r[0] = rd32(node + 0x18u) & 0xfffffffeu;
            node = c->r[0] ? c->r[0] + 0x24u : 0u;
            continue;
        }
        /* 112897b2: a leaf */
        const uint32_t n = rd32(node + 0x20u), prims = rd32(node + 0x1cu);
        if (prims == 0u || n == 0u)
            return;
        c->r[2] = rd32(list + 4u) + n;
        c->r[0] = rd32(list);
        if (c->r[2] > c->r[0]) {
            /* PUSH EBX (saved) / PUSH EBX (n) / MOV ECX,EBP / CALL 11279800, with the registers
             * the original has there */
            px_free(c, *depth);
            const uint32_t ebx0 = c->r[3];
            wr32(sp - 4u, ebx0);
            wr32(sp - 8u, n);
            wr32(sp - 12u, 0x112897d5u);
            c->r[4] = sp - 12u;
            c->r[1] = list;
            c->r[3] = n;
            c->r[5] = list;
            c->r[6] = ray;
            c->r[7] = prims;
            FN_11279800(c);
            c->r[3] = ebx0;
        }
        const uint32_t size = rd32(list + 4u), data = rd32(list + 8u);
        c->r[0] = size;
        c->r[2] = data;
        uint32_t dst = data + size * 4u, src = prims;
        for (uint32_t k = 0; k < n; ++k, src += 4u, dst += 4u)
            wr32(dst, rd32(src));
        c->r[1] = 0u;
        wr32(list + 4u, rd32(list + 4u) + n);
        return;
    }
}

void g3n_11289670(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_11289670(c);
        return;
    }
    const uint32_t e = c->r[4];
    const uint32_t ebx = c->r[3], ebp = c->r[5], esi = c->r[6], edi = c->r[7];
    unsigned depth = 0;
    px_walk(c, c->r[1], rd32(e + 4u), rd32(e + 8u), e, &depth);
    px_free(c, depth);
    c->r[3] = ebx;
    c->r[5] = ebp;
    c->r[6] = esi;
    c->r[7] = edi;
    c->eip = rd32(e);
    c->r[4] = e + 12u;
    recomp_return(c);
}

void fn_11289e10(X86 *c); /* PhysXCore: the ray (segment) against a quantized box tree */

/* The registers a frame of 11289e10 hands on: EDI and EBP are set only on its leaf paths and are
 * callee-saved, so what a guest call (the grow) sees depends on the path the frame took. */
typedef struct {
    uint32_t edi, ebp;
} PxQ;

/* PUSH 1 / MOV ECX,list / CALL 11279800 returning to `ret`, from the frame at `sp`. */
static inline void pxq_grow(X86 *c, uint32_t sp, uint32_t ret, unsigned depth) {
    px_free(c, depth);
    wr32(sp - 4u, 1u);
    wr32(sp - 8u, ret);
    c->r[4] = sp - 8u;
    FN_11279800(c);
}

/* One frame of FUN_11289e10 (thiscall ray, (node), RET 4), its tail loop included: OPCODE's
 * quantized no-leaf tree. A node (0x14 bytes): int16 centre and uint16 extents, dequantized with
 * the ray's scales (+0x6c centre, +0x78 extents; FILD / FMUL / FSTP float, the x centre kept in
 * the register), then the same six separating axes as 11289670 (ray +0x28 |D|, +0x34 D, +0x40 C,
 * counter +0x60), each operand where the original takes it. +0xc and +0x10: the two children, a
 * pointer, or (bit 0 set) a primitive index * 2 + 1 appended to the list at ray+0x90 {capacity,
 * size, data}, grown through the guest by one when full. After the first child, the walk stops if
 * (ray+4 & 5) == 5 (the caller wants any hit). The frame's stores (the dequantized box, dx/dy and
 * FILD's scratch over the argument slot, the saved registers) are below the final ESP and not made. */
static void pxq_walk(X86 *c, uint32_t ray, uint32_t node, uint32_t e, PxQ *q, unsigned *depth) {
    const uint32_t sp = e - 0x2cu; /* after SUB 0x1c and the four pushes */
    const unsigned t0 = c->fpu_top;
    if (*depth < 2u)
        *depth = 2u;
    for (;;) {
        const int32_t qcx = (int16_t)rd16(node), qcy = (int16_t)rd16(node + 2u), qcz = (int16_t)rd16(node + 4u);
        const uint32_t qex = rd16(node + 6u), qey = rd16(node + 8u), qez = rd16(node + 0xau);
        const double ax = fx87(c, (double)qcx * (double)rdf32(ray + 0x6cu));
        const float cy = fto_float(c, fx87(c, (double)qcy * (double)rdf32(ray + 0x70u)));
        c->r[0] = rd32(ray + 0x60u) + 1u;
        const float cz = fto_float(c, fx87(c, (double)qcz * (double)rdf32(ray + 0x74u)));
        wr32(ray + 0x60u, c->r[0]);
        const float ex = fto_float(c, fx87(c, (double)(int32_t)qex * (double)rdf32(ray + 0x78u)));
        const float ey = fto_float(c, fx87(c, (double)(int32_t)qey * (double)rdf32(ray + 0x7cu)));
        const float ez = fto_float(c, fx87(c, (double)(int32_t)qez * (double)rdf32(ray + 0x80u)));
        c->r[1] = qey;
        c->r[2] = qez;
        const double dxf = fto_float(c, fx87(c, (double)rdf32(ray + 0x40u) - ax));
        if (px_fcompp_lt(c, fx87(c, (double)ex + (double)rdf32(ray + 0x28u)), fabs(dxf), t0))
            return;
        const double dy = fx87(c, (double)rdf32(ray + 0x44u) - (double)cy);
        const double dyf = fto_float(c, dy);
        if (px_fcompp_lt(c, fx87(c, (double)ey + (double)rdf32(ray + 0x2cu)), fabs(dy), t0))
            return;
        if (*depth < 3u)
            *depth = 3u;
        const double dz = fx87(c, (double)rdf32(ray + 0x48u) - (double)cz);
        if (px_fcompp_lt(c, fx87(c, (double)ez + (double)rdf32(ray + 0x30u)), fabs(dz), t0 - 1u))
            return;
        *depth = 4u;
        {
            const double p = fx87(c, dz * (double)rdf32(ray + 0x38u));
            const double qq = fx87(c, dyf * (double)rdf32(ray + 0x3cu));
            const double cr = fabs(fx87(c, p - qq));
            const double a = fx87(c, (double)ez * (double)rdf32(ray + 0x2cu));
            const double b = fx87(c, (double)ey * (double)rdf32(ray + 0x30u));
            if (px_fcompp_lt(c, fx87(c, a + b), cr, t0 - 1u))
                return;
        }
        {
            const double p = fx87(c, dxf * (double)rdf32(ray + 0x3cu));
            const double qq = fx87(c, dz * (double)rdf32(ray + 0x34u));
            const double cr = fabs(fx87(c, p - qq));
            const double a = fx87(c, (double)ex * (double)rdf32(ray + 0x30u));
            const double b = fx87(c, (double)ez * (double)rdf32(ray + 0x28u));
            if (px_fcompp_lt(c, fx87(c, a + b), cr, t0))
                return;
        }
        {
            const double p = fx87(c, dyf * (double)rdf32(ray + 0x34u));
            const double qq = fx87(c, dxf * (double)rdf32(ray + 0x38u));
            const double cr = fabs(fx87(c, p - qq));
            const double a = fx87(c, (double)ey * (double)rdf32(ray + 0x28u));
            const double b = fx87(c, (double)ex * (double)rdf32(ray + 0x2cu));
            if (px_fcompp_lt(c, fx87(c, a + b), cr, t0))
                return;
        }
        /* 11289f85: the first child */
        c->r[0] = rd32(node + 0xcu);
        if (c->r[0] & 1u) {
            const uint32_t list = ray + 0x90u;
            c->r[1] = rd32(list);
            c->r[0] >>= 1;
            q->edi = list;
            q->ebp = c->r[0];
            c->r[0] = rd32(list + 4u);
            if (c->r[0] == c->r[1]) {
                c->r[1] = list;
                c->r[3] = node;
                c->r[5] = q->ebp;
                c->r[6] = ray;
                c->r[7] = list;
                pxq_grow(c, sp, 0x11289facu, *depth);
            }
            c->r[1] = rd32(list + 4u);
            c->r[2] = rd32(list + 8u);
            wr32(c->r[2] + c->r[1] * 4u, q->ebp);
            wr32(list + 4u, rd32(list + 4u) + 1u);
        } else {
            PxQ child = *q; /* its EDI and EBP start as ours, and ours come back after the call */
            pxq_walk(c, ray, c->r[0], sp - 8u, &child, depth);
        }
        /* 11289fc2 */
        c->r[0] = rd32(ray + 4u) & 5u;
        if (c->r[0] == 5u)
            return;
        c->r[0] = rd32(node + 0x10u);
        if (!(c->r[0] & 1u)) {
            node = c->r[0];
            continue;
        }
        /* 11289fda: the second child is a primitive */
        const uint32_t prim = rd32(node + 0x10u) >> 1, list = ray + 0x90u;
        c->r[1] = rd32(list + 4u);
        c->r[0] = rd32(list);
        if (c->r[1] == c->r[0]) {
            c->r[1] = list;
            c->r[3] = prim;
            c->r[5] = q->ebp;
            c->r[6] = list;
            c->r[7] = q->edi;
            pxq_grow(c, sp, 0x11289ffeu, *depth);
        }
        c->r[2] = rd32(list + 4u);
        wr32(rd32(list + 8u) + c->r[2] * 4u, prim);
        c->r[0] = rd32(list + 4u) + 1u;
        wr32(list + 4u, c->r[0]);
        return;
    }
}

void g3n_11289e10(X86 *c) {
    if (!G3OPT(PDO)) {
        fn_11289e10(c);
        return;
    }
    const uint32_t e = c->r[4];
    const uint32_t ebx = c->r[3], ebp = c->r[5], esi = c->r[6], edi = c->r[7];
    PxQ q = {edi, ebp};
    unsigned depth = 0;
    pxq_walk(c, c->r[1], rd32(e + 4u), e, &q, &depth);
    px_free(c, depth);
    c->r[3] = ebx;
    c->r[5] = ebp;
    c->r[6] = esi;
    c->r[7] = edi;
    c->eip = rd32(e);
    c->r[4] = e + 8u;
    recomp_return(c);
}
