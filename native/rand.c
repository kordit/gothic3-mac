/* native/rand.c - stage 1: CRT rand through per-thread data (_getptd, TLS shims) on every particle.
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h) and runs its
 * native version only under G3OPT(RAND); otherwise it calls the original.
 *
 * Where the state lives. SharedBase's static CRT keeps rand's seed in the calling thread's
 * per-thread data, _holdrand at ptd+0x14, and reaches it through _getptd (1006ffe8) ->
 * _getptd_noexit (1006ff71), which costs GetLastError, the CRT's FlsGetValue (decoded from an
 * encoded pointer, then TlsGetValue: the kit exports no FlsGetValue, so the CRT falls back to the
 * TLS API) and SetLastError: three import shims per rand, about 6% of a town frame.
 *
 * The native version keeps the state where it is, so the sequence is the original one bit for
 * bit, shared with every other user of the field (GetValue 10053e20 jumps to _rand, srand writes
 * it, the CRT itself reads it), and per guest thread as before. It only skips the shims: the
 * value TlsGetValue(__flsindex) returns is the dword at the thread's TLS array, which the kit puts
 * at TEB + 0x1000 for every guest thread (kernel32.cpp thread_prepare_context, cur_thread: the
 * main thread's TLS_BASE is TEB_BASE + 0x1000), and the TEB is c->fs_base. A native per-host-
 * thread seed was the other design; it would split the sequence from the CRT's field, and
 * srand/GetValue/the CRT would then disagree with rand.
 *
 * The read is not taken on trust: the first time a ptd is needed, the original _getptd runs and
 * the direct read must give the same pointer, or the fast path stays off for the rest of the run
 * (one line on stderr). A slot that reads 0 (no ptd yet, or freed at thread exit) also goes
 * through the original, which allocates one exactly as before. LastError is not touched either
 * way, as the original saves and restores it.
 *
 * The same read stands in for the CRT's __getptd (1006ffe8) and _getptd_noexit (1006ff71) for
 * every other caller too, under the same check: _getptd_noexit and its FlsGetValue lookup 1006fe2a
 * were 3.4% and 1.3% of a town's main thread before the rand group went native
 * (docs/perf/2026-10-24-hostprof-running.txt), rand being one caller of several. */
#include "chunk.h"
#include "g3_opt.h"

#include <stdio.h>

void fn_1006eb6d(X86 *c); /* SharedBase _rand */
void fn_1006eb60(X86 *c); /* SharedBase srand */
void fn_100323a0(X86 *c); /* SharedBase bCRange3::GetRandom */
void fn_1006ffe8(X86 *c); /* SharedBase __getptd */

#define CRT_FLSINDEX 0x100d1158u   /* __flsindex, pushed raw to FlsGetValue by _getptd_noexit */
#define TEB_TLS_ARRAY 0x1000u      /* the kit's TLS slot array, after the TEB page */
#define TLS_SLOTS_KIT 64u          /* kit/runtime/guest.h TLS_SLOTS */
#define RAND_GETPTD_RETURN 0x1006eb72u /* _rand's own call-return site after CALL __getptd */
#define GETRANDOM_DIVISOR 0x10090a98u  /* double RAND_MAX the original divides by */

/* 0: not yet checked, 1: the direct TLS read matched _getptd, -1: it did not. Guest threads run
 * one at a time under the baton, so a plain int is enough. */
int g3n_rand_ptd_direct; /* extern for native/tests/rand_test.c, which resets it */

static inline uint32_t ptd_direct(const X86 *c) {
    const uint32_t idx = rd32(CRT_FLSINDEX);
    if (idx >= TLS_SLOTS_KIT)
        return 0;
    return rd32(c->fs_base + TEB_TLS_ARRAY + 4u * idx);
}

/* The original __getptd, entered as _rand enters it (its return address is _rand's call
 * site, a known call-return, so recomp_return hands control back here). Leaves EAX = ptd and
 * may change ECX/EDX like the original call does. */
static __attribute__((noinline)) uint32_t ptd_slow(X86 *c) {
    c->r[4] -= 4;
    wr32(c->r[4], RAND_GETPTD_RETURN);
    fn_1006ffe8(c);
    const uint32_t ptd = c->r[0];
    if (g3n_rand_ptd_direct == 0) {
        g3n_rand_ptd_direct = ptd_direct(c) == ptd ? 1 : -1;
        if (g3n_rand_ptd_direct < 0)
            fprintf(stderr, "g3n rand: TLS slot %08x != _getptd %08x, native ptd read off\n", ptd_direct(c), ptd);
    }
    return ptd;
}

static inline uint32_t ptd_get(X86 *c) {
    if (__builtin_expect(g3n_rand_ptd_direct > 0, 1)) {
        const uint32_t ptd = ptd_direct(c);
        if (__builtin_expect(ptd != 0, 1))
            return ptd;
    }
    return ptd_slow(c);
}

/* _rand without its RET: holdrand = holdrand * 214013 + 2531011, ECX = holdrand,
 * EAX = (holdrand >> 16) & 0x7fff. */
static inline uint32_t rand_step(X86 *c) {
    const uint32_t ptd = ptd_get(c);
    const uint32_t s = rd32(ptd + 0x14u) * 0x343fdu + 0x269ec3u;
    wr32(ptd + 0x14u, s);
    c->r[1] = s;
    c->r[0] = (s >> 16) & 0x7fffu;
    return c->r[0];
}

/* int __cdecl rand(void) */
void g3n_1006eb6d(X86 *c) {
    if (!G3OPT(RAND)) {
        fn_1006eb6d(c);
        return;
    }
    rand_step(c);
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

/* void __cdecl srand(unsigned seed) */
void g3n_1006eb60(X86 *c) {
    if (!G3OPT(RAND)) {
        fn_1006eb60(c);
        return;
    }
    const uint32_t ptd = ptd_get(c);
    c->r[1] = rd32(c->r[4] + 4u);
    wr32(ptd + 0x14u, c->r[1]);
    c->r[0] = ptd;
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u;
    recomp_return(c);
}

/* Marks ST(-1)..ST(-n) empty, as the original's pushes and pops leave them. */
static inline void ftag_free_below(X86 *c, unsigned n) {
    for (unsigned i = 1; i <= n; ++i)
        ftag_put(c, (c->fpu_top - i) & 7u, FTAG_EMPTY);
}

/* void __thiscall bCRange3::GetRandom(bCVector &out) const, RET 4.
 * this = {min.x, min.y, min.z, max.x, max.y, max.z}. If min == max in all three (FUCOMIP, so a NaN
 * anywhere is "not equal") out = min; else out[i] = min[i] + (max[i] - min[i]) * (rand() / K), each
 * x87 operation rounded as the translation rounds it (fx87: to float under PC = single), components
 * in order with this re-read after each store, as the original does. */
void g3n_100323a0(X86 *c) {
    if (!G3OPT(RAND)) {
        fn_100323a0(c);
        return;
    }
    const uint32_t self = c->r[1];
    const uint32_t out = rd32(c->r[4] + 4u);
    const double x0 = (double)rdf32(self);
    if (x0 == (double)rdf32(self + 0xcu) && (double)rdf32(self + 4u) == (double)rdf32(self + 0x10u) &&
        (double)rdf32(self + 8u) == (double)rdf32(self + 0x14u)) {
        /* MOVSS XMM0,[ESP+4] / MOVSS [EAX],XMM0, then FLD/FSTP of min.y and min.z. */
        const float fx = fto_float(c, x0);
        xmm_set_f32(c, 0, fx);
        c->xmm[0][1] = c->xmm[0][2] = c->xmm[0][3] = 0u;
        wrf32(out, fx);
        wrf32(out + 4u, fto_float(c, (double)rdf32(self + 4u)));
        wrf32(out + 8u, fto_float(c, (double)rdf32(self + 8u)));
        c->r[0] = out;
        ftag_free_below(c, 3);
    } else {
        /* The ptd first, so a fallback through the original _getptd happens before any store. */
        const double k = rdf64(GETRANDOM_DIVISOR);
        for (unsigned i = 0; i < 3; ++i) {
            /* FSTP/FLD through a float stack slot: float bits survive unchanged. */
            const double lo = (double)fto_float(c, (double)rdf32(self + 4u * i));
            const double hi = (double)fto_float(c, (double)rdf32(self + 0xcu + 4u * i));
            const double r = fx87(c, fdivz(c, (double)(int32_t)rand_step(c), k));
            const double d = fx87(c, hi - lo);
            const double m = fx87(c, d * r);
            const double s = fx87(c, lo + m);
            wrf32(out + 4u * i, fto_float(c, s));
        }
        ftag_free_below(c, 4);
    }
    c->eip = rd32(c->r[4]);
    c->r[4] += 8u;
    recomp_return(c);
}

void fn_1006ff71(X86 *c); /* SharedBase _getptd_noexit */

/* The CRT's own entries, for every other caller (errno, locale, the exception machinery, ...).
 * With the direct read checked and the slot set, EAX = ptd and nothing else changes: the original
 * leaves ESI/EDI as they were, LastError as it was (it saves and restores it), and ECX/EDX as the
 * import shims leave them, which no caller can rely on after a call. Anything else runs the
 * original, which allocates a ptd for a thread that has none; while the read is unchecked, the
 * first original run that finds a ptd also checks it. */
static inline int ptd_ret_direct(X86 *c) {
    if (__builtin_expect(g3n_rand_ptd_direct > 0, 1)) {
        const uint32_t ptd = ptd_direct(c);
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

/* _ptd *__cdecl _getptd(void): _getptd_noexit, and _amsg_exit(0x10) when that gives 0. Its
 * original calls _getptd_noexit through g3n_1006ff71, which does the check. */
void g3n_1006ffe8(X86 *c) {
    if (G3OPT(RAND) && ptd_ret_direct(c))
        return;
    fn_1006ffe8(c);
}

/* _ptd *__cdecl _getptd_noexit(void) */
void g3n_1006ff71(X86 *c) {
    if (!G3OPT(RAND)) {
        fn_1006ff71(c);
        return;
    }
    if (ptd_ret_direct(c))
        return;
    fn_1006ff71(c);
    const uint32_t ptd = c->r[0];
    if (g3n_rand_ptd_direct == 0 && ptd != 0) {
        g3n_rand_ptd_direct = ptd_direct(c) == ptd ? 1 : -1;
        if (g3n_rand_ptd_direct < 0)
            fprintf(stderr, "g3n rand: TLS slot %08x != _getptd %08x, native ptd read off\n", ptd_direct(c), ptd);
    }
}

