/* native/tests/difftest.h - run a translated original and its native replacement from the same
 * guest state and compare what a caller can see: the registers it names, EIP, ESP, a data
 * window and the stack from the final ESP up. Built by native/tests/run.sh. */
#ifndef G3_DIFFTEST_H
#define G3_DIFFTEST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "g3_opt.h"

uint8_t *harness_mem(void);

volatile uint32_t g3_opt; /* native/g3_opt.c is not linked: the test flips the groups itself */

/* Every RET in a test returns to the test's own frame. */
int recomp_is_call_return(uint32_t target) {
    (void)target;
    return 1;
}
int32_t recomp_index_of(uint32_t target) {
    (void)target;
    return -1;
}

#define STACK_TOP 0x00200000u
#define STACK_LOW (STACK_TOP - 0x1000u)
#define RET_ADDR 0x00401234u

#define REG(r) (1u << (r))
#define CALLEE_SAVED (REG(R_EBX) | REG(R_EBP) | REG(R_ESI) | REG(R_EDI))

typedef void (*guest_fn)(X86 *);

static uint64_t rng_state = 0x6733a1d2c0ffee11ull;
static inline uint32_t rnd(void) {
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(rng_state >> 33);
}
static inline uint32_t rnd_below(uint32_t n) {
    return n ? rnd() % n : 0u;
}

static unsigned long diff_runs, diff_fails;
static X86 diff_last; /* the native run's final state */

/* A CPU about to CALL: return address at [ESP], `nargs` dwords above it. */
static X86 call_state(const uint32_t *args, unsigned nargs) {
    X86 c;
    memset(&c, 0, sizeof c);
    static const uint32_t seed[8] = {0xa0a0a0a0u, 0xc1c1c1c1u, 0xd2d2d2d2u, 0xb3b3b3b3u,
                                     0,           0x0019ff00u, 0xe6e6e6e6u, 0xf7f7f7f7u};
    memcpy(c.r, seed, sizeof seed);
    c.fpu_cw = 0x037f;
    c.fpu_tag = 0xffff;
    c.eflags_misc = 0x202;
    uint32_t sp = STACK_TOP - 0x100u;
    c.r[R_ESP] = sp;
    wr32(sp, RET_ADDR);
    for (unsigned i = 0; i < nargs; ++i)
        wr32(sp + 4u + 4u * i, args[i]);
    /* what sits below ESP before the call */
    for (uint32_t a = STACK_LOW; a < sp; a += 4)
        wr32(a, 0x5a5a0000u | (a & 0xffffu));
    return c;
}

/* Maps stage/<dll> at its image base (the staged modules are already based where the game maps
 * them), so the originals find their jump tables and data. G3_STAGE comes from run.sh. */
static void map_module(const char *dll) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", G3_STAGE, dll);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)size);
    if (fread(d, 1, (size_t)size, f) != (size_t)size)
        exit(2);
    fclose(f);
    uint32_t pe, base, hdrs;
    uint16_t nsec, optsz;
    memcpy(&pe, d + 0x3c, 4);
    memcpy(&nsec, d + pe + 6, 2);
    memcpy(&optsz, d + pe + 20, 2);
    memcpy(&base, d + pe + 0x34, 4);
    memcpy(&hdrs, d + pe + 0x54, 4);
    memcpy(g_mem + base, d, hdrs);
    for (unsigned i = 0; i < nsec; ++i) {
        const uint8_t *s = d + pe + 24 + optsz + 40 * i;
        uint32_t vsize, va, raw, ptr;
        memcpy(&vsize, s + 8, 4);
        memcpy(&va, s + 12, 4);
        memcpy(&raw, s + 16, 4);
        memcpy(&ptr, s + 20, 4);
        uint32_t n = raw < vsize || vsize == 0 ? raw : vsize;
        memcpy(g_mem + base + va, d + ptr, n);
    }
    free(d);
}

static uint8_t *save_buf[2], *after_buf;

/* Runs `orig` (all groups off) and `native` (all groups on) from `init` and the current guest
 * memory; compares registers in `regs`, EIP, ESP, [lo, hi) and the stack from the final ESP up.
 * Guest memory is left as the native run left it. */
static int diff_run(const char *what, guest_fn orig, guest_fn native, const X86 *init, uint32_t regs,
                    uint32_t lo, uint32_t hi) {
    const size_t n = hi - lo, sn = STACK_TOP - STACK_LOW;
    if (!save_buf[0]) {
        save_buf[0] = malloc(64u << 20);
        save_buf[1] = malloc(sn);
        after_buf = malloc(64u << 20);
    }
    memcpy(save_buf[0], g_mem + lo, n);
    memcpy(save_buf[1], g_mem + STACK_LOW, sn);

    X86 a = *init, b = *init;
    g3_opt = 0;
    orig(&a);
    memcpy(after_buf, g_mem + lo, n);
    uint8_t stack_a[0x1000];
    memcpy(stack_a, g_mem + STACK_LOW, sn);

    memcpy(g_mem + lo, save_buf[0], n);
    memcpy(g_mem + STACK_LOW, save_buf[1], sn);
    g3_opt = 0xffffffffu;
    native(&b);

    diff_runs++;
    diff_last = b;
    int bad = 0;
    for (int r = 0; r < 8; ++r)
        if ((regs & REG(r)) && a.r[r] != b.r[r]) {
            fprintf(stderr, "%s: reg %d original %08x native %08x\n", what, r, a.r[r], b.r[r]);
            bad = 1;
        }
    if (a.r[R_ESP] != b.r[R_ESP] || a.eip != b.eip) {
        fprintf(stderr, "%s: ESP/EIP original %08x/%08x native %08x/%08x\n", what, a.r[R_ESP], a.eip,
                b.r[R_ESP], b.eip);
        bad = 1;
    }
    if (memcmp(after_buf, g_mem + lo, n)) {
        size_t i = 0;
        while (after_buf[i] == g_mem[lo + i])
            ++i;
        fprintf(stderr, "%s: memory differs at %08zx: original %02x native %02x\n", what, lo + i,
                after_buf[i], g_mem[lo + i]);
        bad = 1;
    }
    uint32_t top = a.r[R_ESP];
    if (top >= STACK_LOW && top < STACK_TOP &&
        memcmp(stack_a + (top - STACK_LOW), g_mem + top, STACK_TOP - top)) {
        fprintf(stderr, "%s: stack above ESP differs\n", what);
        bad = 1;
    }
    if (bad && ++diff_fails > 20) {
        fprintf(stderr, "too many failures\n");
        exit(1);
    }
    return !bad;
}

static int diff_report(const char *name) {
    printf("%s: %lu cases, %lu failed\n", name, diff_runs, diff_fails);
    return diff_fails ? 1 : 0;
}

#endif
