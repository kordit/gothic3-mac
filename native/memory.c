/* native/memory.c - stage 6: the static CRT memcpy/memmove copies (PUP fast_memcpy).
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h). Until its native
 * version is written it only calls the original; a native version runs under G3OPT(MEM) and
 * must end as the translated function does (see its RET in build/recomp/gen/aux-<module>/).
 *
 * All fourteen are the same MSVC 8 routine (the listings in analysis/ match instruction for
 * instruction apart from addresses), and memcpy is memmove there: a destination inside the
 * source copies from the top down, every other case forward, and the SSE2 path
 * (_VEC_memcpy, n >= 256 with equal alignment mod 16, taken when __sse2_available is set) is a
 * forward copy only ever entered with the destination below the source or clear of it. So the
 * result is memmove(dst, src, n) in every case, EAX = dst, cdecl RET.
 *
 * The host memmove runs only where the kit's REP MOVS fast path would (x86.h
 * recomp_string_fast): no store hook armed when the build has them, no null-page address in
 * a null-check build, and neither range wrapping past 4 GB. Otherwise the translated original
 * runs. ECX and EDX are left as they were (the original leaves loop leftovers there, which no
 * caller of compiled code reads); EBX ESI EDI EBP are not touched. */
#include "chunk.h"
#include "g3_opt.h"

void fn_10078780(X86 *c); /* SharedBase _memcpy */
void fn_1006bba0(X86 *c); /* SharedBase _memmove */
void fn_1056ad40(X86 *c); /* Engine _memcpy */
void fn_10564220(X86 *c); /* Engine _memmove */
void fn_10db3620(X86 *c); /* FileSystem _memcpy */
void fn_10db0050(X86 *c); /* FileSystem _memmove */
void fn_10af1980(X86 *c); /* Game _memcpy */
void fn_10ae9d80(X86 *c); /* Game _memmove */
void fn_10dde620(X86 *c); /* GFC _memcpy */
void fn_10dd99b0(X86 *c); /* GFC _memmove */
void fn_10e34ef0(X86 *c); /* GUI _memcpy */
void fn_10e2d980(X86 *c); /* GUI _memmove */
void fn_10f58ef0(X86 *c); /* Script _memcpy */
void fn_10f4d5a0(X86 *c); /* Script _memmove */

static inline int copy_fast(uint32_t a, uint32_t n) {
    return (uint64_t)a + n <= 0x100000000ull && RECOMP_STRING_FAST_ADDR(a);
}

static inline void crt_memmove(X86 *c, void (*original)(X86 *)) {
    const uint32_t sp = c->r[R_ESP];
    const uint32_t dst = rd32(sp + 4u), src = rd32(sp + 8u), n = rd32(sp + 12u);
    if (n != 0u) {
        if (!RECOMP_STRING_FAST_STORES() || !copy_fast(dst, n) || !copy_fast(src, n)) {
            original(c);
            return;
        }
        memmove(g_mem + dst, g_mem + src, n);
    }
    c->r[R_EAX] = dst;
    c->eip = rd32(sp);
    c->r[R_ESP] = sp + 4u;
    recomp_return(c);
}

#define CRT_COPY(addr)                                                                             \
    void g3n_##addr(X86 *c) {                                                                      \
        if (G3OPT(MEM))                                                                            \
            crt_memmove(c, fn_##addr);                                                             \
        else                                                                                       \
            fn_##addr(c);                                                                          \
    }

CRT_COPY(10078780)
CRT_COPY(1006bba0)
CRT_COPY(1056ad40)
CRT_COPY(10564220)
CRT_COPY(10db3620)
CRT_COPY(10db0050)
CRT_COPY(10af1980)
CRT_COPY(10ae9d80)
CRT_COPY(10dde620)
CRT_COPY(10dd99b0)
CRT_COPY(10e34ef0)
CRT_COPY(10e2d980)
CRT_COPY(10f58ef0)
CRT_COPY(10f4d5a0)
