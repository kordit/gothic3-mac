/* native/strings.c - stage 6: string comparison (PUP fast_ascii_stricmp).
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h). Until its native
 * version is written it only calls the original; a native version runs under G3OPT(STR) and
 * must end as the translated function does (see its RET in build/recomp/gen/aux-<module>/).
 *
 * The native versions walk guest memory through rd8/rd32 with 32-bit guest addresses, so a
 * pointer wraps and a null-check build faults exactly as the translated code does. They leave
 * EAX, ECX and EDX as the original leaves them (FindNoCase: EAX only; ECX and EDX are scratch
 * to every caller of compiled code), keep EBX ESI EDI EBP, and do not write the original's
 * saved registers below ESP, which nothing reads after the RET. Flags are not set: the callers
 * are compiled C++ and test AL or EAX after the call.
 *
 * Locale: ___ascii_stricmp folds A-Z whatever the locale, as the original does. _tolower and
 * FindNoCase (which calls _tolower per character) fold natively only while SharedBase's CRT
 * locale flag (__locale_changed at 0x100d40f0) is 0, the case where the original takes its own
 * A-Z fast path; otherwise they run the translated original, which goes to _tolower_l. */
#include "chunk.h"
#include "g3_opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fn_1006c3df(X86 *c); /* SharedBase ___ascii_stricmp */
void fn_1006c15b(X86 *c); /* SharedBase _tolower */
void fn_10001ae0(X86 *c); /* SharedBase bCString::CompareFast */
void fn_10005390(X86 *c); /* SharedBase bCString::operator== (JMP 10001ae0) */
void fn_10004460(X86 *c); /* SharedBase bCString::FindNoCase */
void fn_10a73590(X86 *c); /* Game: a bCString hash map's lookup */

#define SB_LOCALE_CHANGED 0x100d40f0u /* SharedBase __locale_changed; _tolower tests it */
#define SB_EMPTY_STRING 0x1008b4e4u   /* the "" FindNoCase searches when the bCString is empty */

static inline uint32_t fold_az(uint32_t v) {
    return v - 0x41u <= 0x19u ? v + 0x20u : v;
}

/* int __ascii_stricmp(const char *a, const char *b), cdecl. EAX = fold(a[i]) - fold(b[i]) at the
 * first difference or at a's NUL; ECX = fold(b[i]); EDX = &b[i + 1]. */
void g3n_1006c3df(X86 *c) {
    if (!G3OPT(STR)) {
        fn_1006c3df(c);
        return;
    }
    const uint32_t sp = c->r[R_ESP];
    uint32_t a = rd32(sp + 4u), b = rd32(sp + 8u);
    uint32_t x, y;
    do {
        x = fold_az(rd8(a++));
        y = fold_az(rd8(b++));
    } while (x != 0u && x == y);
    c->r[R_EAX] = x - y;
    c->r[R_ECX] = y;
    c->r[R_EDX] = b;
    c->eip = rd32(sp);
    c->r[R_ESP] = sp + 4u;
    recomp_return(c);
}

/* int _tolower(int), cdecl. The A-Z path only; ECX = arg - 0x41 as the original leaves it. */
void g3n_1006c15b(X86 *c) {
    if (!G3OPT(STR) || rd32(SB_LOCALE_CHANGED) != 0u) {
        fn_1006c15b(c);
        return;
    }
    const uint32_t sp = c->r[R_ESP];
    const uint32_t v = rd32(sp + 4u);
    c->r[R_ECX] = v - 0x41u;
    c->r[R_EAX] = c->r[R_ECX] <= 0x19u ? v + 0x20u : v;
    c->eip = rd32(sp);
    c->r[R_ESP] = sp + 4u;
    recomp_return(c);
}

/* bool __thiscall bCString::CompareFast(const bCString &) const, RET 4. ECX = this; a bCString
 * holds a char* whose length is the dword 8 bytes before it. AL = equal, and the rest of EAX,
 * ECX and EDX as the original leaves them on each of its four exits. */
void g3n_10001ae0(X86 *c) {
    if (!G3OPT(STR)) {
        fn_10001ae0(c);
        return;
    }
    const uint32_t sp = c->r[R_ESP];
    const uint32_t other = rd32(sp + 4u);
    uint32_t a = rd32(c->r[R_ECX]);
    if (a == 0u) {
        /* both empty is equal; ECX and EDX untouched */
        c->r[R_EAX] = (other & 0xffffff00u) | (rd32(other) == 0u);
    } else {
        uint32_t b = rd32(other);
        c->r[R_ECX] = b;
        if (b == 0u) {
            c->r[R_EAX] = a & 0xffffff00u;
        } else if ((c->r[R_EDX] = rd32(b - 8u)) != rd32(a - 8u)) {
            c->r[R_EAX] = a & 0xffffff00u;
        } else {
            /* strcmp two bytes a step, as the original; ECX ends at the pair it stopped in */
            for (;;) {
                uint8_t d = rd8(a), e = rd8(b);
                if (d != e) {
                    c->r[R_EAX] = d < e ? 0xffffff00u : 0u;
                    c->r[R_EDX] = 0u;
                    break;
                }
                if (d == 0u) {
                    c->r[R_EAX] = 1u;
                    c->r[R_EDX] = 1u;
                    break;
                }
                d = rd8(a + 1u);
                e = rd8(b + 1u);
                if (d != e) {
                    c->r[R_EAX] = d < e ? 0xffffff00u : 0u;
                    c->r[R_EDX] = 0u;
                    break;
                }
                a += 2u;
                b += 2u;
                if (d == 0u) {
                    c->r[R_EAX] = 1u;
                    c->r[R_EDX] = 1u;
                    break;
                }
            }
            c->r[R_ECX] = b;
        }
    }
    c->eip = rd32(sp);
    c->r[R_ESP] = sp + 8u;
    recomp_return(c);
}

/* operator== is a JMP to CompareFast. */
void g3n_10005390(X86 *c) {
    if (!G3OPT(STR)) {
        fn_10005390(c);
        return;
    }
    g3n_10001ae0(c);
}

/* int __thiscall bCString::FindNoCase(const char *sub, int start) const, RET 8. The original's
 * search, including its quirk: after a mismatch the text position does not step back, so the
 * character that broke a partial match is not tried against sub[0]. EAX only. */
void g3n_10004460(X86 *c) {
    if (!G3OPT(STR) || rd32(SB_LOCALE_CHANGED) != 0u) {
        fn_10004460(c);
        return;
    }
    const uint32_t sp = c->r[R_ESP];
    const uint32_t buf = rd32(c->r[R_ECX]);
    const uint32_t sub = rd32(sp + 4u);
    const uint32_t start = rd32(sp + 8u);
    const uint32_t len = buf != 0u ? rd32(buf - 8u) : 0u;
    uint32_t result = 0xffffffffu;
    if ((int32_t)start < (int32_t)len && sub != 0u) {
        uint32_t e = sub;
        while (rd8(e++) != 0u)
            ;
        const int32_t slen = (int32_t)(e - (sub + 1u));
        uint32_t rem = len - start;
        if (slen <= (int32_t)rem) {
            uint32_t p = (buf != 0u ? buf : SB_EMPTY_STRING) + start;
            uint32_t q = sub;
            uint32_t found = 0xffffffffu, index = 0xffffffffu;
            if (rd8(p) != 0u) {
                for (;;) {
                    const uint8_t ch = rd8(q);
                    if (ch == 0u)
                        break;
                    index++;
                    const uint32_t x = fold_az(rd8(p));
                    p++;
                    if (x == fold_az(ch)) {
                        q++;
                        if (found == 0xffffffffu)
                            found = index;
                    } else {
                        q = sub;
                        found = 0xffffffffu;
                        if (slen >= (int32_t)rem)
                            goto done;
                    }
                    rem--;
                    if (rd8(p) == 0u)
                        break;
                }
            }
            result = found;
        }
    }
done:
    c->r[R_EAX] = result;
    c->eip = rd32(sp);
    c->r[R_ESP] = sp + 12u;
    recomp_return(c);
}

/* Game FUN_10a73590: a lookup in a hash map keyed by bCString (EAX = the map: +0 the buckets, +4 their count; EBX = the
 * key; stack: &index out; RET 4; EAX = the node or 0). A node is {bCString key, ?, next}. A quick save spends 13% of
 * its time in this loop and 15% in the CompareFast it calls per node (work/sample-f5-1.txt, session 16), because
 * the save's map has long chains. The hash is g_GetHashValue<bCString>'s (SharedBase 10055f40, which Game calls through
 * [0x10b5df70]), computed here: a guest_call per lookup made the save 60% slower; the walk is here, comparing as CompareFast does: equal when both strings are empty, or when the
 * lengths (the dword 8 bytes before the chars) match and the chars do up to the NUL. EBX, ESI, EDI and EBP are kept;
 * ECX and EDX are scratch to the callers (compiled code). A map with no buckets runs the original (its DIV faults). */
#define GM_HASH_IMPORT 0x10b5df70u /* Game's import of g_GetHashValue<bCString> */
#define GM_HASH_FN 0x10055f40u     /* where it resolves: SharedBase, computed here instead */
#define GM_EMPTY 0x1008b4e4u       /* the "" it hashes for a null string */

static inline int sb_equal(uint32_t a, uint32_t b) {
    if (a == 0u)
        return b == 0u;
    if (b == 0u)
        return 0;
    const uint32_t n = rd32(a - 8u);
    if (rd32(b - 8u) != n)
        return 0;
    const char *x = (const char *)g_mem + a, *y = (const char *)g_mem + b;
    return strcmp(x, y) == 0;
}

void g3n_10a73590(X86 *c) {
    uint32_t *const r = c->r;
    const uint32_t map = r[R_EAX];
    if (!G3OPT(STR) || rd32(map + 4u) == 0u || rd32(GM_HASH_IMPORT) != GM_HASH_FN) {
        fn_10a73590(c);
        return;
    }
    const uint32_t esp = r[R_ESP], key = r[R_EBX], out = rd32(esp + 4u);
    /* g_GetHashValue<bCString> (SharedBase 10055f40): h * 33 + each char, signed, "" for a null string. */
    uint32_t hash = 0u;
    for (const signed char *p = (const signed char *)g_mem + (rd32(key) ? rd32(key) : GM_EMPTY); *p; ++p)
        hash = hash * 33u + (uint32_t)(int32_t)*p;
    const uint32_t count = rd32(map + 4u), at = hash % count;
    wr32(out, at);
    uint32_t found = 0u;
    const uint32_t want = rd32(key);
    for (uint32_t node = rd32(rd32(map) + at * 4u); node != 0u; node = rd32(node + 8u))
        if (sb_equal(want, rd32(node))) {
            found = node;
            break;
        }
    static int check = -1;
    if (check < 0) {
        const char *v = getenv("G3_MAP_CHECK");
        check = v && *v == '1';
    }
    if (check) { /* G3_MAP_CHECK=1: the original answers, and a different answer here is logged */
        fn_10a73590(c);
        static int told;
        if (r[R_EAX] != found && told++ < 50)
            fprintf(stderr, "[g3] str: map %08x key %08x: the original found %08x, the native walk %08x\n", map, key,
                    r[R_EAX], found);
        return;
    }
    r[R_EAX] = found;
    c->eip = rd32(esp);
    r[R_ESP] = esp + 8u;
    recomp_return(c);
}
