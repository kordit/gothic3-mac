/* native/tests/ai_test.c - native/ai.c against the translation.
 *
 * Build and run (from the checkout root):
 *   sh native/tests/build.sh ai aux-filesystem/chunk_10da8000.c aux-sharedbase/chunk_10068000.c
 * (the first chunk holds FUN_10da9e10 and FUN_10da9d30, the second SharedBase _strrchr).
 * G3N_BENCH=1 adds timings.
 *
 * FileSystem's _tolower 10db08d6 is a fake defined here. While the CRT's locale-changed flag is
 * clear it does what the real one does then (EAX = the argument, ECX = EAX - 0x41, +0x20 for
 * 'A'..'Z'); with the flag set it stands for the locale path: it appends a record of what it was
 * handed (return address, ESP, EAX..EDI, its argument) to a log in guest memory and answers EAX,
 * ECX, EDX from a hash of the case seed, its position in the call sequence and those inputs. The
 * log is compared memory, so another order, argument, ESP or register fails the case.
 *
 * Every case runs the original and the native from the same state (random registers, x87, XMM)
 * and compares registers, x87 state, XMM and memory bitwise (the stack from the final ESP up; the
 * four 256-byte buffers below it are dead after the RET, but 10da9e10's overruns of its own buffers
 * are compared through the names' bytes they feed back into the match). Names are drawn so that
 * every path is taken: equal names, names that differ only in case, '*' and '?' in the wanted
 * name and in the entry name, several dots, no dot, empty parts, bytes >= 0x80 (MOVSX), and parts
 * longer than their buffer. */
#include "difftest_rp.h"

#include <time.h>

void fn_10da9e10(X86 *c);
void g3n_10da9e10(X86 *c);
void fn_1006b740(X86 *c);
void g3n_1006b740(X86 *c);

#define ESP0 0x0e800000u
#define WANT 0x03000000u   /* wanted name, up to 0x800 bytes */
#define NAME 0x03001000u   /* entry name */
#define STR 0x03002000u    /* strrchr's string */
#define LOG 0x03010000u    /* the fake's call log: count, then 8 words per call */
#define LOG_SIZE 0x4000u
#define LOCALE 0x10dc6b60u /* FileSystem __locale_changed */
#define RET_SITE 0x10da2a29u /* FUN_10da29f0's continuation after CALL 10da9e10 */
#define RET_SITE2 0x1000456cu /* ReverseFind's continuation after CALL _strrchr */

static uint32_t case_seed;
static unsigned paths[16];

static uint32_t mix(uint32_t h, uint32_t v) {
    h ^= v + 0x7f4a7c15u + (h << 6) + (h >> 2);
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

/* FileSystem _tolower, cdecl, RET. */
void fn_10db08d6(X86 *c) {
    const uint32_t esp = c->r[4];
    const uint32_t arg = rd32(esp + 4u);
    if (rd32(LOCALE) == 0u) {
        c->r[0] = arg;
        c->r[1] = arg - 0x41u;
        if (c->r[1] <= 0x19u)
            c->r[0] += 0x20u;
        ++paths[8];
    } else {
        const uint32_t n = rd32(LOG);
        const uint32_t e = LOG + 4u + 32u * n;
        if (e + 32u > LOG + LOG_SIZE) {
            fprintf(stderr, "call log full\n");
            abort();
        }
        wr32(LOG, n + 1u);
        const uint32_t w[8] = {rd32(esp), esp, c->r[0], c->r[1], c->r[2], c->r[3], c->r[5] ^ c->r[6], c->r[7] ^ arg};
        uint32_t h = mix(case_seed, n);
        for (unsigned i = 0; i < 8; ++i) {
            wr32(e + 4u * i, w[i]);
            h = mix(h, w[i]);
        }
        /* mostly the ASCII answer, so matches still happen; sometimes something else */
        uint32_t v = arg - 0x41u <= 0x19u ? arg + 0x20u : arg;
        if ((h & 7u) == 0)
            v = h >> 8;
        c->r[0] = v;
        c->r[1] = mix(h, 1);
        c->r[2] = mix(h, 2);
        ++paths[9];
    }
    c->eip = rd32(esp);
    c->r[4] = esp + 4u;
}

/* ---- FUN_10da29f0's world: the real SharedBase string functions, fake heap calls ---------- */

void fn_10da29f0(X86 *c);
void g3n_10da29f0(X86 *c);
void fn_10002a50(X86 *c);
void fn_10005680(X86 *c);
void fn_10001a00(X86 *c);
void fn_10004550(X86 *c);
void fn_10004a10(X86 *c);
void fn_10005040(X86 *c);
void fn_100052f0(X86 *c);
void fn_10da2a40(X86 *c);
void fn_10da29d0(X86 *c);

#define ENT 0x03003000u     /* the directory entry: vtable, +4 its path (bCString) */
#define VT 0x03003100u      /* its vtable */
#define PAT 0x03003200u     /* the wanted bCString */
#define PDATA 0x03004000u   /* the path's string block (header 8 bytes, then the text) */
#define WDATA 0x03005000u   /* the wanted string's block */
#define HEAP 0x03006000u    /* the fake Malloc's blocks; [HEAP] = next free */
#define HEAP_SIZE 0x2000u
#define MEMADMIN 0x10231a58u
#define FAKE_VCALL 0x0fd00200u /* another entry type's vtable +0x1c */

static void log_call(X86 *c, uint32_t addr, uint32_t arg_words, uint32_t *hash) {
    const uint32_t n = rd32(LOG);
    const uint32_t e = LOG + 4u + 32u * n;
    if (e + 32u > LOG + LOG_SIZE) {
        fprintf(stderr, "call log full\n");
        abort();
    }
    wr32(LOG, n + 1u);
    const uint32_t esp = c->r[4];
    uint32_t w[8] = {addr ^ rd32(esp), esp, c->r[0], c->r[1], c->r[2], c->r[3] ^ c->r[5], c->r[6] ^ c->r[7], 0};
    for (uint32_t k = 0; k < arg_words; ++k)
        w[7] = mix(w[7], rd32(esp + 4u + 4u * k));
    uint32_t h = mix(case_seed, n);
    for (unsigned i = 0; i < 8; ++i) {
        wr32(e + 4u * i, w[i]);
        h = mix(h, w[i]);
    }
    *hash = h;
}

static void fake_ret(X86 *c, uint32_t h, uint32_t pop) {
    c->r[1] = mix(h, 1);
    c->r[2] = mix(h, 2);
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + pop;
}

/* bCMemoryAdmin::GetInstance's first call (construction, atexit): the bit set, EAX = instance. */
void fn_1000c800(X86 *c) {
    if (rd8(MEMADMIN + 4u) & 1u) { /* the real one's fast path: MOV EAX,1 / TEST / JNZ / MOV EAX,inst / RET */
        c->r[0] = MEMADMIN;
        c->eip = rd32(c->r[4]);
        c->r[4] += 4u;
        return;
    }
    uint32_t h;
    log_call(c, 0x1000c800u, 0, &h);
    wr8(MEMADMIN + 4u, rd8(MEMADMIN + 4u) | 1u);
    c->r[0] = MEMADMIN;
    fake_ret(c, h, 0);
    ++paths[10];
}

/* bCMemoryAdmin::Malloc(size), thiscall, RET 4: a block from the bump heap, filled with noise. */
void fn_1000c9a0(X86 *c) {
    uint32_t h;
    log_call(c, 0x1000c9a0u, 1, &h);
    const uint32_t size = rd32(c->r[4] + 4u);
    const uint32_t p = rd32(HEAP);
    if (size > 0x400u || p + size + 8u > HEAP + HEAP_SIZE) {
        fprintf(stderr, "fake heap full\n");
        abort();
    }
    wr32(HEAP, p + ((size + 15u) & ~7u));
    for (uint32_t k = 0; k < size; ++k)
        wr8(p + k, (uint8_t)mix(h, k));
    c->r[0] = p;
    fake_ret(c, h, 4);
    ++paths[11];
}

/* MemHeap::Free(block), stdcall, RET 4: logs, then scribbles the block's first word. */
void fn_10020090(X86 *c) {
    uint32_t h;
    log_call(c, 0x10020090u, 1, &h);
    const uint32_t p = rd32(c->r[4] + 4u);
    if (p >= HEAP && p < HEAP + HEAP_SIZE)
        wr32(p, h);
    c->r[0] = mix(h, 3);
    fake_ret(c, h, 4);
    ++paths[12];
}

/* _memcpy(dst, src, n), cdecl (the game's is native, group mem). */
void g3n_10078780(X86 *c) {
    uint32_t h;
    log_call(c, 0x10078780u, 3, &h);
    const uint32_t esp = c->r[4], d = rd32(esp + 4u), s = rd32(esp + 8u), n = rd32(esp + 12u);
    for (uint32_t k = 0; k < n; ++k)
        wr8(d + k, rd8(s + k));
    c->r[0] = d;
    fake_ret(c, h, 0);
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    switch (target) {
    case 0x10002a50u: fn_10002a50(c); return;
    case 0x10005680u: fn_10005680(c); return;
    case 0x10001a00u: fn_10001a00(c); return;
    case 0x10004550u: fn_10004550(c); return;
    case 0x10004a10u: fn_10004a10(c); return;
    case 0x10005040u: fn_10005040(c); return;
    case 0x100052f0u: fn_100052f0(c); return;
    case 0x10da2a40u: fn_10da2a40(c); return;
    case 0x10da29d0u: fn_10da29d0(c); return;
    case FAKE_VCALL: {
        uint32_t h;
        log_call(c, FAKE_VCALL, 0, &h);
        c->r[0] = (h & 0xffffff00u) | ((h >> 9) & 1u);
        fake_ret(c, h, 0);
        ++paths[13];
        return;
    }
    }
    fprintf(stderr, "unexpected indirect call to %08x\n", target);
    abort();
}

static const char alpha[] = "abcXYZ_09.*?.Aa";

static uint8_t pick_char(void) {
    const uint32_t k = dt_rand32() % 40u;
    if (k < 28)
        return (uint8_t)alpha[k % (sizeof alpha - 1)];
    if (k < 32)
        return (uint8_t)(0x80u + dt_rand32() % 0x80u);
    if (k < 36)
        return (uint8_t)(0x20u + dt_rand32() % 0x60u);
    return (uint8_t)(1u + dt_rand32() % 0xffu);
}

/* A name: base, maybe '.', extension, maybe more dots; part lengths mostly short, now and then
 * longer than the 256-byte buffer (only where an overrun stays inside the frame's buffers). */
static uint32_t make_name(uint8_t *out, int long_ext_ok) {
    uint32_t n = 0;
    const uint32_t kb = dt_rand32() % 16u;
    uint32_t base = kb < 12 ? dt_rand32() % 12u : kb < 15 ? dt_rand32() % 40u : 250u + dt_rand32() % 30u;
    for (uint32_t i = 0; i < base; ++i) {
        uint8_t ch = pick_char();
        if (ch == '.' && (dt_rand32() & 1))
            ch = 'q';
        out[n++] = ch;
    }
    if (dt_rand32() % 4u) {
        out[n++] = '.';
        const uint32_t ke = dt_rand32() % 16u;
        uint32_t ext = ke < 12 ? dt_rand32() % 5u : ke < 15 ? dt_rand32() % 20u : (long_ext_ok ? 250u + dt_rand32() % 30u : 200u);
        for (uint32_t i = 0; i < ext; ++i)
            out[n++] = pick_char();
    }
    /* the whole wanted extension (everything after the first dot) must fit its buffer */
    out[n] = 0;
    return n;
}

/* The wanted name made from the entry name: equal, case flipped, wildcards put in. */
static uint32_t derive(uint8_t *out, const uint8_t *name, uint32_t len) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < len && n < 0x200u; ++i) {
        uint8_t ch = name[i];
        const uint32_t k = dt_rand32() % 24u;
        if (k == 0 && ch >= 'a' && ch <= 'z')
            ch -= 0x20;
        else if (k == 1 && ch >= 'A' && ch <= 'Z')
            ch += 0x20;
        else if (k == 2)
            ch = '?';
        else if (k == 3) {
            out[n++] = '*';
            const uint32_t skip = dt_rand32() % 4u;
            i += skip;
            continue;
        } else if (k == 4)
            out[n++] = '*';
        else if (k == 5)
            continue;
        out[n++] = ch;
    }
    out[n] = 0;
    return n;
}

static int wanted_ext_fits(const uint8_t *s) {
    const uint8_t *d = (const uint8_t *)strchr((const char *)s, '.');
    return !d || strlen((const char *)d + 1) < 0x100u;
}

static void put_str(uint32_t at, const uint8_t *s, uint32_t n) {
    memcpy(g_mem + at, s, n + 1u);
    /* bytes after the NUL that a wrong reader would pick up */
    for (uint32_t k = n + 1u; k < n + 9u; ++k)
        g_mem[at + k] = (uint8_t)("q.Q*x?.z"[k & 7u]);
}

static void test_match(unsigned cases) {
    uint8_t name[0x800], want[0x800];
    for (unsigned i = 0; i < cases; ++i) {
        case_seed = dt_rand32();
        wr32(LOG, 0);
        wr32(LOCALE, (dt_rand32() % 8u) == 0 ? 1u : 0u);
        uint32_t nl = make_name(name, 1);
        uint32_t wl;
        do {
            if (dt_rand32() % 3u)
                wl = derive(want, name, nl);
            else
                wl = make_name(want, 0);
        } while (!wanted_ext_fits(want));
        uint32_t wa = WANT, na = NAME;
        const uint32_t k = dt_rand32() % 64u;
        if (k == 0)
            na = wa; /* the same string */
        put_str(WANT, want, wl);
        put_str(NAME, name, nl);
        X86 c;
        dt_context(&c, ESP0 - 4u, dt_rand32() % 4u);
        wr32(ESP0 - 4u, RET_SITE);
        c.r[0] = wa;
        c.r[2] = na;
        if (k == 1)
            c.r[0] = 0x03000800u, wr8(0x03000800u, 0); /* empty wanted name */
        dt_run("10da9e10", &c, fn_10da9e10, g3n_10da9e10);
        ++paths[c.r[0] & 1u];
        if (rd32(LOG))
            ++paths[2];
    }
}

static void test_strrchr(unsigned cases) {
    uint8_t s[0x400];
    for (unsigned i = 0; i < cases; ++i) {
        uint32_t n = dt_rand32() % 8u ? dt_rand32() % 40u : dt_rand32() % 0x300u;
        for (uint32_t k = 0; k < n; ++k)
            s[k] = (uint8_t)("ab/c\\d./"[dt_rand32() % 8u]);
        if (dt_rand32() % 4u == 0)
            for (uint32_t k = 0; k < n; ++k)
                s[k] = (uint8_t)(1u + dt_rand32() % 0xffu);
        put_str(STR, s, n);
        X86 c;
        dt_context(&c, ESP0 - 12u, dt_rand32() % 4u);
        wr32(ESP0 - 12u, RET_SITE2);
        wr32(ESP0 - 8u, STR + (dt_rand32() % 8u == 0 && n ? dt_rand32() % n : 0u));
        uint32_t ch;
        switch (dt_rand32() % 6u) {
        case 0: ch = '/'; break;
        case 1: ch = 0; break;
        case 2: ch = n ? s[dt_rand32() % n] : 'a'; break;
        case 3: ch = dt_rand32(); break; /* garbage above the low byte */
        case 4: ch = 0xffffff00u | (n ? s[dt_rand32() % n] : 0u); break;
        default: ch = dt_rand32() & 0xffu; break;
        }
        wr32(ESP0 - 4u, ch);
        c.eflags_df = 0;
        dt_run("1006b740", &c, fn_1006b740, g3n_1006b740);
        ++paths[c.r[0] ? 4 : 5];
    }
}

static void test_off(unsigned cases) {
    uint8_t name[0x800];
    for (unsigned i = 0; i < cases; ++i) {
        uint32_t nl = make_name(name, 0);
        put_str(WANT, name, nl);
        put_str(NAME, name, nl);
        X86 c;
        dt_context(&c, ESP0 - 4u, 0);
        wr32(ESP0 - 4u, RET_SITE);
        c.r[0] = WANT;
        c.r[2] = NAME;
        X86 o = c;
        dt_save(0);
        g3_opt = 0;
        fn_10da9e10(&o);
        dt_save(1);
        dt_restore();
        g3_opt = 0;
        g3n_10da9e10(&c);
        dt_compare("10da9e10 off", &o, &c);
    }
}

/* A bCString block at `at`: length, reference count (16 bits) and noise in the other half, text. */
static uint32_t put_bstr(uint32_t at, const uint8_t *s, uint32_t n) {
    uint32_t len = n;
    const uint32_t k = dt_rand32() % 32u;
    if (k == 0)
        len = 0; /* text, but a zero length (IsEmpty and Right go by the length) */
    else if (k == 1 && n > 2)
        len = n - 1u - dt_rand32() % (n - 1u);
    wr32(at, len);
    const uint32_t rc = dt_rand32() % 8u == 0 ? dt_rand32() % 3u : 1u + dt_rand32() % 3u;
    wr32(at + 4u, (dt_rand32() & 0xffff0000u) | rc);
    put_str(at + 8u, s, n);
    return at + 8u;
}

static void test_entry(unsigned cases) {
    static const char *dirs[] = {"", "Data/Sound/Speech/", "a/", "x/y/", "/", "no_slash_dir\\"};
    uint8_t name[0x800], want[0x800], path[0x900];
    static const uint32_t iat[][2] = {{0x10dbc1b4u, 0x10002a50u}, {0x10dbc224u, 0x10005680u}, {0x10dbc1f0u, 0x10001a00u},
                                      {0x10dbc1acu, 0x10004550u}, {0x10dbc1a8u, 0x10004a10u}, {0x10dbc210u, 0x10005040u},
                                      {0x10dbc204u, 0x100052f0u}};
    for (unsigned k = 0; k < 7; ++k)
        wr32(iat[k][0], iat[k][1]);
    /* a name at guest address 0, so that reading a null string pointer as "" would show */
    memcpy(g_mem, "zz.q", 5);
    for (unsigned i = 0; i < cases; ++i) {
        case_seed = dt_rand32();
        wr32(LOG, 0);
        wr32(HEAP, HEAP + 0x10u);
        wr32(LOCALE, (dt_rand32() % 16u) == 0 ? 1u : 0u);
        wr8(MEMADMIN + 4u, dt_rand32() % 16u == 0 ? 0u : 1u);
        uint32_t nl = make_name(name, 0);
        if (nl > 0x300u)
            nl = 0x300u, name[nl] = 0;
        const char *dir = dirs[dt_rand32() % 6u];
        const uint32_t dl = (uint32_t)strlen(dir);
        memcpy(path, dir, dl);
        memcpy(path + dl, name, nl + 1u);
        if (dt_rand32() % 16u == 0)
            path[dl + nl] = '/', path[dl + nl + 1u] = 0, ++nl; /* ends with a slash: an empty name */
        uint32_t wl;
        do {
            wl = dt_rand32() % 3u ? derive(want, name, nl) : make_name(want, 0);
        } while (!wanted_ext_fits(want));
        wr32(ENT, VT);
        wr32(VT + 0x1cu, dt_rand32() % 16u == 0 ? FAKE_VCALL : 0x10da29d0u);
        wr32(VT + 0x20u, 0x10da29f0u);
        wr32(VT + 0x24u, 0x10da2a40u);
        wr32(ENT + 4u, dt_rand32() % 32u == 0 ? 0u : put_bstr(PDATA, path, dl + nl));
        wr32(PAT, dt_rand32() % 32u == 0 ? 0u : put_bstr(WDATA, want, wl));
        X86 c;
        dt_context(&c, ESP0 - 8u, dt_rand32() % 4u);
        wr32(ESP0 - 8u, 0x10da48e3u); /* FUN_10da4800's continuation after CALL [vtable+0x20] */
        wr32(ESP0 - 4u, PAT);
        c.r[1] = ENT;
        dt_run("10da29f0", &c, fn_10da29f0, g3n_10da29f0);
        ++paths[14 + (c.r[0] & 1u)];
    }
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* A sound file name against a directory's worth of other names, as FindFirstFileA asks. */
static void bench(void) {
    static const char *names[] = {"SFX_Hit_Sword_Metal_01.wav", "Info_Ardea_Bauer_02_06.wav",
                                  "ORC_Attack_Scream_03.wav", "Speech_Hero_Angry_14.wav"};
    const char *want = "Info_Ardea_Bauer_02_07.wav";
    wr32(LOCALE, 0);
    put_str(WANT, (const uint8_t *)want, (uint32_t)strlen(want));
    double t[2];
    for (int v = 0; v < 2; ++v) {
        g3_opt = v ? 0xffffffffu : 0u;
        const int n = 2000000;
        const double t0 = now();
        for (int i = 0; i < n; ++i) {
            const char *nm = names[i & 3];
            memcpy(g_mem + NAME, nm, strlen(nm) + 1);
            X86 c;
            memset(&c, 0, sizeof c);
            c.r[4] = ESP0 - 4u;
            wr32(ESP0 - 4u, RET_SITE);
            c.r[0] = WANT;
            c.r[2] = NAME;
            g3n_10da9e10(&c);
        }
        t[v] = (now() - t0) * 1e9 / n;
    }
    printf("bench 10da9e10, ns per call: translation %.1f, native %.1f\n", t[0], t[1]);
    put_str(STR, (const uint8_t *)"Data/Sound/Speech/Info_Ardea_Bauer_02_07.wav", 44);
    for (int v = 0; v < 2; ++v) {
        g3_opt = v ? 0xffffffffu : 0u;
        const int n = 2000000;
        const double t0 = now();
        for (int i = 0; i < n; ++i) {
            X86 c;
            memset(&c, 0, sizeof c);
            c.r[4] = ESP0 - 12u;
            wr32(ESP0 - 12u, RET_SITE2);
            wr32(ESP0 - 8u, STR);
            wr32(ESP0 - 4u, '/');
            g3n_1006b740(&c);
        }
        t[v] = (now() - t0) * 1e9 / n;
    }
    printf("bench _strrchr, ns per call: translation %.1f, native %.1f\n", t[0], t[1]);
    const char *path = "Data/Sound/Speech/Info_Ardea_Bauer_02_06.wav";
    wr8(MEMADMIN + 4u, 1u);
    for (int v = 0; v < 2; ++v) {
        g3_opt = v ? 0xffffffffu : 0u;
        const int n = 1000000;
        const double t0 = now();
        for (int i = 0; i < n; ++i) {
            wr32(LOG, 0);
            wr32(HEAP, HEAP + 0x10u);
            wr32(PDATA, (uint32_t)strlen(path));
            wr32(PDATA + 4u, 1u);
            memcpy(g_mem + PDATA + 8u, path, strlen(path) + 1);
            wr32(WDATA, (uint32_t)strlen(want));
            wr32(WDATA + 4u, 1u);
            memcpy(g_mem + WDATA + 8u, want, strlen(want) + 1);
            wr32(ENT, VT);
            wr32(VT + 0x1cu, 0x10da29d0u);
            wr32(VT + 0x24u, 0x10da2a40u);
            wr32(ENT + 4u, PDATA + 8u);
            wr32(PAT, WDATA + 8u);
            X86 c;
            memset(&c, 0, sizeof c);
            c.r[4] = ESP0 - 8u;
            wr32(ESP0 - 8u, 0x10da48e3u);
            wr32(ESP0 - 4u, PAT);
            c.r[1] = ENT;
            g3n_10da29f0(&c);
        }
        t[v] = (now() - t0) * 1e9 / n;
    }
    printf("bench 10da29f0 (Malloc, Free, memcpy are fakes), ns per call: translation %.1f, native %.1f\n", t[0], t[1]);
}

int main(void) {
    harness_mem();
    dt_load_pe("FileSystem.dll");
    dt_load_pe("SharedBase.dll");
    dt_stack_floor = ESP0 - 0x800u;
    dt_region(dt_stack_floor, 0x900u);
    dt_region(WANT, 0x900u);
    dt_region(NAME, 0x900u);
    dt_region(STR, 0x400u);
    dt_region(LOG, LOG_SIZE);
    dt_region(LOCALE, 4u);
    dt_region(ENT, 0x400u);
    dt_region(PDATA, 0x1000u);
    dt_region(WDATA, 0x1000u);
    dt_region(HEAP, HEAP_SIZE);
    dt_region(MEMADMIN, 8u);

    test_match(400000);
    test_strrchr(200000);
    test_off(2000);
    test_entry(300000);
    printf("paths: match 0/1 %u/%u, cases with locale-path calls %u, strrchr found/none %u/%u, "
           "tolower fast/locale %u/%u\n",
           paths[0], paths[1], paths[2], paths[4], paths[5], paths[8], paths[9]);
    printf("10da29f0: false/true %u/%u, GetInstance first calls %u, Malloc %u, Free %u, other entry type %u\n",
           paths[14], paths[15], paths[10], paths[11], paths[12], paths[13]);
    if (getenv("G3N_BENCH"))
        bench();
    return dt_report("ai_test");
}
