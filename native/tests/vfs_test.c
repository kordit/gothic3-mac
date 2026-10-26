/* native/tests/vfs_test.c - native/vfs.c (FileSystem directory scan 10da4800) against the translated original.
 *
 *   G3N_OUT=$TMPDIR/g3n NATIVE="vfs.c ai.c" sh native/tests/build.sh vfs aux-filesystem/chunk_10da4000.c \
 *       aux-filesystem/chunk_10da0000.c aux-filesystem/chunk_10da8000.c aux-filesystem/chunk_10dac000.c \
 *       aux-filesystem/chunk_10db0000.c
 * (ai.c: the entry match 10da29f0 and name match 10da9e10 the original path calls are native too;
 * chunk_10db0000: FileSystem's _tolower they call.)
 *
 * The original's entry methods are the real translated ones (IsValid 10da29d0, Match 10da29f0, GetName
 * 10da2a40, the wildcard compare 10da9e10/10da9d30 with FileSystem's own CRT _tolower). The SharedBase
 * bCString methods they import are fakes here, written from SharedBase's listings (ctor, dtor, IsEmpty,
 * operator const char *, ReverseFind = strrchr, Right, operator=, operator!=), allocating from a scratch
 * arena that is not compared (the native skip makes none of those temporaries). Entries of another class
 * have a fake Match that answers from a hash; every entry's fill-in (+0x28) is a fake that appends to a
 * call log. Compared: AL, EBX ESI EDI EBP ESP EIP, the handle, node, find data, entries and the log. */
#define _DARWIN_C_SOURCE 1
#include <time.h>
#include "difftest_rp.h"

void fn_10da4800(X86 *c);
void g3n_10da4800(X86 *c);
void fn_10da29d0(X86 *c);
void fn_10da29f0(X86 *c);
void fn_10da2a40(X86 *c);

#define ESP0 0x00200000u
#define NODE 0x00300000u   /* directory node; parent at NODE + 0x100 */
#define HANDLE 0x00300200u /* find handle: +8 pattern bCString, +0x10 index */
#define FIND 0x00300300u   /* find data, +0x2c a bCString */
#define ARRS 0x00300400u   /* subdirectory array, then the file array at +0x800 */
#define ENTS 0x00310000u   /* entries, 0x10 bytes each: vtable, path bCString */
#define STRS 0x00320000u   /* entry paths and the pattern */
#define VTS 0x0033f000u    /* vtables */
#define LOG 0x00340000u    /* fake call log: count, then 4 words per call */
#define ARENA 0x00350000u  /* fake bCString allocations, not compared */
#define EMPTY 0x0035fff0u  /* SharedBase's "" */

#define VT_FILE (VTS + 0x000u)  /* the archive file class */
#define VT_OTHER (VTS + 0x100u) /* another class: fake Match */
#define VT_ODD (VTS + 0x200u)   /* the file class's Match with another IsValid */
#define VT_NODE (VTS + 0x300u)
#define F_FILL 0x00360000u
#define F_MATCH 0x00360010u
#define F_VALID 0x00360020u

/* FileSystem's import slots for SharedBase (pefile on stage/FileSystem.dll) */
#define IAT_NE 0x10dbc194u      /* operator!=(const char *) */
#define IAT_RIGHT 0x10dbc1a8u   /* Right(int) */
#define IAT_RFIND 0x10dbc1acu   /* ReverseFind(char) */
#define IAT_ASSIGNC 0x10dbc1b0u /* operator=(const char *) */
#define IAT_CTOR 0x10dbc1b4u
#define IAT_DTOR 0x10dbc1f0u
#define IAT_EMPTY 0x10dbc204u
#define IAT_ASSIGN 0x10dbc210u
#define IAT_CSTR 0x10dbc224u
#define SB(n) (0x00370000u + 0x10u * (n))

static uint32_t arena;
static uint32_t new_string(const uint8_t *s, uint32_t n) {
    uint32_t p = arena + 8u;
    wr32(p - 8u, n);
    wr32(p - 4u, 1u);
    for (uint32_t i = 0; i < n; ++i)
        wr8(p + i, s[i]);
    wr8(p + n, 0);
    arena = (p + n + 1u + 3u) & ~3u;
    if (arena > EMPTY - 0x400u)
        abort();
    return p;
}
static uint32_t cstr(uint32_t bs) {
    uint32_t p = rd32(bs);
    return p ? p : EMPTY;
}

static void ret(X86 *c, uint32_t n) {
    uint32_t sp = c->r[R_ESP];
    c->eip = rd32(sp);
    c->r[R_ESP] = sp + 4u + n;
    c->r[R_ECX] = dt_rand32();
    c->r[R_EDX] = dt_rand32();
}
static void logcall(uint32_t what, uint32_t a, uint32_t b, uint32_t d) {
    uint32_t n = rd32(LOG);
    if (n >= 200u)
        abort();
    wr32(LOG + 4u + 16u * n, what);
    wr32(LOG + 8u + 16u * n, a);
    wr32(LOG + 12u + 16u * n, b);
    wr32(LOG + 16u + 16u * n, d);
    wr32(LOG, n + 1u);
}
static uint32_t case_seed;
static uint32_t hash(uint32_t x) {
    x ^= case_seed;
    x *= 0x9e3779b1u;
    return x ^ (x >> 15);
}

static unsigned fills, fake_matches;
static uint32_t last_fill;
void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    const uint32_t sp = c->r[R_ESP], th = c->r[R_ECX];
    switch (target) {
    case 0x10da29d0u: fn_10da29d0(c); return;
    case 0x10da29f0u: fn_10da29f0(c); return;
    case 0x10da2a40u: fn_10da2a40(c); return;
    case F_FILL: /* entry->FillFindData(find), RET 4 */
        logcall(1u, th, rd32(sp + 4u), rd32(sp));
        wr32(rd32(sp + 4u), th);
        ++fills;
        last_fill = th;
        c->r[R_EAX] = hash(th);
        ret(c, 4);
        return;
    case F_MATCH: /* other class's Match(pattern), RET 4 */
        logcall(2u, th, rd32(sp + 4u), rd32(sp));
        ++fake_matches;
        c->r[R_EAX] = (hash(th) & 0xffffff00u) | ((hash(th + 1u) & 7u) == 0);
        ret(c, 4);
        return;
    case F_VALID: /* another IsValid: answers from the hash */
        logcall(3u, th, 0, rd32(sp));
        c->r[R_EAX] = hash(th + 2u) & 1u;
        ret(c, 0);
        return;
    case SB(0): /* ctor */
        wr32(th, 0);
        c->r[R_EAX] = th;
        ret(c, 0);
        return;
    case SB(1): /* dtor */
        ret(c, 0);
        return;
    case SB(2): { /* IsEmpty */
        uint32_t p = rd32(th);
        c->r[R_EAX] = (p == 0u || rd32(p - 8u) == 0u) ? 1u : 0u;
        ret(c, 0);
        return;
    }
    case SB(3): /* operator const char * */
        c->r[R_EAX] = cstr(th);
        ret(c, 0);
        return;
    case SB(4): { /* ReverseFind(char) RET 4: strrchr */
        uint32_t p = rd32(th);
        int32_t r = -1;
        if (p) {
            uint8_t ch = (uint8_t)rd32(sp + 4u);
            for (uint32_t i = 0;; ++i) {
                uint8_t v = rd8(p + i);
                if (v == ch)
                    r = (int32_t)i;
                if (!v)
                    break;
            }
        }
        c->r[R_EAX] = (uint32_t)r;
        ret(c, 4);
        return;
    }
    case SB(5): { /* Right(out, n) RET 8 */
        uint32_t out = rd32(sp + 4u), p = rd32(th), len = p ? rd32(p - 8u) : 0u;
        int32_t n = (int32_t)rd32(sp + 8u);
        uint8_t buf[0x1000];
        if (n < 0 || (uint32_t)n >= len) {
            wr32(out, p);
        } else {
            for (int32_t i = 0; i < n; ++i)
                buf[i] = rd8(p + len - (uint32_t)n + (uint32_t)i);
            wr32(out, new_string(buf, (uint32_t)n));
        }
        c->r[R_EAX] = out;
        ret(c, 8);
        return;
    }
    case SB(6): /* operator=(const bCString &) RET 4 */
        wr32(th, rd32(rd32(sp + 4u)));
        c->r[R_EAX] = th;
        ret(c, 4);
        return;
    case SB(7): { /* operator!=(const char *) RET 4 */
        uint32_t a = cstr(th), b = rd32(sp + 4u), i = 0;
        while (rd8(a + i) == rd8(b + i) && rd8(a + i))
            ++i;
        c->r[R_EAX] = rd8(a + i) != rd8(b + i);
        ret(c, 4);
        return;
    }
    case SB(8): { /* operator=(const char *) RET 4 */
        uint32_t s = rd32(sp + 4u), n = 0;
        uint8_t buf[64];
        while (rd8(s + n) && n < 63u)
            buf[n] = rd8(s + n), ++n;
        wr32(th, new_string(buf, n));
        c->r[R_EAX] = th;
        ret(c, 4);
        return;
    }
    }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}

static const char *names[] = {"Speech_Diego_01.wav", "speech_diego_01.WAV", "Speech_Diego_02.wav", "sfx_hit.ogg",
                              "a.b.c", "noext", ".hidden", "x.", "Ardea_Gate_Guard_07.mp3", "Z", "", "abc*def.txt"};
static const char *patterns[] = {"Speech_Diego_01.wav", "SPEECH_DIEGO_01.WAV", "*.*", "*.wav", "Speech_*.wav", "*",
                                 "?peech_Diego_0?.wav", "a.b.c", "a.*", "noext", "*_01.*", "", ".", "x.", "*.*.*", "**.w*"};

static uint8_t rnd_ch(void) {
    static const char pal[] = "aAbBzZ_.*?/0123";
    switch (dt_rand32() % 16) {
    case 0: return (uint8_t)(0x80u + dt_rand32() % 0x80u);
    case 1: return (uint8_t)(1u + dt_rand32() % 0x7fu);
    default: return (uint8_t)pal[dt_rand32() % (sizeof pal - 1)];
    }
}

static uint32_t strs;
static uint32_t put_str(const uint8_t *s, uint32_t n, uint32_t lenfield) {
    uint32_t p = strs + 8u;
    wr32(p - 8u, lenfield);
    wr32(p - 4u, 1u);
    for (uint32_t i = 0; i < n; ++i)
        wr8(p + i, s[i]);
    wr8(p + n, 0);
    strs = (p + n + 1u + 3u) & ~3u;
    return p;
}

/* A random file name: a known one, a mutation of one, a long one or noise, under a random dir path. */
static uint32_t make_name(uint8_t *b) {
    uint32_t n = 0;
    const unsigned dirs = dt_rand32() % 3;
    for (unsigned d = 0; d < dirs; ++d) {
        static const char *dn[] = {"Data/", "speech/", "x/"};
        const char *s = dn[dt_rand32() % 3];
        while (*s)
            b[n++] = (uint8_t)*s++;
    }
    switch (dt_rand32() % 8) {
    case 0: { /* long */
        uint32_t k = 200u + dt_rand32() % 120u;
        for (uint32_t i = 0; i < k; ++i)
            b[n++] = (uint8_t)('a' + i % 26);
        if (dt_rand32() & 1)
            b[n++] = '.', b[n++] = 'w';
        break;
    }
    case 1: { /* noise */
        uint32_t k = dt_rand32() % 12;
        for (uint32_t i = 0; i < k; ++i)
            b[n++] = rnd_ch();
        break;
    }
    default: {
        const char *s = names[dt_rand32() % (sizeof names / sizeof names[0])];
        while (*s)
            b[n++] = (uint8_t)*s++;
        if (dt_rand32() % 4 == 0 && n)
            b[dt_rand32() % n] ^= 0x20;
    }
    }
    return n;
}

static unsigned paths[6]; /* outcomes: no match, match, ".", "..", other class, fallback */

static void one_case(void) {
    case_seed = dt_rand32();
    strs = STRS;
    arena = ARENA;
    wr32(LOG, 0);
    for (uint32_t a = NODE; a < NODE + 0x200u; a += 4)
        wr32(a, dt_rand32());
    for (uint32_t a = FIND; a < FIND + 0x40u; a += 4)
        wr32(a, dt_rand32());
    const uint32_t nsub = dt_rand32() % 4 == 0 ? dt_rand32() % 4 : 0u;
    const uint32_t nfile = dt_rand32() % 4 == 0 ? dt_rand32() % 8 : dt_rand32() % 300;
    wr32(NODE, VT_NODE);
    wr32(NODE + 0x2cu, ARRS);
    wr32(NODE + 0x30u, nsub);
    wr32(NODE + 0x38u, ARRS + 0x800u);
    wr32(NODE + 0x3cu, nfile);
    wr32(NODE + 0x48u, dt_rand32() & 1 ? NODE + 0x100u : 0u);
    wr32(NODE + 0x100u, VT_NODE);
    uint8_t b[1024];
    const int odd_mix = dt_rand32() % 3 == 0;
    for (uint32_t i = 0; i < nsub + nfile; ++i) {
        const uint32_t e = ENTS + 0x10u * i;
        uint32_t vt = VT_FILE;
        if (i < nsub || (odd_mix && dt_rand32() % 16 == 0))
            vt = dt_rand32() & 1 ? VT_OTHER : VT_ODD;
        wr32(e, vt);
        uint32_t n = make_name(b), lf = n;
        if (dt_rand32() % 32 == 0)
            lf = dt_rand32() % (n + 2u); /* a length field that lies */
        wr32(e + 4u, dt_rand32() % 64 == 0 ? 0u : put_str(b, n, lf));
        wr32(e + 8u, dt_rand32());
        wr32(e + 12u, dt_rand32());
        wr32((i < nsub ? ARRS + 4u * i : ARRS + 0x800u + 4u * (i - nsub)), e);
    }
    /* pattern: a known one or one taken from an entry's name */
    uint32_t pn = 0;
    if (dt_rand32() % 4 == 0 && nfile) {
        pn = make_name(b);
        uint32_t s = 0;
        for (uint32_t i = 0; i < pn; ++i)
            if (b[i] == '/')
                s = i + 1u;
        memmove(b, b + s, pn - s);
        pn -= s;
    } else {
        const char *s = patterns[dt_rand32() % (sizeof patterns / sizeof patterns[0])];
        while (*s)
            b[pn++] = (uint8_t)*s++;
    }
    wr32(HANDLE + 8u, dt_rand32() % 32 == 0 ? 0u : put_str(b, pn, pn));
    /* index: before the first call, or somewhere in the middle (FindNext) */
    int32_t idx;
    switch (dt_rand32() % 4) {
    case 0: case 1: idx = -3; break;
    case 2: idx = (int32_t)(dt_rand32() % (nsub + nfile + 2u)) - 2; break;
    default: idx = (int32_t)(nsub + nfile) - (int32_t)(dt_rand32() % 3); break;
    }
    wr32(HANDLE + 0x10u, (uint32_t)idx);
    wr32(HANDLE, dt_rand32());
    wr32(HANDLE + 4u, NODE);
}

static void run(unsigned cases, int on) {
    for (unsigned k = 0; k < cases; ++k) {
        one_case();
        X86 c0;
        dt_context(&c0, ESP0, 0);
        c0.r[R_EAX] = dt_rand32() % 64 == 0 ? 0u : HANDLE;
        wr32(ESP0, 0x10da47adu);
        wr32(ESP0 + 4u, NODE);
        wr32(ESP0 + 8u, FIND);
        wr32(ESP0 + 12u, dt_rand32());
        dt_save(0);
        X86 o = c0, n = c0;
        g3_opt = 0;
        unsigned f0 = fills, m0 = fake_matches;
        last_fill = 0;
        fn_10da4800(&o);
        const uint32_t ofill = last_fill;
        dt_save(1);
        const uint32_t logn = rd32(LOG);
        if (fills != f0) {
            uint32_t e = rd32(FIND);
            paths[1]++;
            (void)e;
        } else
            paths[0]++;
        if (fake_matches != m0)
            paths[4]++;
        dt_restore();
        arena = ARENA;
        g3_opt = on ? 0xffffffffu : 0u;
        g3n_10da4800(&n);
        /* AL only of EAX; ECX and EDX are scratch to both callers */
        o.r[R_EAX] &= 0xffu;
        n.r[R_EAX] &= 0xffu;
        o.r[R_ECX] = n.r[R_ECX];
        o.r[R_EDX] = n.r[R_EDX];
        if (!dt_compare("scan", &o, &n) && ofill) {
            /* the entry the original filled, and the pattern */
            uint32_t pp = rd32(ofill + 4u), pt = rd32(HANDLE + 8u);
            fprintf(stderr, "  entry %08x vt %08x len %u path '", ofill, rd32(ofill), pp ? rd32(pp - 8u) : 0u);
            for (uint32_t i = 0; pp && rd8(pp + i); ++i)
                fputc(rd8(pp + i), stderr);
            fprintf(stderr, "' pattern '");
            for (uint32_t i = 0; pt && rd8(pt + i); ++i)
                fputc(rd8(pt + i), stderr);
            fprintf(stderr, "'\n");
        }
        (void)logn;
    }
}

int main(void) {
    harness_mem();
    dt_load_pe("FileSystem.dll");
    for (uint32_t i = 0; i < 9; ++i) {
        static const uint32_t slots[9] = {IAT_CTOR, IAT_DTOR, IAT_EMPTY, IAT_CSTR, IAT_RFIND, IAT_RIGHT, IAT_ASSIGN, IAT_NE, IAT_ASSIGNC};
        wr32(slots[i], SB(i));
    }
    wr32(VT_FILE + 0x1cu, 0x10da29d0u);
    wr32(VT_FILE + 0x20u, 0x10da29f0u);
    wr32(VT_FILE + 0x24u, 0x10da2a40u);
    wr32(VT_FILE + 0x28u, F_FILL);
    wr32(VT_OTHER + 0x20u, F_MATCH);
    wr32(VT_OTHER + 0x28u, F_FILL);
    wr32(VT_ODD + 0x1cu, F_VALID);
    wr32(VT_ODD + 0x20u, 0x10da29f0u);
    wr32(VT_ODD + 0x24u, 0x10da2a40u);
    wr32(VT_ODD + 0x28u, F_FILL);
    wr32(VT_NODE + 0x28u, F_FILL);
    wr32(EMPTY, 0);
    dt_stack_floor = ESP0 - 0x1000u;
    dt_region(dt_stack_floor, 0x1100u);
    dt_region(NODE, 0x200u);
    dt_region(HANDLE, 0x20u);
    dt_region(FIND, 0x40u);
    dt_region(ARRS, 0x1000u);
    dt_region(ENTS, 0x4000u);
    dt_region(LOG, 0x1000u);
    run(60000, 1);
    run(1000, 0);
    printf("outcomes: no fill %u, fill %u, other-class matches %u\n", paths[0], paths[1], paths[4]);
    if (getenv("G3N_BENCH")) {
        /* 2000 archive files, exact name not present: the original and the native scan */
        struct timespec t0, t1;
        double ms[2];
        for (int on = 0; on < 2; ++on) {
            strs = STRS;
            for (uint32_t i = 0; i < 2000; ++i) {
                char nm[64];
                int n = snprintf(nm, sizeof nm, "Data/Speech/Speech_NPC_%05u.wav", i);
                const uint32_t e = ENTS + 0x10u * (i % 1024u);
                (void)e;
                wr32(0x00380000u + 8u * i, VT_FILE);
                wr32(0x00380004u + 8u * i, put_str((const uint8_t *)nm, (uint32_t)n, (uint32_t)n));
                wr32(0x003a0000u + 4u * i, 0x00380000u + 8u * i);
            }
            wr32(NODE + 0x2cu, ARRS);
            wr32(NODE + 0x30u, 0);
            wr32(NODE + 0x38u, 0x003a0000u);
            wr32(NODE + 0x3cu, 2000);
            wr32(NODE + 0x48u, 0);
            const char *pat = "Speech_Hero_00001.wav";
            wr32(HANDLE + 8u, put_str((const uint8_t *)pat, (uint32_t)strlen(pat), (uint32_t)strlen(pat)));
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int r = 0; r < 20; ++r) {
                arena = ARENA;
                wr32(HANDLE + 0x10u, (uint32_t)-3);
                X86 c;
                dt_context(&c, ESP0, 0);
                c.r[R_EAX] = HANDLE;
                wr32(ESP0, 0x10da47adu);
                wr32(ESP0 + 4u, NODE);
                wr32(ESP0 + 8u, FIND);
                g3_opt = on ? 0xffffffffu : 0u;
                g3n_10da4800(&c);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ms[on] = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / 20.0;
        }
        printf("bench: scan of 2000 entries, original %.3f ms, native %.3f ms\n", ms[0], ms[1]);
    }
    return dt_report("vfs_test");
}
