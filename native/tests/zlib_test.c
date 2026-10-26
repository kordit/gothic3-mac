/* native/tests/zlib_test.c - native/zlib.c (FileSystem inflate 10daa430) against the translated original.
 *
 *   G3N_OUT=$TMPDIR/g3n NATIVE=zlib.c sh native/tests/build.sh zlib aux-filesystem/chunk_10da8000.c \
 *       aux-filesystem/chunk_10dac000.c aux-filesystem/chunk_10db0000.c
 *   G3N_BENCH=1 ... adds MB/s of the translation and the native code on a 24 MB stream.
 *
 * The original is the whole translated chain: inflate 10daa430, inflate_fast 10dad830, inflate_table
 * 10dadff0, updatewindow 10daa340, adler32 10dad5b0, FileSystem's _memcpy (memory.c is not linked, so
 * its g3n_ stub is replaced here by a call to the translated one) and the /GS cookie check. The
 * streams come from the host's libz (dlopen, so build.sh needs no link flag): random, text, runs,
 * periodic, texture-like and game-file content, levels 0-9, windows 2^9..2^15, every strategy, sync /
 * full / partial / block flushes and parameter changes inside the stream, preset dictionaries, raw
 * streams (state->wrap = 0), and real zlib streams found in stage/Data/_compiledImage.pak. Each is
 * inflated by a sequence of calls with the game's chunking (128 KB in, all output), all at once,
 * output-limited, or small random pieces down to zero bytes, and every call is one case: both run
 * from the same guest state, compared are EAX..EDI ESP EIP, the z_stream, the state with its window
 * and code tables, the zalloc heap and call log, the input, the whole output buffer and the stack
 * above the final ESP. Then the same streams damaged (bit flips, bytes, truncation, garbage after a
 * valid header) for the Z_DATA_ERROR paths and their messages, a zalloc that fails (Z_MEM_ERROR, then
 * MEM on the next call), Z_NEED_DICT followed by havedict = 1, and a few with the group off. */
#define _DARWIN_C_SOURCE 1
#include <dlfcn.h>
#include <time.h>
#include <zlib.h>

#include "difftest_rp.h"

void fn_10daa430(X86 *c); /* inflate */
void g3n_10daa430(X86 *c);
void fn_10daa2c0(X86 *c); /* inflateInit_ (strm EAX) */
void fn_10db3620(X86 *c); /* FileSystem _memcpy */

/* memory.c is not part of this test: the memcpy the original calls is the translated one */
void g3n_10db3620(X86 *c) {
    fn_10db3620(c);
}

#define ESP0 0x00200000u
#define CTRL 0x00300000u   /* z_stream; gz_header at +0x100; zalloc arena word +0x200, fail +0x204, log +0x210 */
#define STRM CTRL
#define GZHEAD (CTRL + 0x100u)
#define ARENA_PTR (CTRL + 0x200u)
#define FAIL_AT (CTRL + 0x204u)
#define LOG (CTRL + 0x210u)
#define HEAP 0x00310000u
#define HEAP_END 0x00340000u
#define IN 0x01000000u
#define IN_MAX (24u << 20)
#define OUT 0x04000000u
#define OUT_MAX (40u << 20)
#define F_ZALLOC 0x00360000u
#define F_ZFREE 0x00360010u

/* host libz */
static __typeof__(&deflateInit2_) h_deflateInit2_;
static __typeof__(&deflate) h_deflate;
static __typeof__(&deflateEnd) h_deflateEnd;
static __typeof__(&deflateParams) h_deflateParams;
static __typeof__(&deflateSetDictionary) h_deflateSetDictionary;
static __typeof__(&inflateInit2_) h_inflateInit2_;
static __typeof__(&inflate) h_inflate;
static __typeof__(&inflateEnd) h_inflateEnd;
static __typeof__(&zlibVersion) h_zlibVersion;

static void load_libz(void) {
    void *z = dlopen("/usr/lib/libz.1.dylib", RTLD_NOW);
    if (!z)
        z = dlopen("libz.dylib", RTLD_NOW);
    if (!z) {
        fprintf(stderr, "no libz: %s\n", dlerror());
        exit(2);
    }
#define SYM(n) h_##n = (__typeof__(h_##n))dlsym(z, #n)
    SYM(deflateInit2_);
    SYM(deflate);
    SYM(deflateEnd);
    SYM(deflateParams);
    SYM(deflateSetDictionary);
    SYM(inflateInit2_);
    SYM(inflate);
    SYM(inflateEnd);
    SYM(zlibVersion);
#undef SYM
}

/* ---- the fake zalloc: stdcall (opaque, items, size), from an arena kept in guest memory ---- */

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
static unsigned zalloc_calls, zalloc_fails;

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    const uint32_t sp = c->r[R_ESP];
    if (target == F_ZALLOC) {
        const uint32_t opaque = rd32(sp + 4u), items = rd32(sp + 8u), size = rd32(sp + 12u);
        const uint32_t n = rd32(LOG);
        if (n < 32u) {
            const uint32_t e = LOG + 4u + 24u * n;
            wr32(e, opaque);
            wr32(e + 4u, items);
            wr32(e + 8u, size);
            wr32(e + 12u, rd32(sp));
            wr32(e + 16u, sp);
            wr32(e + 20u, c->r[R_EBX] ^ c->r[R_ESI] ^ c->r[R_EDI] ^ c->r[R_EBP]);
        }
        wr32(LOG, n + 1u);
        uint32_t p = rd32(ARENA_PTR);
        const uint32_t fail = rd32(FAIL_AT);
        ++zalloc_calls;
        if (fail != 0u && fail == n + 1u) {
            p = 0;
            ++zalloc_fails;
        } else {
            const uint64_t bytes = (uint64_t)items * size;
            if (p + bytes + 16u > HEAP_END) {
                fprintf(stderr, "heap arena full\n");
                abort();
            }
            /* fresh memory is not zero, like malloc's */
            for (uint32_t i = 0; i < bytes; i += 4u)
                wr32(p + i, hash32(p + i));
            wr32(ARENA_PTR, (uint32_t)((p + bytes + 15u) & ~15u));
        }
        c->r[R_EAX] = p;
        c->r[R_ECX] = hash32(n * 3u + 1u);
        c->r[R_EDX] = hash32(n * 3u + 2u);
        c->eip = rd32(sp);
        c->r[R_ESP] = sp + 16u; /* zcalloc 10dadfc0 is RET 0xc */
        return;
    }
    fprintf(stderr, "unexpected guest call %08x\n", target);
    abort();
}

/* ---- stream sources ---- */

static uint8_t *src_buf, *cmp_buf, *pak_buf;
static size_t pak_len;

static void fill_source(uint8_t *d, size_t n, int kind) {
    static const char *words[] = {"the",    "Gothic", "orc",   "sword",  "Myrtana", " ",    "\n",    "Vengard",
                                  "Nordmar", "paladin", "rune", "of",     "and",     "a",    "<item>", "Hero",
                                  "Xardas", "0.000000", "1",    "Rhobar", "Varant",  "Geldern", ",",   "texture"};
    size_t i = 0;
    switch (kind) {
    case 0: /* random */
        for (; i < n; ++i)
            d[i] = (uint8_t)dt_rand32();
        break;
    case 1: /* text */
        while (i < n) {
            const char *w = words[dt_rand32() % (sizeof words / sizeof words[0])];
            while (*w && i < n)
                d[i++] = (uint8_t)*w++;
        }
        break;
    case 2: /* runs */
        while (i < n) {
            uint8_t b = (uint8_t)dt_rand32();
            size_t r = 1u + dt_rand32() % (dt_rand32() % 4 ? 20u : 2000u);
            while (r-- && i < n)
                d[i++] = b;
        }
        break;
    case 3: { /* periodic with noise: matches shorter than their length */
        const unsigned p = 1u + dt_rand32() % (dt_rand32() % 2 ? 8u : 300u);
        for (; i < n; ++i)
            d[i] = i < p ? (uint8_t)dt_rand32() : (dt_rand32() % 97 == 0 ? (uint8_t)dt_rand32() : d[i - p]);
        break;
    }
    case 4: { /* texture-like: DXT1 blocks, colours of a few neighbourhoods, index noise */
        uint16_t c0 = (uint16_t)dt_rand32(), c1 = (uint16_t)dt_rand32();
        for (; i + 8 <= n; i += 8) {
            if (dt_rand32() % 16 == 0) {
                c0 = (uint16_t)(c0 + dt_rand32() % 64 - 32);
                c1 = (uint16_t)(c1 + dt_rand32() % 64 - 32);
            }
            memcpy(d + i, &c0, 2);
            memcpy(d + i + 2, &c1, 2);
            uint32_t idx = dt_rand32() % 3 ? hash32((uint32_t)i / 64u) : dt_rand32();
            memcpy(d + i + 4, &idx, 4);
        }
        for (; i < n; ++i)
            d[i] = 0;
        break;
    }
    case 5: /* game file bytes */
        if (pak_len > n) {
            memcpy(d, pak_buf + dt_rand32() % (pak_len - n), n);
            break;
        }
        /* fallthrough */
    default: /* zeros with a few bytes */
        memset(d, 0, n);
        for (unsigned k = dt_rand32() % 8; k; --k)
            if (n)
                d[dt_rand32() % n] = (uint8_t)dt_rand32();
        break;
    }
}

/* Compresses src into cmp with host zlib. wbits < 0 is raw. Returns the length. */
static size_t compress_host(const uint8_t *src, size_t n, uint8_t *cmp, size_t cap, int level, int wbits, int memlevel,
                            int strategy, int flushes, const uint8_t *dict, unsigned ndict) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (h_deflateInit2_(&z, level, Z_DEFLATED, wbits, memlevel, strategy, h_zlibVersion(), (int)sizeof z) != Z_OK)
        abort();
    if (dict)
        h_deflateSetDictionary(&z, dict, ndict);
    z.next_out = cmp;
    z.avail_out = (uInt)cap;
    size_t pos = 0;
    while (pos < n) {
        size_t piece = flushes ? 1u + dt_rand32() % (dt_rand32() % 2 ? 300u : 40000u) : n - pos;
        if (piece > n - pos)
            piece = n - pos;
        z.next_in = (Bytef *)src + pos;
        z.avail_in = (uInt)piece;
        int f = Z_NO_FLUSH;
        if (flushes) {
            switch (dt_rand32() % 8) {
            case 0: f = Z_SYNC_FLUSH; break;
            case 1: f = Z_FULL_FLUSH; break;
            case 2: f = Z_PARTIAL_FLUSH; break;
            case 3: f = Z_BLOCK; break;
            case 4: h_deflateParams(&z, (int)(dt_rand32() % 10), (int)(dt_rand32() % 5)); break;
            default: break;
            }
        }
        while (z.avail_in) {
            if (h_deflate(&z, f) == Z_STREAM_ERROR)
                abort();
            if (z.avail_out == 0)
                abort();
        }
        if (f != Z_NO_FLUSH)
            h_deflate(&z, f);
        pos += piece;
    }
    z.avail_in = 0;
    while (h_deflate(&z, Z_FINISH) != Z_STREAM_END)
        if (z.avail_out == 0)
            abort();
    size_t len = cap - z.avail_out;
    h_deflateEnd(&z);
    return len;
}

/* ---- running the guest inflate ---- */

static unsigned rets[8], streams_ok, streams_checked, outputs_bad, fallbacks_forced;
static unsigned msg_seen[64];

static void set_regions(uint32_t nin, uint32_t nout) {
    dt_regions[1].len = HEAP + 0x10000u - CTRL; /* the state and the window, grown by the arena below */
    dt_regions[2].len = nin ? nin : 1u;
    dt_regions[3].len = nout ? nout : 1u;
}

/* inflateInit_ through the translation (both runs share its result) */
static void init_stream(uint32_t opaque) {
    for (uint32_t a = CTRL; a < CTRL + 0x400u; a += 4)
        wr32(a, 0);
    wr32(ARENA_PTR, HEAP);
    wr32(STRM + 0x20u, F_ZALLOC);
    wr32(STRM + 0x24u, F_ZFREE);
    wr32(STRM + 0x28u, opaque);
    X86 c;
    dt_context(&c, ESP0, 0);
    c.r[R_EAX] = STRM;
    wr32(ESP0, 0x10da3514u);
    g3_opt = 0;
    fn_10daa2c0(&c);
    if (c.r[R_EAX] != 0u) {
        fprintf(stderr, "inflateInit_ failed %d\n", (int)c.r[R_EAX]);
        exit(1);
    }
}

static uint32_t state_addr(void) {
    return rd32(STRM + 0x1cu);
}

/* One inflate call, both ways. Returns the native's EAX. */
static int32_t call_inflate(const char *what) {
    X86 c;
    dt_context(&c, ESP0, (unsigned)(dt_rand32() % 3));
    wr32(ESP0, 0x10da35beu);
    wr32(ESP0 + 4u, STRM);
    wr32(ESP0 + 8u, dt_rand32());
    dt_regions[1].len = rd32(ARENA_PTR) + 0x8100u - CTRL; /* room for a window allocated now */
    dt_run(what, &c, fn_10daa430, g3n_10daa430);
    const int32_t r = (int32_t)c.r[R_EAX];
    if (r >= -5 && r <= 2)
        rets[r + 5]++;
    const uint32_t m = rd32(STRM + 0x18u);
    if (m >= 0x10dc1cbcu && m < 0x10dc1ea0u)
        msg_seen[(m - 0x10dc1cbcu) / 8u % 64u]++;
    return r;
}

enum { CH_GAME, CH_ALL, CH_OUTLIM, CH_SMALL, CH_MIXED, CH_N };

/* Inflates cmp[0, nc) (already at IN) into OUT with a chunking pattern. `expect` (or NULL) is the
 * plain data, checked when the stream ends. */
static void inflate_stream(uint32_t nc, uint32_t nout_cap, int chunking, const uint8_t *expect, uint32_t nexpect,
                           int wrap0, int fail_alloc, int dict_ok) {
    init_stream(dt_rand32());
    const uint32_t st = state_addr();
    if (wrap0)
        wr32(st + 8u, 0);
    if (dt_rand32() % 8 == 0)
        wr32(st + 0x20u, GZHEAD); /* head: zlib streams only write head->done */
    if (dt_rand32() % 16 == 0)
        wr32(st + 0x24u, 8u + dt_rand32() % 8u); /* a smaller wbits: "invalid window size" */
    if (fail_alloc)
        wr32(FAIL_AT, 2u); /* the state is call 1; the window call 2 fails */
    set_regions(nc, nout_cap);
    for (uint32_t a = OUT; a < OUT + nout_cap; a += 4)
        wr32(a, 0xcdcdcdcdu);
    wr32(STRM + 0u, IN);
    wr32(STRM + 0xcu, OUT);
    unsigned calls = 0, stuck = 0;
    for (;;) {
        const uint32_t used_in = rd32(STRM) - IN, used_out = rd32(STRM + 0xcu) - OUT;
        const uint32_t rem_in = nc - used_in, rem_out = nout_cap - used_out;
        uint32_t ai, ao;
        switch (chunking) {
        case CH_GAME: ai = rem_in < 0x20000u ? rem_in : 0x20000u; ao = rem_out; break;
        case CH_ALL: ai = rem_in; ao = rem_out; break;
        case CH_OUTLIM: ai = rem_in; ao = 1u + dt_rand32() % (dt_rand32() % 2 ? 300u : 40000u); break;
        case CH_SMALL: ai = dt_rand32() % 24u; ao = dt_rand32() % 400u; break;
        default: ai = dt_rand32() % (dt_rand32() % 2 ? 9000u : 200u); ao = dt_rand32() % (dt_rand32() % 2 ? 70000u : 600u); break;
        }
        if (ai > rem_in)
            ai = rem_in;
        if (ao > rem_out)
            ao = rem_out;
        wr32(STRM + 4u, ai);
        wr32(STRM + 0x10u, ao);
        const int32_t r = call_inflate("inflate");
        ++calls;
        if (r == Z_NEED_DICT && dict_ok) {
            wr32(st + 0xcu, 1u); /* inflateSetDictionary's havedict, without the window */
            continue;
        }
        if (r == Z_STREAM_END) {
            if (expect) {
                ++streams_checked;
                if (rd32(STRM + 0x14u) != nexpect || memcmp(g_mem + OUT, expect, nexpect)) {
                    if (++outputs_bad < 5)
                        fprintf(stderr, "stream output differs from the source (%u bytes)\n", nexpect);
                }
            }
            ++streams_ok;
            if (dt_rand32() % 4 == 0)
                call_inflate("inflate after end");
            break;
        }
        if (r == Z_DATA_ERROR || r == Z_MEM_ERROR || r == Z_NEED_DICT || r == Z_STREAM_ERROR) {
            if (r == Z_MEM_ERROR || dt_rand32() % 2 == 0)
                call_inflate("inflate after error");
            break;
        }
        /* Z_OK or Z_BUF_ERROR: go on while something can move */
        if (rd32(STRM) - IN == used_in && rd32(STRM + 0xcu) - OUT == used_out) {
            if (rem_in == 0 || rem_out == 0 || ++stuck > 64)
                break;
        } else
            stuck = 0;
        if (calls > 200000u)
            break;
    }
}

static void damage(uint8_t *d, uint32_t *n) {
    const unsigned k = 1u + dt_rand32() % 4u;
    for (unsigned i = 0; i < k && *n; ++i) {
        switch (dt_rand32() % 6) {
        case 0:
        case 1: d[dt_rand32() % *n] ^= (uint8_t)(1u << (dt_rand32() % 8)); break;
        case 2: d[dt_rand32() % *n] = (uint8_t)dt_rand32(); break;
        case 3: *n = dt_rand32() % *n; break;
        case 4: { /* garbage after the header */
            for (uint32_t j = 2u + dt_rand32() % 8u; j < *n; ++j)
                d[j] = (uint8_t)dt_rand32();
            break;
        }
        default: if (*n > 2) d[dt_rand32() % 2] = (uint8_t)dt_rand32(); break; /* the header */
        }
    }
}

/* A zlib header and one block of random bits: fixed, stored, or a dynamic block whose code-length
 * code is complete (8 of the 19 at length 3), so decoding gets past it to the repeat, end-of-block
 * and lit/len / distance set checks. */
static uint32_t craft(uint8_t *d, uint32_t cap) {
    uint32_t n = 0, acc = 0, nb = 0;
#define PUT(v, k)                                                                                  \
    do {                                                                                           \
        acc |= (uint32_t)(v) << nb;                                                                \
        nb += (k);                                                                                 \
        while (nb >= 8) {                                                                          \
            d[n++] = (uint8_t)acc;                                                                 \
            acc >>= 8;                                                                             \
            nb -= 8;                                                                               \
        }                                                                                          \
    } while (0)
    d[n++] = 0x78;
    d[n++] = 0x9c;
    const unsigned type = dt_rand32() % 4 ? 2u : dt_rand32() % 3;
    PUT(dt_rand32() & 1u, 1);
    PUT(type, 2);
    if (type == 2 && dt_rand32() % 2) {
        /* a complete lit/len code (254 at 8 bits, 4 at 9) and random distance lengths: the
         * distance set's checks. Code-length code: 8 symbols at 3 bits, 8 and 9 among them. */
        unsigned set[8] = {8, 9}, k = 2;
        while (k < 8) {
            const unsigned v = dt_rand32() % 16u;
            int dup = 0;
            for (unsigned i = 0; i < k; ++i)
                dup |= set[i] == v;
            if (!dup)
                set[k++] = v;
        }
        for (unsigned i = 0; i < 8; ++i) /* ascending: the canonical codes are 0..7 */
            for (unsigned j = i + 1; j < 8; ++j)
                if (set[j] < set[i]) {
                    const unsigned t = set[i];
                    set[i] = set[j];
                    set[j] = t;
                }
        const unsigned ndist = 1u + dt_rand32() % 30u;
        PUT(1, 5); /* HLIT: 258 */
        PUT(ndist - 1u, 5);
        PUT(15, 4); /* HCLEN: 19 */
        static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        for (unsigned i = 0; i < 19; ++i) {
            unsigned l = 0;
            for (unsigned j = 0; j < 8; ++j)
                l |= (set[j] == order[i]) ? 3u : 0u;
            PUT(l, 3);
        }
        for (unsigned sym = 0; sym < 258u + ndist; ++sym) {
            unsigned want = sym < 254u ? 8u : sym < 258u ? 9u : set[dt_rand32() % 8u];
            if (sym >= 258u && want > 15u)
                want = 0;
            unsigned code = 0;
            for (unsigned j = 0; j < 8; ++j)
                if (set[j] == want)
                    code = j;
            PUT(((code & 1u) << 2) | (code & 2u) | ((code >> 2) & 1u), 3); /* MSB first */
        }
    } else if (type == 2) {
        const unsigned hclen = 4u + dt_rand32() % 16u; /* 4..19 */
        PUT(dt_rand32() % (dt_rand32() % 4 ? 30u : 32u), 5);
        PUT(dt_rand32() % (dt_rand32() % 4 ? 30u : 32u), 5);
        PUT(hclen - 4u, 4);
        unsigned lens[19] = {0}, k = 0;
        if (dt_rand32() % 4) {
            while (k < 8 && hclen >= 8) {
                const unsigned i = dt_rand32() % hclen;
                if (!lens[i])
                    lens[i] = 3, ++k;
            }
        } else
            for (unsigned i = 0; i < hclen; ++i)
                lens[i] = dt_rand32() % 8u;
        for (unsigned i = 0; i < hclen; ++i)
            PUT(lens[i], 3);
    }
    while (n + 8 < cap && n < 60u + dt_rand32() % 3000u)
        PUT(dt_rand32() & 0xffu, 8);
    PUT(0, 7);
#undef PUT
    return n;
}

/* real streams from a game archive: zlib headers that the host inflates to the end */
typedef struct {
    uint32_t off, nc, nout;
} pak_stream;
static pak_stream paks[64];
static unsigned npaks;

static void find_pak_streams(void) {
    if (!pak_buf)
        return;
    uint8_t *tmp = malloc(8u << 20);
    for (size_t i = 0; i + 2 < pak_len && npaks < 64; ++i) {
        if (pak_buf[i] != 0x78 || ((pak_buf[i] << 8) | pak_buf[i + 1]) % 31 != 0)
            continue;
        z_stream z;
        memset(&z, 0, sizeof z);
        h_inflateInit2_(&z, 15, h_zlibVersion(), (int)sizeof z);
        z.next_in = pak_buf + i;
        z.avail_in = (uInt)(pak_len - i < (16u << 20) ? pak_len - i : (16u << 20));
        z.next_out = tmp;
        z.avail_out = 8u << 20;
        const int r = h_inflate(&z, Z_FINISH);
        if (r == Z_STREAM_END && z.total_out >= 64 && z.total_in < IN_MAX) {
            paks[npaks].off = (uint32_t)i;
            paks[npaks].nc = (uint32_t)z.total_in;
            paks[npaks].nout = (uint32_t)z.total_out;
            ++npaks;
            i += z.total_in - 1;
        }
        h_inflateEnd(&z);
    }
    free(tmp);
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void) {
    harness_mem();
    dt_load_pe("FileSystem.dll");
    load_libz();
    src_buf = malloc(OUT_MAX);
    cmp_buf = malloc(IN_MAX);

    /* game bytes: the start of the texture archive */
    {
        const char *main_repo = getenv("G3_MAIN");
        char path[1024];
        snprintf(path, sizeof path, "%s/stage/Data/_compiledImage.pak", main_repo ? main_repo : ".");
        FILE *f = fopen(path, "rb");
        if (f) {
            pak_buf = malloc(24u << 20);
            pak_len = fread(pak_buf, 1, 24u << 20, f);
            fclose(f);
        }
        find_pak_streams();
        printf("real streams from _compiledImage.pak: %u\n", npaks);
    }

    dt_stack_floor = ESP0 - 0x1000u;
    dt_region(dt_stack_floor, 0x1100u);
    dt_region(CTRL, HEAP_END - CTRL);
    dt_region(IN, IN_MAX);
    dt_region(OUT, OUT_MAX);

    const double t0 = now_ms();
    const unsigned nstreams = getenv("G3N_ZLIB_STREAMS") ? (unsigned)atoi(getenv("G3N_ZLIB_STREAMS")) : 1500u;
    /* valid streams from the host */
    for (unsigned s = 0; s < nstreams; ++s) {
        const int big = dt_rand32() % 16 == 0;
        uint32_t n = big ? 200000u + dt_rand32() % 2000000u : dt_rand32() % (dt_rand32() % 2 ? 2000u : 120000u);
        const int kind = (int)(dt_rand32() % 7);
        fill_source(src_buf, n, kind);
        const int raw = dt_rand32() % 10 == 0;
        int wb = 9 + (int)(dt_rand32() % 7);
        const int level = (int)(dt_rand32() % 11) - 1;
        const int strategy = (int)(dt_rand32() % 5);
        const int memlevel = 1 + (int)(dt_rand32() % 9);
        const int flushes = dt_rand32() % 4 == 0;
        uint8_t dict[512];
        const int use_dict = !raw && dt_rand32() % 24 == 0;
        if (use_dict)
            fill_source(dict, sizeof dict, kind);
        uint32_t nc = (uint32_t)compress_host(src_buf, n, cmp_buf, IN_MAX, level, raw ? -wb : wb, memlevel, strategy,
                                              flushes, use_dict ? dict : NULL, sizeof dict);
        memcpy(g_mem + IN, cmp_buf, nc);
        int ch = (int)(dt_rand32() % CH_N);
        if ((ch == CH_SMALL || ch == CH_MIXED || ch == CH_OUTLIM) && nc > 60000u)
            ch = CH_GAME;
        const uint32_t cap = n + (dt_rand32() % 2 ? 0u : dt_rand32() % 5000u);
        inflate_stream(nc, cap, ch, use_dict ? NULL : src_buf, n, raw, dt_rand32() % 40 == 0, use_dict);
    }
    const unsigned valid_cases = dt_cases;
    /* real archive streams, game chunking and small pieces */
    for (unsigned i = 0; i < npaks; ++i) {
        memcpy(g_mem + IN, pak_buf + paks[i].off, paks[i].nc);
        inflate_stream(paks[i].nc, paks[i].nout, CH_GAME, NULL, 0, 0, 0, 0);
        if (paks[i].nc < 60000u) {
            memcpy(g_mem + IN, pak_buf + paks[i].off, paks[i].nc);
            inflate_stream(paks[i].nc, paks[i].nout, CH_MIXED, NULL, 0, 0, 0, 0);
        }
    }
    /* damaged streams */
    for (unsigned s = 0; s < nstreams; ++s) {
        uint32_t n = dt_rand32() % (dt_rand32() % 2 ? 3000u : 60000u);
        fill_source(src_buf, n, (int)(dt_rand32() % 7));
        const int raw = dt_rand32() % 10 == 0;
        uint32_t nc = (uint32_t)compress_host(src_buf, n, cmp_buf, IN_MAX, (int)(dt_rand32() % 10), raw ? -15 : 15,
                                              8, (int)(dt_rand32() % 5), dt_rand32() % 4 == 0, NULL, 0);
        if (dt_rand32() % 8 == 0) { /* noise behind a valid header */
            for (uint32_t j = 2; j < nc; ++j)
                cmp_buf[j] = (uint8_t)dt_rand32();
        } else
            damage(cmp_buf, &nc);
        memcpy(g_mem + IN, cmp_buf, nc);
        int ch = (int)(dt_rand32() % CH_N);
        inflate_stream(nc, n + dt_rand32() % 3000u, ch, NULL, 0, raw, 0, 0);
    }
    /* crafted blocks: the table errors */
    for (unsigned s = 0; s < nstreams * 2u; ++s) {
        uint32_t nc = craft(cmp_buf, IN_MAX);
        memcpy(g_mem + IN, cmp_buf, nc);
        inflate_stream(nc, 1u + dt_rand32() % 70000u, (int)(dt_rand32() % CH_N), NULL, 0, 0, 0, 0);
    }
    /* a window allocation that fails */
    for (unsigned s = 0; s < 60; ++s) {
        uint32_t n = 1000u + dt_rand32() % 60000u;
        fill_source(src_buf, n, (int)(dt_rand32() % 7));
        uint32_t nc = (uint32_t)compress_host(src_buf, n, cmp_buf, IN_MAX, 6, 15, 8, 0, 0, NULL, 0);
        memcpy(g_mem + IN, cmp_buf, nc);
        inflate_stream(nc, n, CH_OUTLIM, NULL, 0, 0, 1, 0);
    }
    /* the group off: the native entry is the original */
    {
        const uint32_t keep = dt_native_opt;
        dt_native_opt = 0;
        for (unsigned s = 0; s < 40; ++s) {
            uint32_t n = dt_rand32() % 30000u;
            fill_source(src_buf, n, (int)(dt_rand32() % 7));
            uint32_t nc = (uint32_t)compress_host(src_buf, n, cmp_buf, IN_MAX, 6, 15, 8, 0, 0, NULL, 0);
            memcpy(g_mem + IN, cmp_buf, nc);
            inflate_stream(nc, n, CH_MIXED, src_buf, n, 0, 0, 0);
        }
        dt_native_opt = keep;
    }
    /* gzip state (wrap 2): the native code hands it to the original */
    for (unsigned s = 0; s < 40; ++s) {
        uint32_t n = dt_rand32() % 30000u;
        fill_source(src_buf, n, (int)(dt_rand32() % 7));
        uint32_t nc = (uint32_t)compress_host(src_buf, n, cmp_buf, IN_MAX, 6, 31, 8, 0, 0, NULL, 0);
        memcpy(g_mem + IN, cmp_buf, nc);
        init_stream(1);
        wr32(state_addr() + 8u, 2u);
        set_regions(nc, n);
        wr32(STRM, IN);
        wr32(STRM + 0xcu, OUT);
        wr32(STRM + 4u, nc);
        wr32(STRM + 0x10u, n);
        call_inflate("gzip");
        ++fallbacks_forced;
    }

    printf("returns: ok %u, end %u, need dict %u, data error %u, mem error %u, buf error %u, stream error %u\n",
           rets[5], rets[6], rets[7], rets[2], rets[1], rets[0], rets[3]);
    printf("streams ended %u, checked against the source %u (%u differ), valid-stream cases %u, zalloc %u (%u failed)\n",
           streams_ok, streams_checked, outputs_bad, valid_cases, zalloc_calls, zalloc_fails);
    {
        static const char *names[] = {"header check", "method", "window size", "flags", "hcrc", "block type",
                                      "stored lengths", "too many", "code lengths", "repeat", "missing eob",
                                      "lit/len set", "dist set", "lit/len code", "dist code", "too far",
                                      "data check", "length check"};
        static const uint32_t addrs[] = {0x10dc1cbcu, 0x10dc1cd4u, 0x10dc1cf0u, 0x10dc1d04u, 0x10dc1d20u, 0x10dc1d34u,
                                         0x10dc1d48u, 0x10dc1d68u, 0x10dc1d8cu, 0x10dc1da8u, 0x10dc1dc4u, 0x10dc1decu,
                                         0x10dc1e08u, 0x10dc1e20u, 0x10dc1e3cu, 0x10dc1e54u, 0x10dc1e74u, 0x10dc1e8cu};
        printf("messages (calls ending with it set):");
        for (unsigned i = 0; i < sizeof addrs / sizeof addrs[0]; ++i)
            printf(" %s %u%s", names[i], msg_seen[(addrs[i] - 0x10dc1cbcu) / 8u % 64u], i + 1 < sizeof addrs / 4 ? "," : "\n");
    }
    printf("time %.1f s\n", (now_ms() - t0) / 1e3);

    if (getenv("G3N_BENCH")) {
        /* 24 MB of texture-like and game data at level 6, the game's chunking, both ways */
        const uint32_t n = 24u << 20;
        for (uint32_t i = 0; i < n; i += 1u << 20)
            fill_source(src_buf + i, 1u << 20, i % (3u << 20) == 0 ? 5 : 4);
        uint32_t nc = (uint32_t)compress_host(src_buf, n, cmp_buf, IN_MAX, 6, 15, 8, 0, 0, NULL, 0);
        double ms[2];
        for (int on = 0; on < 2; ++on) {
            memcpy(g_mem + IN, cmp_buf, nc);
            init_stream(1);
            wr32(STRM, IN);
            wr32(STRM + 0xcu, OUT);
            const double b0 = now_ms();
            for (;;) {
                const uint32_t used = rd32(STRM) - IN;
                wr32(STRM + 4u, nc - used < 0x20000u ? nc - used : 0x20000u);
                wr32(STRM + 0x10u, n - (rd32(STRM + 0xcu) - OUT));
                X86 c;
                dt_context(&c, ESP0, 0);
                wr32(ESP0, 0x10da35beu);
                wr32(ESP0 + 4u, STRM);
                g3_opt = on ? 0xffffffffu : 0u;
                g3n_10daa430(&c);
                if (c.r[R_EAX] != 0u)
                    break;
            }
            ms[on] = now_ms() - b0;
            if (rd32(STRM + 0x14u) != n || memcmp(g_mem + OUT, src_buf, n))
                printf("bench: output differs (%s)\n", on ? "native" : "original");
        }
        printf("bench: %u -> %u bytes, original %.1f ms (%.1f MB/s), native %.1f ms (%.1f MB/s), %.1fx\n", nc, n, ms[0],
               n / 1e3 / ms[0], ms[1], n / 1e3 / ms[1], ms[0] / ms[1]);
        /* the archive's own streams, each inflated whole as the game does, 5 rounds */
        double pms[2] = {0, 0};
        uint64_t pin = 0, pout = 0;
        const int rounds = getenv("G3N_BENCH_ROUNDS") ? atoi(getenv("G3N_BENCH_ROUNDS")) : 5;
        for (int on = 0; on < 2; ++on)
            for (int round = 0; round < (on ? rounds : 5); ++round)
                for (unsigned i = 0; i < npaks; ++i) {
                    memcpy(g_mem + IN, pak_buf + paks[i].off, paks[i].nc);
                    init_stream(1);
                    wr32(STRM, IN);
                    wr32(STRM + 0xcu, OUT);
                    const double b0 = now_ms();
                    for (;;) {
                        const uint32_t used = rd32(STRM) - IN;
                        wr32(STRM + 4u, paks[i].nc - used < 0x20000u ? paks[i].nc - used : 0x20000u);
                        wr32(STRM + 0x10u, paks[i].nout - (rd32(STRM + 0xcu) - OUT));
                        X86 c;
                        dt_context(&c, ESP0, 0);
                        wr32(ESP0, 0x10da35beu);
                        wr32(ESP0 + 4u, STRM);
                        g3_opt = on ? 0xffffffffu : 0u;
                        g3n_10daa430(&c);
                        if (c.r[R_EAX] != 0u)
                            break;
                    }
                    pms[on] += now_ms() - b0;
                    if (on == 0 && round == 0) {
                        pin += paks[i].nc;
                        pout += paks[i].nout;
                    }
                }
        printf("bench: %u archive streams, %llu -> %llu bytes, original %.1f MB/s, native %.1f MB/s, %.1fx\n", npaks,
               (unsigned long long)pin, (unsigned long long)pout, pout * 5 / 1e3 / pms[0], pout * rounds / 1e3 / pms[1],
               (pms[0] / 5) / (pms[1] / rounds));
    }
    return dt_report("zlib_test");
}
