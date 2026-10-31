/* native/arenatrace.c - G3_ARENA_TRACE=1: where an arena fighter is sent (31.10, Kap Dun: Fadi and Darmok draw their
 * weapon after Arokkh's dialog and stay where they stand instead of going into the ring).
 *
 * Two game.toml native_sites in Script_Game.dll, both only reading and going on with the original, each on the instruction after MOV EBP,ESP (a site may not sit on a function's first one):
 * native_site_10fc19a3 the entry of ZS_Arena (FUN_10fc19a0: GetCurrentDestinationPoint, _AI_Goto_Ext to it, then the
 * weapon and the alignment to the player) and native_site_10fc1f43 the entry of ZS_Arena_Loop. Each prints the NPC
 * (gCScriptProcessingUnit::GetSelfEntity, Game 10a7f090, the processing unit is the second cdecl argument, [EBP+0xc]), its world
 * position (eCEntity +0x70), its gCNavigation_PS (the entity's vtable +0xec with type 5) and from it the current
 * destination point (the entity proxy at +0x90, resolved through GetSelfEntity on proxy - 0x20, as that reads its
 * proxy at +0x20), the goto status (+0x21c) and IsOnDestination (+0x105). The loop is printed for its first 8 calls
 * per NPC. */
#include "chunk.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");

#define AT_GET_SELF 0x10a7f090u /* Game gCScriptProcessingUnit::GetSelfEntity, thiscall */
#define AT_NAV_TYPE 5u          /* gCNavigation_PS::GetPropertySetType */

static int at_on = -1;

static uint32_t at_thiscall(X86 *c, uint32_t fn, uint32_t self, const uint32_t *args, int n) {
    c->r[R_ECX] = self;
    return guest_call(c, fn, args, n);
}

static void at_name(uint32_t ent, char *out, size_t size) {
    const uint32_t text = ent ? rd32(ent + 0x138u) : 0u;
    size_t i = 0;
    for (; text && i + 1 < size && g_mem[text + i]; ++i)
        out[i] = (char)g_mem[text + i];
    out[i] = 0;
    if (!i)
        snprintf(out, size, "?%08x", ent);
}

static void at_report(X86 *c, const char *where) {
    if (at_on < 0) {
        const char *v = getenv("G3_ARENA_TRACE");
        at_on = v && *v == '1';
    }
    if (!at_on)
        return;
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    const uint32_t eip = c->eip, spu = rd32(c->r[R_EBP] + 0xcu);
    const uint32_t self = spu ? at_thiscall(c, AT_GET_SELF, spu, 0, 0) : 0u;
    uint32_t nav = 0u, dest = 0u;
    if (self) {
        const uint32_t type = AT_NAV_TYPE;
        nav = at_thiscall(c, rd32(rd32(self) + 0xecu), self, &type, 1);
    }
    if (nav)
        dest = at_thiscall(c, AT_GET_SELF, nav + 0x90u - 0x20u, 0, 0);
    memcpy(c->r, saved, sizeof saved);
    c->eip = eip;
    static uint32_t seen[64], counts[64];
    uint32_t k = 0;
    while (k < 64u && seen[k] && seen[k] != self)
        ++k;
    if (k < 64u) {
        seen[k] = self;
        if (where[0] == 'l' && ++counts[k] > 8u)
            return;
    }
    char ns[96], nd[96];
    at_name(self, ns, sizeof ns);
    at_name(dest, nd, sizeof nd);
    fprintf(stderr,
            "[g3] arena %s: %s at (%.0f,%.0f,%.0f), destination point %s at (%.0f,%.0f,%.0f), goto status %u, on "
            "destination %u\n",
            where, ns, self ? rdf32(self + 0x70u) : 0.0f, self ? rdf32(self + 0x74u) : 0.0f,
            self ? rdf32(self + 0x78u) : 0.0f, nd, dest ? rdf32(dest + 0x70u) : 0.0f, dest ? rdf32(dest + 0x74u) : 0.0f,
            dest ? rdf32(dest + 0x78u) : 0.0f, nav ? rd32(nav + 0x21cu) : 0u, nav ? rd8(nav + 0x105u) : 0u);
}

uint32_t native_site_10fc19a3(X86 *c) {
    at_report(c, "start");
    return 0;
}

uint32_t native_site_10fc1f43(X86 *c) {
    at_report(c, "loop");
    return 0;
}

/* native_site_10a7ca73: gCScriptProcessingUnit::sAIGotoItlLoop (Game 10a7ca70, static stdcall, the unit at [EBP+8])
 * after its MOV EBP,ESP. For an NPC named in G3_ARENA_TRACE_NPC (default Fadi), its first 60 calls: the goto status
 * the loop finds (+0x21c of the gCNavigation_PS at unit +0x128; it goes on only from 1 or 2), IsOnDestination, the
 * movement kind at unit +0x124 and the NPC's position. */
uint32_t native_site_10a7ca73(X86 *c) {
    if (at_on < 0) {
        const char *v = getenv("G3_ARENA_TRACE");
        at_on = v && *v == '1';
    }
    if (!at_on)
        return 0;
    static const char *who;
    static unsigned calls;
    if (!who) {
        who = getenv("G3_ARENA_TRACE_NPC");
        if (!who || !*who)
            who = "Fadi";
    }
    if (calls >= 60u)
        return 0;
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    const uint32_t eip = c->eip, spu = rd32(c->r[R_EBP] + 8u);
    const uint32_t self = spu ? at_thiscall(c, AT_GET_SELF, spu + 0x78u - 0x20u, 0, 0) : 0u;
    memcpy(c->r, saved, sizeof saved);
    c->eip = eip;
    char ns[96];
    at_name(self, ns, sizeof ns);
    if (strcmp(ns, who))
        return 0;
    ++calls;
    const uint32_t nav = rd32(spu + 0x128u);
    fprintf(stderr, "[g3] arena goto: %s at (%.0f,%.0f,%.0f), status %u, on destination %u, kind %u\n", ns,
            rdf32(self + 0x70u), rdf32(self + 0x74u), rdf32(self + 0x78u), nav ? rd32(nav + 0x21cu) : 99u,
            nav ? rd8(nav + 0x105u) : 99u, rd32(spu + 0x124u));
    return 0;
}

/* native_site_10a3efc3: gCNavigationMap::FindWay(gCNavigation_PS *) (Game 10a3efc0, thiscall: the map in ECX, the
 * navigation at [EBP+8]) after its MOV EBP,ESP. The first time it runs for the traced NPC: its destination point and
 * every nav area of the map (the gSNavArea list at map +0x14, 0x30 bytes each, its count at +0x18; an area's +0x24 is
 * 0 for a zone) that may hold that point: a zone whose radius (its set +0x20, GetZone's first test) reaches it with
 * 5 m to spare, a path whose entity lies within 40 m. G3_ARENA_TRACE_AT=x,y,z asks about that point instead;
 * G3_ARENA_TRACE_WIDE=<units> lists every zone within that distance and no paths. G3_ARENA_TRACE_NPC=* takes the first
 * NPC of any name (the map is the same for all), G3_ARENA_TRACE_AT2=x,y,z replaces the NPC's position in the second
 * GetZone. */
#define AT_GET_SET_IMPORT 0x10b5c4fcu /* Game's import of eCPropertySetProxy::GetPropertySet */
#define AT_GET_ZONE 0x10a3c620u       /* gCNavigationMap::GetZone, thiscall, 6 arguments */
#define AT_FLOAT_10C1805C 0x10c1805cu /* the float FindWay passes as GetZone's last argument */

/* G3_ARENA_TRACE_CENSUS=1 (with G3_ARENA_TRACE_AREAS): who holds the class name of the first area named there. A
 * bCString is one pointer to text shared by every copy, its count a 16-bit word at text - 4 (SharedBase 100019e0
 * adds 1 per copy, ~bCString 10001a00 frees the text when it comes down to 0), so more than 65535 copies wrap it. Every
 * aligned word of guest memory equal to the text is a bCString; every eCPropertySetProxy (vtable 106d64a8) whose +0x20
 * points at one of them is grouped by the first object above it with an RTTI vtable (its class and the proxy's offset
 * in it), the 24 largest groups printed. */
#define AT_PROXY_VTABLE 0x106d64a8u

static int at_cmp(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int at_rtti(uint32_t v, char *out, size_t size) {
    if (v < 0x10000000u || v >= 0x11000000u)
        return 0;
    const uint32_t col = rd32(v - 4u);
    if (col < 0x10000000u || col >= 0x11000000u)
        return 0;
    const uint32_t td = rd32(col + 0xcu);
    if (td < 0x10000000u || td >= 0x11000000u || memcmp(g_mem + td + 8u, ".?AV", 4))
        return 0;
    size_t i = 0;
    for (; i + 1 < size && g_mem[td + 12u + i] && g_mem[td + 12u + i] != '@'; ++i)
        out[i] = (char)g_mem[td + 12u + i];
    out[i] = 0;
    return 1;
}

static void at_census(uint32_t text) {
    if (!text)
        return;
    const uint32_t lo = 0x10000u, hi = 0xfffe0000u;
    uint32_t cap = 1u << 20, n = 0;
    uint32_t *str = malloc(cap * sizeof *str);
    for (uint32_t a = lo; str && a < hi; a += 4u)
        if (rd32(a) == text) {
            if (n == cap)
                str = realloc(str, (cap *= 2u) * sizeof *str);
            if (str)
                str[n++] = a;
        }
    if (!str)
        return;
    struct {
        char cls[48];
        int32_t off;
        unsigned n;
    } kinds[256];
    unsigned nkinds = 0u, proxies = 0u, others = 0u;
    for (uint32_t p = lo + 0x20u; n && p < hi; p += 4u) {
        const uint32_t v = rd32(p);
        if (v < str[0] || v > str[n - 1u] || !bsearch(&v, str, n, sizeof *str, at_cmp))
            continue;
        const uint32_t proxy = p - 0x20u;
        if (rd32(proxy) != AT_PROXY_VTABLE) {
            ++others;
            continue;
        }
        ++proxies;
        char cls[48] = "(none)";
        int32_t off = 0;
        for (uint32_t q = proxy - 4u; q + 0x1000u > proxy && q > lo; q -= 4u)
            if (at_rtti(rd32(q), cls, sizeof cls)) {
                off = (int32_t)(proxy - q);
                break;
            }
        unsigned k = 0;
        while (k < nkinds && (kinds[k].off != off || strcmp(kinds[k].cls, cls)))
            ++k;
        if (k == nkinds && nkinds < 256u) {
            memcpy(kinds[k].cls, cls, sizeof cls);
            kinds[k].off = off;
            kinds[k].n = 0u;
            ++nkinds;
        }
        if (k < nkinds)
            ++kinds[k].n;
    }
    fprintf(stderr, "[g3] arena census: text %08x (count word %04x): %u bCStrings, %u held by proxies, %u by other words\n",
            text, rd16(text - 4u), n, proxies, others);
    for (unsigned shown = 0; shown < 24u; ++shown) {
        unsigned best = nkinds;
        for (unsigned k = 0; k < nkinds; ++k)
            if (kinds[k].n && (best == nkinds || kinds[k].n > kinds[best].n))
                best = k;
        if (best == nkinds)
            break;
        fprintf(stderr, "[g3] arena census:   %u proxies in %s at +0x%x\n", kinds[best].n, kinds[best].cls,
                (unsigned)kinds[best].off);
        kinds[best].n = 0u;
    }
    free(str);
}

/* G3_ARENA_TRACE_SAMPLE=<area>: a host thread from the process start reads, every half millisecond, the class name
 * of that nav area (gCNavigationAdmin at Game 10d1604c, its map at +0x54) and prints with the time whenever the list,
 * the text or the text's first word changes, and the count word at text - 4 when it moved by 256 or more, with the
 * highest and lowest value seen between two lines. Only reads; a racy read is a diagnostic's risk. */
#define AT_NAV_ADMIN 0x10d1604cu

static double at_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* C sees g_mem const (kit/runtime/x86.h) and may load it once, so the loop runs only after it is set, in its own
 * function. */
__attribute__((noinline)) static void at_sample_loop(uint32_t pick, double t0) {
    uint32_t areas0 = 1u, text0 = 1u, word0 = 1u, cw0 = 0u, hi = 0u, lo = 0xffffu;
    for (;;) {
        usleep(500);
        const uint32_t admin = rd32(AT_NAV_ADMIN), map = admin ? admin + 0x54u : 0u;
        const uint32_t areas = map ? rd32(map + 0x14u) : 0u, count = map ? rd32(map + 0x18u) : 0u;
        const uint32_t name = areas && count > pick ? rd32(areas + pick * 0x30u + 0x20u) : 0u;
        const uint32_t text = name ? rd32(name) : 0u, word = text ? rd32(text) : 0u, cw = text ? rd16(text - 4u) : 0u;
        if (cw > hi)
            hi = cw;
        if (cw < lo)
            lo = cw;
        const int moved = (cw > cw0 ? cw - cw0 : cw0 - cw) >= 256u;
        if (areas != areas0 || text != text0 || word != word0 || moved) {
            fprintf(stderr, "[g3] arena sample %.1f ms: list %08x (%u), text %08x first word %08x, count %u (%u-%u since)\n",
                    at_ms() - t0, areas, count, text, word, cw, lo, hi);
            areas0 = areas, text0 = text, word0 = word, cw0 = cw, hi = 0u, lo = 0xffffu;
        }
    }
}

static void *at_sampler(void *arg) {
    const double t0 = at_ms();
    while (!*(uint8_t *const volatile *)&g_mem)
        usleep(1000);
    at_sample_loop((uint32_t)(uintptr_t)arg, t0);
    return 0;
}

__attribute__((constructor)) static void at_sampler_start(void) {
    const char *v = getenv("G3_ARENA_TRACE_SAMPLE");
    if (!v || !*v)
        return;
    pthread_t t;
    if (pthread_create(&t, 0, at_sampler, (void *)(uintptr_t)strtoul(v, 0, 10)) == 0)
        pthread_detach(t);
}

uint32_t native_site_10a3efc3(X86 *c) {
    if (at_on < 0) {
        const char *v = getenv("G3_ARENA_TRACE");
        at_on = v && *v == '1';
    }
    /* G3_ARENA_TRACE_EVERY=<s>: report again at most every <s> seconds instead of once (across a save's load). */
    static int done;
    static time_t last;
    if (!at_on)
        return 0;
    if (done) {
        const char *every = getenv("G3_ARENA_TRACE_EVERY");
        if (!every || time(0) - last < atoi(every))
            return 0;
    }
    const char *who = getenv("G3_ARENA_TRACE_NPC");
    if (!who || !*who)
        who = "Fadi";
    const uint32_t map = c->r[R_ECX], nav = rd32(c->r[R_EBP] + 8u);
    const uint32_t self = nav ? rd32(nav + 0xcu) : 0u;
    char ns[96];
    at_name(self, ns, sizeof ns);
    if (strcmp(who, "*") && strcmp(ns, who))
        return 0;
    done = 1;
    last = time(0);
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    const uint32_t eip = c->eip;
    const uint32_t dest = at_thiscall(c, AT_GET_SELF, nav + 0x90u - 0x20u, 0, 0);
    char nd[96];
    at_name(dest, nd, sizeof nd);
    float p[3];
    for (int i = 0; i < 3; ++i)
        p[i] = dest ? rdf32(dest + 0x70u + 4u * (uint32_t)i) : rdf32(self + 0x70u + 4u * (uint32_t)i);
    const char *at = getenv("G3_ARENA_TRACE_AT");
    if (at && sscanf(at, "%f,%f,%f", &p[0], &p[1], &p[2]) == 3)
        snprintf(nd, sizeof nd, "G3_ARENA_TRACE_AT");
    const uint32_t areas = rd32(map + 0x14u), count = rd32(map + 0x18u), get_set = rd32(AT_GET_SET_IMPORT);
    fprintf(stderr, "[g3] arena zones: %s at (%.0f,%.0f,%.0f), destination %s at (%.0f,%.0f,%.0f), %u nav areas\n", ns,
            rdf32(self + 0x70u), rdf32(self + 0x74u), rdf32(self + 0x78u), nd, p[0], p[1], p[2], count);
    /* G3_ARENA_TRACE_AREAS=<i>,<j>,...: those entries raw before GetPropertySet touches them (the proxy at +0: its
     * vtable, +4 the entity handle, a pointer to the entity pointer, +8 the bCPropertyID, +0x1c the cached set, +0x20
     * the set's class name; +0x24 0 for a zone), and after the call the set and its entity. Then, for the whole list,
     * how many zones and paths give no set, with the first 24 of them raw. */
    const char *pick = getenv("G3_ARENA_TRACE_AREAS");
    for (const char *s = pick; s && *s;) {
        char *end;
        const unsigned long i = strtoul(s, &end, 10);
        if (end == s)
            break;
        s = *end == ',' ? end + 1 : end;
        if (i >= count)
            continue;
        const uint32_t area = areas + (uint32_t)i * 0x30u, handle = rd32(area + 4u), name = rd32(area + 0x20u);
        fprintf(stderr, "[g3] arena area #%lu raw", i);
        for (uint32_t k = 0; k < 0x30u; k += 4u)
            fprintf(stderr, " %08x", rd32(area + k));
        char cls[64];
        const uint32_t text = name ? rd32(name) : 0u;
        size_t n = 0;
        for (; text && n + 1 < sizeof cls && g_mem[text + n]; ++n)
            cls[n] = (char)g_mem[text + n];
        cls[n] = 0;
        fprintf(stderr, "; string %08x -> %08x:", name, text);
        for (uint32_t k = 0; text && k < 0x20u; k += 4u)
            fprintf(stderr, " %08x", rd32(text - 0x10u + k));
        const uint32_t set = at_thiscall(c, get_set, area, 0, 0), ent = set ? rd32(set + 0xcu) : 0u;
        char na[96];
        at_name(ent, na, sizeof na);
        fprintf(stderr, "; handle -> %08x, class %s; set %08x, entity %s at (%.0f,%.0f,%.0f); after, handle -> %08x\n",
                handle ? rd32(handle) : 0u, cls, set, ent ? na : "none", ent ? rdf32(ent + 0x70u) : 0.0f,
                ent ? rdf32(ent + 0x74u) : 0.0f, ent ? rdf32(ent + 0x78u) : 0.0f,
                rd32(area + 4u) ? rd32(rd32(area + 4u)) : 0u);
    }
    unsigned lost[2] = {0u, 0u}, shown = 0u;
    /* Each (zone or path, class name) pair of the list with its count, in order of first appearance. */
    struct {
        int zone;
        char cls[24];
        unsigned n;
    } kinds[16];
    unsigned nkinds = 0u;
    for (uint32_t i = 0; pick && areas && i < count && i < 200000u; ++i) {
        const uint32_t area = areas + i * 0x30u, name = rd32(area + 0x20u), text = name ? rd32(name) : 0u;
        char cls[24];
        size_t n = 0;
        for (; text && n + 1 < sizeof cls && g_mem[text + n]; ++n)
            cls[n] = (char)g_mem[text + n];
        cls[n] = 0;
        if (!name)
            snprintf(cls, sizeof cls, "(no string)");
        else if (!text)
            snprintf(cls, sizeof cls, "(no text)");
        const int zone = rd8(area + 0x24u) == 0u;
        unsigned k = 0;
        while (k < nkinds && (kinds[k].zone != zone || strcmp(kinds[k].cls, cls)))
            ++k;
        if (k == nkinds && nkinds < 16u) {
            kinds[k].zone = zone;
            memcpy(kinds[k].cls, cls, sizeof cls);
            kinds[k].n = 0u;
            ++nkinds;
        }
        if (k < nkinds)
            ++kinds[k].n;
    }
    for (unsigned k = 0; k < nkinds; ++k)
        fprintf(stderr, "[g3] arena areas: %u %s with class \"%s\"\n", kinds[k].n, kinds[k].zone ? "zones" : "paths",
                kinds[k].cls);
    for (uint32_t i = 0; pick && areas && i < count && i < 200000u; ++i) {
        const uint32_t area = areas + i * 0x30u;
        if (at_thiscall(c, get_set, area, 0, 0))
            continue;
        ++lost[rd8(area + 0x24u) != 0u];
        if (shown++ < 6u) {
            fprintf(stderr, "[g3] arena area #%u gives no set:", i);
            for (uint32_t k = 0; k < 0x30u; k += 4u)
                fprintf(stderr, " %08x", rd32(area + k));
            fprintf(stderr, "\n");
        }
    }
    if (pick)
        fprintf(stderr, "[g3] arena areas without a set: %u zones, %u paths of %u\n", lost[0], lost[1], count);
    if (pick && getenv("G3_ARENA_TRACE_CENSUS")) {
        const unsigned long first = strtoul(pick, 0, 10);
        const uint32_t name = first < count ? rd32(areas + (uint32_t)first * 0x30u + 0x20u) : 0u;
        at_census(name ? rd32(name) : 0u);
    }
    for (uint32_t i = 0; areas && i < count && i < 200000u; ++i) {
        const uint32_t area = areas + i * 0x30u;
        const uint32_t set = at_thiscall(c, get_set, area, 0, 0);
        const uint32_t ent = set ? rd32(set + 0xcu) : 0u;
        if (!ent)
            continue;
        const float dx = rdf32(ent + 0x70u) - p[0], dz = rdf32(ent + 0x78u) - p[2];
        const int zone = rd8(area + 0x24u) == 0u;
        static float wide = -1.0f;
        if (wide < 0.0f) {
            const char *v = getenv("G3_ARENA_TRACE_WIDE");
            wide = v ? (float)atof(v) : 0.0f;
        }
        const float reach = zone ? (wide > 0.0f ? wide : rdf32(set + 0x20u) + 500.0f) : (wide > 0.0f ? 0.0f : 4000.0f);
        if (!(dx * dx + dz * dz <= reach * reach))
            continue;
        char na[96];
        at_name(ent, na, sizeof na);
        fprintf(stderr, "[g3] arena zones:   #%u %s %s at (%.0f,%.0f,%.0f), radius %.0f, distance %.0f\n", i,
                zone ? "zone" : "path", na, rdf32(ent + 0x70u), rdf32(ent + 0x74u), rdf32(ent + 0x78u),
                zone ? rdf32(set + 0x20u) : 0.0f, sqrtf(dx * dx + dz * dz));
    }
    /* GetZone(bCVector const &, bCPropertyID &, bool &, bool, bool, float) on the map, as FindWay's first call
     * (10a3efc0: false, true and the float at 10c1805c), for the point and for the NPC's own position. The vector,
     * the ID and the bool live 0x200 bytes below ESP, which is moved down for the calls. */
    for (int which = 0; which < 2; ++which) {
        const uint32_t base = c->r[R_ESP] - 0x200u;
        for (uint32_t i = 0; i < 0x100u; i += 4u)
            wr32(base + i, 0u);
        for (int i = 0; i < 3; ++i)
            wr32(base + 4u * (uint32_t)i, which ? rd32(self + 0x70u + 4u * (uint32_t)i) : 0u);
        float q[3];
        const char *at2 = getenv("G3_ARENA_TRACE_AT2");
        if (which && at2 && sscanf(at2, "%f,%f,%f", &q[0], &q[1], &q[2]) == 3)
            for (int i = 0; i < 3; ++i)
                memcpy(g_mem + base + 4u * (uint32_t)i, &q[i], 4);
        if (!which)
            for (int i = 0; i < 3; ++i)
                memcpy(g_mem + base + 4u * (uint32_t)i, &p[i], 4);
        const uint32_t esp = c->r[R_ESP];
        c->r[R_ESP] = base - 0x10u;
        const uint32_t args[6] = {base, base + 0x20u, base + 0x80u, 0u, 1u, rd32(AT_FLOAT_10C1805C)};
        const uint32_t r = at_thiscall(c, AT_GET_ZONE, map, args, 6) & 0xffu;
        c->r[R_ESP] = esp;
        fprintf(stderr, "[g3] arena zones: GetZone(%s (%.0f,%.0f,%.0f)) = %u, flag %u, id", which ? ns : nd,
                rdf32(base), rdf32(base + 4u), rdf32(base + 8u), r, rd8(base + 0x80u));
        for (uint32_t i = 0; i < 0x18u; i += 4u)
            fprintf(stderr, " %08x", rd32(base + 0x20u + i));
        fprintf(stderr, "\n");
    }
    memcpy(c->r, saved, sizeof saved);
    c->eip = eip;
    return 0;
}
