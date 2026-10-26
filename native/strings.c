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

void fn_1006c3df(X86 *c); /* SharedBase ___ascii_stricmp */
void fn_1006c15b(X86 *c); /* SharedBase _tolower */
void fn_10001ae0(X86 *c); /* SharedBase bCString::CompareFast */
void fn_10005390(X86 *c); /* SharedBase bCString::operator== (JMP 10001ae0) */
void fn_10004460(X86 *c); /* SharedBase bCString::FindNoCase */

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
