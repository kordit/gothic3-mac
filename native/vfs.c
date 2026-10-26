/* native/vfs.c - the virtual file system's directory scan (FileSystem.dll), stage 6 of the night plan.
 *
 * An NPC's shout opens its speech file: eCAudioStreamer::OpenFile -> eCVirtualFileSystem::GetFileSize
 * -> CFFFileSystemModule::FindFirstFileA 10da1cc0 -> ... -> 10da4800, the loop that walks one
 * directory node entry by entry. For every entry it calls the entry's virtual Match (+0x20,
 * 10da29f0 for archive files), which builds a temporary bCString of the entry's name (GetName
 * +0x24, 10da2a40: the part of the path after the last '/'), splits name and pattern at the first
 * '.', and matches both halves with the wildcard compare 10da9d30. In the speech directory that is
 * thousands of entries with two string allocations each, 6% of the main thread in combat
 * (docs/perf/combat-profile.md).
 *
 * g3n_10da4800 walks the same entries in the same order and decides "no match" natively for an
 * entry whose vtable has the archive file's IsValid (+0x1c 10da29d0), Match and GetName and whose
 * name and pattern are plain ASCII under 256 bytes per half. At the first entry it cannot decide
 * that way (".", "..", another class, a match, non-ASCII, a long name) it stores the index before
 * it and runs the translated loop from there, so everything with an effect (the find data the
 * entry fills, the "." / ".." answers) is the original's. What the native skip leaves out is only
 * the temporaries: Match's local bCString and GetName's Right() copy, each allocated and freed in
 * the same call, and the dead stack below ESP. Callers test AL only (10da47ad, 10da68d5) and treat
 * ECX and EDX as scratch, so a no-match return sets AL = 0 and keeps EBX ESI EDI EBP.
 *
 * Locale: 10da9d30 folds through FileSystem's CRT _tolower; for ASCII that is A-Z in every locale
 * the game can set, which is why bytes from 0x80 up go to the original. */
#include "chunk.h"
#include "g3_opt.h"

void fn_10da4800(X86 *c); /* FileSystem directory scan (FindFirst/FindNext helper) */

#define VFS_ISVALID 0x10da29d0u /* entry vtable +0x1c: !IsEmpty(path) */
#define VFS_MATCH 0x10da29f0u   /* entry vtable +0x20: wildcard match of the name */
#define VFS_GETNAME 0x10da2a40u /* entry vtable +0x24: path after the last '/' */

/* The halves 10da9e10 copies into its 256-byte stack buffers: up to the first '.', and the rest
 * after it. Returns 0 when a half would not fit (the original overruns its frame then) or a byte is
 * not ASCII. */
static int split(uint32_t s, uint32_t lim, uint32_t *base, uint32_t *nbase, uint32_t *ext, uint32_t *next) {
    /* bytes at and past `lim` read as the terminator (Right() copies `lim` bytes and ends there) */
    uint32_t i = 0;
    for (;; ++i) {
        const uint8_t ch = i < lim ? rd8(s + i) : 0u;
        if (ch == 0u || ch == '.')
            break;
        if (ch >= 0x80u || i >= 255u)
            return 0;
    }
    *base = s;
    *nbase = i;
    if (i >= lim || rd8(s + i) != '.') {
        *ext = s + i;
        *next = 0;
        return 1;
    }
    const uint32_t e = s + i + 1u;
    uint32_t j = 0;
    for (;; ++j) {
        const uint8_t ch = i + 1u + j < lim ? rd8(e + j) : 0u;
        if (ch == 0u)
            break;
        if (ch >= 0x80u || j >= 255u)
            return 0;
    }
    *ext = e;
    *next = j;
    return 1;
}

static inline uint8_t lower(uint8_t ch) {
    return ch - 'A' <= 'Z' - 'A' ? (uint8_t)(ch + 0x20) : ch;
}

/* 10da9d30 on two counted ASCII strings: '?' any one char, '*' skips to the first occurrence of
 * the next pattern char (no backtracking), case folded; trailing '*' match the end. */
static int wild(const uint8_t *p, uint32_t np, const uint8_t *s, uint32_t ns) {
    uint32_t i = 0, k = 0;
    for (;;) {
        if (i >= np || k >= ns) {
            while (i < np && p[i] == '*')
                ++i;
            return i >= np && k >= ns;
        }
        if (p[i] == '*') {
            while (i < np && (p[i] == '*' || p[i] == '?'))
                ++i;
            const uint8_t t = i < np ? lower(p[i]) : 0u;
            while (k < ns && lower(s[k]) != t)
                ++k;
        } else {
            if (p[i] != '?' && p[i] != s[k] && lower(p[i]) != lower(s[k]))
                return 0;
            ++i;
            ++k;
        }
    }
}

static void fetch(uint8_t *dst, uint32_t src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        dst[i] = rd8(src + i);
}

/* 1 = this entry certainly does not match, 0 = let the original decide. */
static int no_match(uint32_t entry, const uint8_t *pb, uint32_t npb, const uint8_t *pe, uint32_t npe) {
    const uint32_t vt = rd32(entry);
    if (rd32(vt + 0x1cu) != VFS_ISVALID || rd32(vt + 0x20u) != VFS_MATCH || rd32(vt + 0x24u) != VFS_GETNAME)
        return 0;
    /* GetName: bCString at entry+4 (char *, length at [p-8]); empty or invalid gives "" */
    const uint32_t p = rd32(entry + 4u);
    uint32_t name = 0, len = 0;
    if (p != 0u && (len = rd32(p - 8u)) != 0u) {
        /* ReverseFind('/') is strrchr on the C string; Right(len - pos - 1) the tail from pos + 1 */
        uint32_t slash = 0xffffffffu;
        for (uint32_t i = 0;; ++i) {
            const uint8_t ch = rd8(p + i);
            if (ch == 0u)
                break;
            if (ch == '/')
                slash = i;
            if (i > 0x10000u)
                return 0;
        }
        /* Right(n): n < 0 copies the whole C string, n >= length shares it, else the last n bytes */
        const int32_t n = (int32_t)(len - slash - 1u);
        if (n < 0 || (uint32_t)n >= len) {
            name = p;
            len = 0xffffffffu;
        } else {
            name = p + len - (uint32_t)n;
            len = (uint32_t)n;
        }
    }
    uint32_t nb, nnb, ne, nne;
    if (name == 0u) {
        nnb = nne = 0;
    } else if (!split(name, len, &nb, &nnb, &ne, &nne)) {
        return 0;
    }
    uint8_t b[256], e[256];
    if (nnb)
        fetch(b, nb, nnb);
    if (nne)
        fetch(e, ne, nne);
    if (wild(pb, npb, b, nnb) && wild(pe, npe, e, nne))
        return 0; /* a match: the original calls the entry's fill-in */
    return 1;
}

/* bool __usercall scan(handle EAX, node [ESP+4], find data [ESP+8]), RET 8. handle+0x10 is the
 * index (-3 before the first call, -2 ".", -1 "..", then node+0x30 subdirectories from node+0x2c
 * and node+0x3c files from node+0x38), handle+8 the pattern bCString. */
void g3n_10da4800(X86 *c) {
    const uint32_t h = c->r[R_EAX];
    if (!G3OPT(VFS) || h == 0u) {
        fn_10da4800(c);
        return;
    }
    const uint32_t sp = c->r[R_ESP];
    const uint32_t node = rd32(sp + 4u);
    const int32_t nsub = (int32_t)rd32(node + 0x30u);
    const int32_t limit = nsub + (int32_t)rd32(node + 0x3cu);

    uint32_t pat = rd32(h + 8u);
    uint32_t plen = pat ? rd32(pat - 8u) : 0u;
    uint32_t pb = 0, npb = 0, pe = 0, npe = 0;
    if (pat == 0u)
        plen = 0u;
    else if (!split(pat, 0xffffffffu, &pb, &npb, &pe, &npe)) {
        fn_10da4800(c);
        return;
    }
    const uint32_t pc = pat ? pat : 0u;
    const int star_star = pc != 0u && rd8(pc) == '*' && rd8(pc + 1u) == '.' && rd8(pc + 2u) == '*' && rd8(pc + 3u) == 0u;
    (void)plen;
    uint8_t b[256], e[256];
    if (npb)
        fetch(b, pb, npb);
    if (npe)
        fetch(e, pe, npe);

    int32_t idx = (int32_t)rd32(h + 0x10u);
    for (;;) {
        const int32_t next = idx + 1;
        if (next >= limit) {
            /* the original's exit: index stored as limit, AL = 0 */
            wr32(h + 0x10u, (uint32_t)next);
            c->r[R_EAX] &= 0xffffff00u;
            c->eip = rd32(sp);
            c->r[R_ESP] = sp + 12u;
            recomp_return(c);
            return;
        }
        if (next < 0) {
            /* "." (-2) and ".." (-1) answer only when the node has a parent (+0x48) and the
             * pattern is exactly "*.*" (operator!= with "*.*", no letters to fold) */
            if (next < -2 || (rd32(node + 0x48u) != 0u && star_star))
                break;
            idx = next;
            continue;
        }
        const uint32_t arr = next < nsub ? rd32(node + 0x2cu) : rd32(node + 0x38u);
        const uint32_t entry = rd32(arr + 4u * (uint32_t)(next < nsub ? next : next - nsub));
        if (!no_match(entry, b, npb, e, npe))
            break;
        idx = next;
    }
    wr32(h + 0x10u, (uint32_t)idx);
    fn_10da4800(c);
}
