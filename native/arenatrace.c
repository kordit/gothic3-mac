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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

uint32_t native_site_10a3efc3(X86 *c) {
    if (at_on < 0) {
        const char *v = getenv("G3_ARENA_TRACE");
        at_on = v && *v == '1';
    }
    static int done;
    if (!at_on || done)
        return 0;
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
