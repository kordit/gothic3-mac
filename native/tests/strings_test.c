/* native/tests/strings_test.c - native/strings.c against the translated SharedBase originals.
 *
 * Build and run (from the repository root or a worktree):
 *   sh native/tests/run.sh strings
 *
 * ___ascii_stricmp, _tolower, bCString::CompareFast and operator==, bCString::FindNoCase on
 * random strings (mixed case, high-bit bytes, empty, shared prefixes, lying length fields,
 * null bCStrings, negative and past-the-end starts). SharedBase's locale flag stays 0, the only
 * state in which the native _tolower and FindNoCase run. */
#include "difftest.h"

void fn_1006c3df(X86 *c);
void fn_1006c15b(X86 *c);
void fn_10001ae0(X86 *c);
void fn_10005390(X86 *c);
void fn_10004460(X86 *c);

#define DATA_LO 0x00300000u
#define DATA_HI 0x00310000u

static unsigned long hits[3]; /* stricmp equal, CompareFast equal, FindNoCase found */

static const uint8_t palette[] = {'a', 'A', 'b', 'B', 'z', 'Z', '@', '[', '`', '{', '0', ' ',
                                  0x80, 0xc1, 0xe1, 0xff, 0xc9, 0xe9, 0x7f, 0x01};

static uint8_t rnd_char(void) {
    if (rnd_below(8) == 0)
        return (uint8_t)(1u + rnd_below(255));
    return palette[rnd_below(sizeof palette)];
}

/* A NUL-terminated string of up to `max` characters at guest `a`; returns its length. */
static uint32_t put_random(uint32_t a, uint32_t max) {
    uint32_t n = rnd_below(max + 1u);
    for (uint32_t i = 0; i < n; ++i)
        wr8(a + i, rnd_char());
    wr8(a + n, 0);
    for (uint32_t i = 1; i < 8; ++i)
        wr8(a + n + i, (uint8_t)rnd());
    return n;
}

/* b as a variation of a: the same, case flipped, one byte changed, cut or extended. */
static uint32_t put_variant(uint32_t b, uint32_t a) {
    uint32_t n = 0;
    while (rd8(a + n))
        ++n;
    for (uint32_t i = 0; i <= n; ++i)
        wr8(b + i, rd8(a + i));
    switch (rnd_below(6)) {
    case 0:
        break;
    case 1:
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t ch = rd8(b + i);
            if (rnd_below(2) && ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z'))
                wr8(b + i, ch ^ 0x20);
        }
        break;
    case 2:
        if (n)
            wr8(b + rnd_below(n), rnd_char());
        break;
    case 3:
        n = rnd_below(n + 1u);
        wr8(b + n, 0);
        break;
    default: {
        uint32_t extra = 1u + rnd_below(4);
        for (uint32_t i = 0; i < extra; ++i)
            wr8(b + n + i, rnd_char());
        n += extra;
        wr8(b + n, 0);
    }
    }
    return n;
}

static void test_stricmp(void) {
    const uint32_t s1 = DATA_LO + 0x100u, s2 = DATA_LO + 0x2100u;
    for (int i = 0; i < 200000; ++i) {
        put_random(s1, 24);
        uint32_t b = s2;
        if (rnd_below(16) == 0)
            b = s1;
        else
            put_variant(s2, s1);
        uint32_t args[2] = {s1, b};
        if (rnd_below(2)) {
            args[0] = b;
            args[1] = s1;
        }
        X86 c = call_state(args, 2);
        diff_run("stricmp", fn_1006c3df, g3n_1006c3df, &c,
                 CALLEE_SAVED | REG(R_EAX) | REG(R_ECX) | REG(R_EDX), DATA_LO, DATA_HI);
        hits[0] += diff_last.r[R_EAX] == 0u;
    }
}

static void test_tolower(void) {
    for (uint32_t v = 0; v < 0x300u; ++v) {
        uint32_t vals[3] = {v, (uint32_t)(int32_t)(int8_t)v, v ^ 0x80000000u};
        for (int k = 0; k < 3; ++k) {
            X86 c = call_state(&vals[k], 1);
            diff_run("tolower", fn_1006c15b, g3n_1006c15b, &c,
                     CALLEE_SAVED | REG(R_EAX) | REG(R_ECX) | REG(R_EDX), DATA_LO, DATA_HI);
        }
    }
    for (int i = 0; i < 10000; ++i) {
        uint32_t v = rnd() * 2654435761u;
        X86 c = call_state(&v, 1);
        diff_run("tolower", fn_1006c15b, g3n_1006c15b, &c,
                 CALLEE_SAVED | REG(R_EAX) | REG(R_ECX) | REG(R_EDX), DATA_LO, DATA_HI);
    }
}

/* bCString objects at THIS_OBJ and OTHER_OBJ; their texts 12 bytes into TEXT_A and TEXT_B,
 * the length dword 8 bytes before the text. */
#define THIS_OBJ (DATA_LO + 0x4000u)
#define OTHER_OBJ (DATA_LO + 0x4010u)
#define TEXT_A (DATA_LO + 0x5000u + 12u)
#define TEXT_B (DATA_LO + 0x6000u + 12u)

static void set_bcstring(uint32_t obj, uint32_t text, uint32_t len) {
    wr32(obj, text);
    if (!text)
        return;
    wr32(text - 8u, rnd_below(32) == 0 ? len + rnd_below(3) - 1u : len);
    wr32(text - 4u, 1u + rnd_below(3));
}

static void test_compare_fast(void) {
    for (int i = 0; i < 200000; ++i) {
        uint32_t la = put_random(TEXT_A, 20);
        uint32_t lb = put_variant(TEXT_B, TEXT_A);
        uint32_t ta = TEXT_A, tb = TEXT_B;
        switch (rnd_below(12)) {
        case 0:
            ta = 0;
            break;
        case 1:
            tb = 0;
            break;
        case 2:
            ta = tb = 0;
            break;
        case 3:
            tb = TEXT_A, lb = la;
            break;
        }
        set_bcstring(THIS_OBJ, ta, la);
        set_bcstring(OTHER_OBJ, tb, lb);
        uint32_t arg = OTHER_OBJ;
        X86 c = call_state(&arg, 1);
        c.r[R_ECX] = THIS_OBJ;
        int op = rnd_below(2);
        diff_run(op ? "operator==" : "CompareFast", op ? fn_10005390 : fn_10001ae0,
                 op ? g3n_10005390 : g3n_10001ae0, &c,
                 CALLEE_SAVED | REG(R_EAX) | REG(R_ECX) | REG(R_EDX), DATA_LO, DATA_HI);
        hits[1] += (diff_last.r[R_EAX] & 0xffu) == 1u;
    }
}

static void test_find_nocase(void) {
    const uint32_t sub_at = DATA_LO + 0x7000u;
    for (int i = 0; i < 300000; ++i) {
        /* text before the string so a negative start reads something */
        for (uint32_t a = TEXT_A - 12u - 8u; a < TEXT_A - 8u; ++a)
            wr8(a, rnd_char());
        uint32_t len = put_random(TEXT_A, 40);
        uint32_t text = rnd_below(24) == 0 ? 0u : TEXT_A;
        set_bcstring(THIS_OBJ, text, len);
        uint32_t sub = sub_at;
        if (rnd_below(3) == 0 && len) {
            /* a piece of the text, maybe case flipped, maybe changed */
            uint32_t from = rnd_below(len), n = rnd_below(len - from + 1u);
            for (uint32_t k = 0; k < n; ++k)
                wr8(sub + k, rd8(TEXT_A + from + k));
            wr8(sub + n, 0);
            put_variant(sub_at + 0x100u, sub);
            if (rnd_below(2))
                sub = sub_at + 0x100u;
        } else {
            put_random(sub, 4);
        }
        if (rnd_below(40) == 0)
            sub = 0;
        int32_t start = (int32_t)rnd_below(len + 4u) - 2;
        if (rnd_below(4) == 0)
            start = 0;
        uint32_t args[2] = {sub, (uint32_t)start};
        X86 c = call_state(args, 2);
        c.r[R_ECX] = THIS_OBJ;
        diff_run("FindNoCase", fn_10004460, g3n_10004460, &c, CALLEE_SAVED | REG(R_EAX), DATA_LO,
                 DATA_HI);
        hits[2] += diff_last.r[R_EAX] != 0xffffffffu;
    }
}

int main(void) {
    harness_mem();
    map_module("SharedBase.dll");
    if (rd32(0x100d40f0u) != 0u) {
        fprintf(stderr, "SharedBase __locale_changed is not 0 in the image\n");
        return 2;
    }
    test_stricmp();
    test_tolower();
    test_compare_fast();
    test_find_nocase();
    printf("equal stricmp %lu, equal CompareFast %lu, FindNoCase found %lu\n", hits[0], hits[1], hits[2]);
    return diff_report("strings");
}
