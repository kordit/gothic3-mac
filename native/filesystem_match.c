/* native/filesystem_match.c - the name match of FileSystem.dll's finds (FUN_10da9e10 with FUN_10da9d30), for
 * native/filesystem_core.cpp and for the differential test native/tests/fsmatch_test.c.
 *
 * The wanted name (the pattern) and the entry's name are split at their first '.', and base matches base and
 * extension matches extension, each without backtracking: '?' and an equal character move both on, '*' skips the '*'
 * and '?' that follow it and then the name up to the first character equal to the pattern's next one. Characters
 * compare folded through the module's CRT _tolower in the C locale (A-Z only). The original copies each half into a
 * 256-byte stack buffer and overruns it for a longer one; here a half is cut at 255 bytes instead. */
#include <stddef.h>

static char fm_fold(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int fm_half(const char *p, const char *n) {
    if (*p != 0)
        while (*n != 0) {
            const char a = *p;
            if (a == '*') {
                char ch = a;
                while (ch != 0 && (ch == '*' || ch == '?')) {
                    ch = p[1];
                    ++p;
                }
                const char t = fm_fold(*p);
                while (*n != 0 && fm_fold(*n) != t)
                    ++n;
            } else if (a == '?' || a == *n || fm_fold(a) == fm_fold(*n)) {
                ++p;
                ++n;
            } else
                return 0;
            if (*p == 0)
                break;
        }
    while (*p == '*')
        ++p;
    return *p == 0 && *n == 0;
}

static void fm_split(const char *s, char *base, char *ext) {
    size_t i = 0, j = 0;
    while (*s != 0 && *s != '.') {
        if (i < 255)
            base[i++] = *s;
        ++s;
    }
    base[i] = 0;
    if (*s == '.')
        for (++s; *s != 0; ++s)
            if (j < 255)
                ext[j++] = *s;
    ext[j] = 0;
}

int g3fs_name_fits(const char *pattern, const char *name) {
    char pb[256], pe[256], nb[256], ne[256];
    fm_split(pattern, pb, pe);
    fm_split(name, nb, ne);
    return fm_half(pb, nb) && fm_half(pe, ne);
}
