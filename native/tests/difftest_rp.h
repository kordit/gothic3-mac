/* native/tests/difftest_rp.h - differential test helpers: run a translated original and its native
 * replacement from the same guest state, compare what the caller can observe.
 *
 * Observable: EAX..EDI and EIP, the x87 top, tag word, status and control word and every live
 * register (bitwise), XMM0-7, and the bytes of each registered memory region, except stack bytes
 * below the final ESP (dead once the function has returned). EFLAGS are not compared: nothing reads
 * them across a call in MSVC code, and the translator keeps a flag only where it is live. */
#ifndef G3N_DIFFTEST_H
#define G3N_DIFFTEST_H
#include "chunk.h"
#include "../g3_opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *harness_mem(void);

/* The harness predates these runtime pieces; all zero is the old behaviour. */
__attribute__((weak)) uint32_t recomp_return_hits[1u << 14];
int recomp_is_call_return(uint32_t target) {
    (void)target;
    return 1;
}
int32_t recomp_index_of(uint32_t target) {
    (void)target;
    return -1;
}

/* Maps a module's sections from the main repo's stage/ at its preferred base (the port never
 * relocates), so constants, vtables and globals the code reads are the real ones. */
static void dt_load_pe(const char *dll) {
    char path[1024];
    const char *main_repo = getenv("G3_MAIN");
    snprintf(path, sizeof path, "%s/stage/%s", main_repo ? main_repo : ".", dll);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n)
        exit(2);
    fclose(f);
    uint32_t pe, base;
    uint16_t nsec, opt;
    memcpy(&pe, d + 0x3c, 4);
    memcpy(&nsec, d + pe + 6, 2);
    memcpy(&opt, d + pe + 20, 2);
    memcpy(&base, d + pe + 24 + 28, 4);
    for (unsigned i = 0; i < nsec; ++i) {
        const uint8_t *s = d + pe + 24 + opt + 40 * i;
        uint32_t va, rs, rp;
        memcpy(&va, s + 12, 4);
        memcpy(&rs, s + 16, 4);
        memcpy(&rp, s + 20, 4);
        memcpy(g_mem + base + va, d + rp, rs);
    }
    free(d);
}

#define DT_MAX_REGIONS 16
static struct {
    uint32_t base, len;
    uint8_t *before, *orig;
} dt_regions[DT_MAX_REGIONS];
static unsigned dt_nregions;
static unsigned dt_fail, dt_cases;
static uint32_t dt_stack_floor; /* stack region base; bytes below the final ESP are dead */

static void dt_region(uint32_t base, uint32_t len) {
    dt_regions[dt_nregions].base = base;
    dt_regions[dt_nregions].len = len;
    dt_regions[dt_nregions].before = malloc(len);
    dt_regions[dt_nregions].orig = malloc(len);
    ++dt_nregions;
}

static uint64_t dt_rng = 0x9e3779b97f4a7c15ull;
static uint32_t dt_rand32(void) {
    dt_rng ^= dt_rng << 13;
    dt_rng ^= dt_rng >> 7;
    dt_rng ^= dt_rng << 17;
    return (uint32_t)(dt_rng >> 11);
}
static float dt_bits(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}
/* Mostly ordinary values in a game's range, sometimes the awkward ones. No signalling NaNs: an
 * FLD/FSTP pair quiets one or not depending on how the compiler optimised the generated C (clang
 * folds (float)(double)x to x when both sit in one block), so the translation itself has no single
 * answer there; real x87 quiets it. */
static float dt_float_any(void) {
    switch (dt_rand32() % 24) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return dt_bits(0x7fc00000u); /* qNaN */
    case 3: return dt_bits(0xffc00000u); /* the x87 indefinite */
    case 4: return dt_bits(0x7f800000u); /* +inf */
    case 5: return dt_bits(0xff800000u); /* -inf */
    case 6: return dt_bits(0x00000001u + dt_rand32() % 0x7fffffu); /* denormal */
    case 7: return 1.0f;
    case 8: return dt_bits(dt_rand32()); /* any pattern */
    case 9: return dt_bits(0x7f7fffffu); /* FLT_MAX */
    default: {
        float m = (float)((int32_t)dt_rand32()) / 2147483648.0f;
        float scale = (float)(1u << (dt_rand32() % 12)) / 16.0f;
        return m * scale * (dt_rand32() & 1 ? 1000.0f : 1.0f);
    }
    }
}
static float dt_float(void) {
    float f = dt_float_any();
    uint32_t b;
    memcpy(&b, &f, 4);
    if ((b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu) != 0)
        b |= 0x00400000u;
    return dt_bits(b);
}
static const uint16_t dt_cws[] = {0x007f, 0x007f, 0x007f, 0x027f, 0x037f, 0x047f, 0x087f, 0x0c7f};
static uint16_t dt_cw(void) {
    return dt_cws[dt_rand32() % (sizeof dt_cws / sizeof dt_cws[0])];
}

/* A context with random scratch registers, a random x87 top and `live` caller values on it. */
static void dt_context(X86 *c, uint32_t esp, unsigned live) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 8; ++i)
        c->r[i] = dt_rand32();
    c->r[4] = esp;
    c->eflags_misc = 0x202;
    c->fpu_cw = dt_cw();
    c->fpu_tag = 0xffff;
    c->fpu_top = dt_rand32() & 7u;
    c->fs_base = GUEST_TEB_BASE;
    for (int i = 0; i < 8; ++i)
        c->st[i] = (double)dt_float(); /* stale values in empty slots */
    for (unsigned i = 0; i < live; ++i)
        fpush(c, (double)dt_float());
    /* DT_STALE_TAGS=1: free slots tagged in use now and then (a caller that left values behind), so
     * the tag word shows which slots the original's pushes and pops used. The game's translation
     * keeps a body's pushes in C locals (game.toml [translate] x87_locals, D-031) and never touches
     * those slots, so by default they are empty, as MSVC code has them at every call. */
#ifndef DT_STALE_TAGS
#define DT_STALE_TAGS 0
#endif
    for (unsigned i = 1; DT_STALE_TAGS && i <= 8u - live; ++i)
        if (dt_rand32() & 1)
            ftag_put(c, (c->fpu_top - i) & 7u, FTAG_VALID);
    for (int i = 0; i < 4; ++i)
        for (int r = 0; r < 8; ++r)
            c->xmm[r][i] = dt_rand32();
}

static void dt_save(int which) {
    for (unsigned i = 0; i < dt_nregions; ++i)
        memcpy(which ? dt_regions[i].orig : dt_regions[i].before, g_mem + dt_regions[i].base, dt_regions[i].len);
}
static void dt_restore(void) {
    for (unsigned i = 0; i < dt_nregions; ++i)
        memcpy(g_mem + dt_regions[i].base, dt_regions[i].before, dt_regions[i].len);
}

static int dt_same_f(double a, double b) {
    return memcmp(&a, &b, 8) == 0;
}

/* Compares the original's end state (o, memory saved by dt_save(1)) with the native one (n, live
 * memory). Prints the first differences under `what`. */
static int dt_compare(const char *what, const X86 *o, const X86 *n) {
    int bad = 0;
    char msg[2048];
    int len = 0;
#define DT_NOTE(...) (len += snprintf(msg + len, len < (int)sizeof msg ? sizeof msg - len : 0, __VA_ARGS__), bad = 1)
    for (int i = 0; i < 8; ++i)
        if (o->r[i] != n->r[i])
            DT_NOTE(" r%d %08x/%08x", i, o->r[i], n->r[i]);
    if (o->eip != n->eip)
        DT_NOTE(" eip %08x/%08x", o->eip, n->eip);
    /* Without DT_STALE_TAGS a slot the original leaves in use where the native emptied it is not a
     * difference: a body that pushes past the caller's values wraps the stack and pops their slots
     * empty, which the original's locals never do (x87_locals); MSVC code has an empty stack at a
     * call. A value the native leaves that the original lost still is. */
    uint16_t tag_o = o->fpu_tag;
    if (!DT_STALE_TAGS)
        for (unsigned p = 0; p < 8; ++p)
            if (((n->fpu_tag >> (2 * p)) & 3u) == FTAG_EMPTY)
                tag_o = (uint16_t)(tag_o | (FTAG_EMPTY << (2 * p)));
    if (o->fpu_top != n->fpu_top || tag_o != n->fpu_tag || o->fpu_sw != n->fpu_sw || o->fpu_cw != n->fpu_cw)
        DT_NOTE(" fpu top %u/%u tag %04x/%04x sw %04x/%04x cw %04x/%04x", o->fpu_top, n->fpu_top, o->fpu_tag,
                n->fpu_tag, o->fpu_sw, n->fpu_sw, o->fpu_cw, n->fpu_cw);
    for (unsigned p = 0; p < 8; ++p)
        if (((tag_o >> (2 * p)) & 3u) != FTAG_EMPTY && !dt_same_f(o->st[p], n->st[p]))
            DT_NOTE(" st[%u] %a/%a", p, o->st[p], n->st[p]);
    for (int r = 0; r < 8; ++r)
        if (memcmp(o->xmm[r], n->xmm[r], 16))
            DT_NOTE(" xmm%d", r);
    for (unsigned i = 0; i < dt_nregions; ++i) {
        const uint32_t base = dt_regions[i].base;
        for (uint32_t k = 0; k < dt_regions[i].len; ++k) {
            const uint32_t a = base + k;
            if (a >= dt_stack_floor && a < o->r[4] && a < GUEST_STACK_TOP && base == dt_stack_floor)
                continue;
            if (dt_regions[i].orig[k] != g_mem[a]) {
                const uint32_t w = k & ~3u;
                uint32_t ow = 0, nw = 0;
                if (w + 4 <= dt_regions[i].len) {
                    memcpy(&ow, dt_regions[i].orig + w, 4);
                    memcpy(&nw, g_mem + base + w, 4);
                }
                DT_NOTE(" mem[%08x] %08x/%08x", base + w, ow, nw);
                break;
            }
        }
    }
    ++dt_cases;
    if (bad) {
        if (dt_fail < 20)
            fprintf(stderr, "FAIL %s:%s\n", what, msg);
        ++dt_fail;
    }
#undef DT_NOTE
    return !bad;
}

/* The groups on for the native run: all, unless a test keeps a group whose callees it fakes off
 * (scene_test fakes CullVPTHC and friends, which the cull group would run natively). */
static uint32_t dt_native_opt = 0xffffffffu;

/* Runs `orig` then `native` from the same state; the caller has already set *c and memory.
 * g3_opt is cleared for the original, so a g3n_ it calls takes the translated path too. */
static int dt_run(const char *what, X86 *c, void (*orig)(X86 *), void (*native)(X86 *)) {
    X86 start = *c, o;
    dt_save(0);
    g3_opt = 0;
    orig(c);
    o = *c;
    dt_save(1);
    dt_restore();
    *c = start;
    g3_opt = dt_native_opt;
    native(c);
    return dt_compare(what, &o, c);
}

static int dt_report(const char *name) {
    printf("%s: %u cases, %u failures\n", name, dt_cases, dt_fail);
    return dt_fail ? 1 : 0;
}
#endif
