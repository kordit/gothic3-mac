/* native/findway.c - gCNavigationMap::FindWay (Game.dll 10a41090) as one native function, group "findway" (D-058).
 *
 * The search under every NPC's way (gCNavigationMap::FindWay(gCNavigation_PS *) 10a3efc0 and 10a422a0) is a
 * best-first search whose open list holds whole ways, not nodes: a node expanded once comes back on every other way
 * that reaches it, and only ways still in the open list are compared by their last node. When the destination cannot
 * be reached the search walks the graph's cycles until its own breaker, every 1000 steps a bCTimer::GetTimeStamp,
 * gives up after 3000 ms ("gCNavigationMap::FindWay - DEADLOCK PREVENTION (3 sec.)"): one frame of 3.0 s. The player
 * met it twice on 31.10 (game.log: before a dialog began and after one ended), the bench at the arena orc too.
 *
 * bEResult FindWay(bCVector const &from, bCVector const &to, gSNavArea const &a, gSNavArea const &b,
 * bTObjArray<gSWayAreaElement> &way, bool &deadlock, bool doors), thiscall, RET 0x1c.
 *
 * This first asks whether the destination can be reached at all, on the same graph and by the same rules, without
 * costs and with a visited set: the area with fewer links is the start (as the original swaps them), its links are
 * the first nodes, a node's neighbours come from this+0x2c, and a node is a goal, passes or is dropped as the
 * original decides it (the property set across the link: the destination's set is a goal; one whose +0xb8 is 0, or
 * with `doors` whose +0x2c is 0, is a closed door; the first links are also goals when the destination area lists
 * them). The rules that depend on the order of the search (a dead end left out after the first step, a way that may
 * not pass one zone twice) are left out, so this finds at least every node the original can reach. When the
 * destination is reachable the original runs and answers as before, bit for bit. When it is not, the answer is the
 * one the breaker gives: the way cleared (FUN_10927720, as the original's first step), deadlock true, 0; the caller
 * (10a40428) then sets the NPC's navigation state 0xf and its +0x255 byte as after 3 s of search.
 *
 * The original also runs for everything its first steps settle without a search (a proxy without a set, both areas
 * in one set, an area not in the map) and for a map this cannot walk (an index out of range).
 *
 * G3_FINDWAY_CHECK=1: the original runs after every check, and each call is logged with both answers; a destination
 * this calls unreachable that the original reaches is reported as a mismatch. Without it, each short answer is
 * logged (the first 20, then every 100th). */
#include "chunk.h"
#include "g3_opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void fn_10a41090(X86 *c);
/* kit/runtime (C++): guest.h guest_call. */
uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");

#define FW_GET_SET_IMPORT 0x10b5c4fcu /* Game's import of eCPropertySetProxy::GetPropertySet (thiscall, no arguments) */
#define FW_AREA_INDEX 0x10a439a0u     /* gCNavigationMap::GetNavAreaListIndex(gSNavArea const &), thiscall, RET 4 */
#define FW_CLEAR_WAY 0x10927720u      /* the way array's clear (stdcall, the array), the original's first call */
#define FW_MAX_LINKS (1u << 20)       /* an index beyond this: not a map this walks */

typedef struct {
    X86 *c;
    uint32_t get_set, map, links, adj, dest_set, doors;
    uint32_t n;            /* links known so far (the arrays' size) */
    uint8_t *seen;         /* per link: 1 queued */
    uint32_t *set_c, *set_30; /* per link: the property set of its +0xc and +0x30 proxy, +1 (0: not asked yet) */
    uint32_t *queue;
    uint32_t qn, qcap;
    int bad;
} Fw;

static uint32_t fw_thiscall(X86 *c, uint32_t fn, uint32_t self, const uint32_t *args, int n) {
    c->r[R_ECX] = self;
    return guest_call(c, fn, args, n);
}

static int fw_grow(Fw *w, uint32_t link) {
    if (link < w->n)
        return 1;
    if (link >= FW_MAX_LINKS) {
        w->bad = 1;
        return 0;
    }
    uint32_t n = w->n ? w->n : 256u;
    while (n <= link)
        n *= 2u;
    uint8_t *seen = realloc(w->seen, n);
    uint32_t *sc = realloc(w->set_c, n * 4u), *s30 = realloc(w->set_30, n * 4u);
    if (seen)
        w->seen = seen;
    if (sc)
        w->set_c = sc;
    if (s30)
        w->set_30 = s30;
    if (!seen || !sc || !s30) {
        w->bad = 1;
        return 0;
    }
    memset(w->seen + w->n, 0, n - w->n);
    memset(w->set_c + w->n, 0, (n - w->n) * 4u);
    memset(w->set_30 + w->n, 0, (n - w->n) * 4u);
    w->n = n;
    return 1;
}

/* The property set of link `link`'s proxy at +0xc or +0x30, asked once per link and side. */
static uint32_t fw_set(Fw *w, uint32_t link, int far) {
    uint32_t *cache = far ? w->set_30 : w->set_c;
    if (!cache[link])
        cache[link] = fw_thiscall(w->c, w->get_set, w->links + link * 0x54u + (far ? 0x30u : 0xcu), 0, 0) + 1u;
    return cache[link] - 1u;
}

/* A set the way may not pass: neither the destination's, nor open (+0xb8), nor with `doors` passable (+0x2c). */
static int fw_closed(Fw *w, uint32_t set) {
    return set != 0u && !(rd8(set + 0xb8u) != 0u && (!w->doors || rd8(set + 0x2cu) != 0u));
}

static int fw_push(Fw *w, uint32_t link) {
    if (w->seen[link])
        return 1;
    w->seen[link] = 1;
    if (w->qn == w->qcap) {
        uint32_t cap = w->qcap ? w->qcap * 2u : 256u;
        uint32_t *q = realloc(w->queue, cap * 4u);
        if (!q) {
            w->bad = 1;
            return 0;
        }
        w->queue = q;
        w->qcap = cap;
    }
    w->queue[w->qn++] = link;
    return 1;
}

/* 1: the destination can be reached, 0: it cannot, -1: not settled here (the original decides). */
static int fw_reachable(Fw *w, uint32_t esp, uint32_t *searched) {
    X86 *c = w->c;
    const uint32_t pa = rd32(esp + 12u), pb = rd32(esp + 16u);
    const uint32_t set_a = fw_thiscall(c, w->get_set, pa, 0, 0), set_b = fw_thiscall(c, w->get_set, pb, 0, 0);
    if (!set_a || !set_b || set_a == set_b)
        return -1;
    uint32_t arg = pa;
    const int32_t ia = (int32_t)fw_thiscall(c, FW_AREA_INDEX, w->map, &arg, 1);
    arg = pb;
    const int32_t ib = (int32_t)fw_thiscall(c, FW_AREA_INDEX, w->map, &arg, 1);
    if (ia == -1 || ib == -1)
        return -1;
    const uint32_t areas = rd32(w->map + 0x14u);
    const uint32_t na = rd16(areas + (uint32_t)ia * 0x30u + 0x2au), nb = rd16(areas + (uint32_t)ib * 0x30u + 0x2au);
    /* The original starts from the area with fewer links (10a41169). */
    const int a_first = na <= nb;
    const uint32_t start_area = areas + (uint32_t)(a_first ? ia : ib) * 0x30u;
    const uint32_t dest_area = areas + (uint32_t)(a_first ? ib : ia) * 0x30u;
    const uint32_t start_param = a_first ? pa : pb;
    w->dest_set = a_first ? set_b : set_a;
    const int start_is_zone = rd8(start_param + 0x24u) == 0u;
    const uint32_t start_n = rd16(start_area + 0x2au), start_links = rd32(start_area + 0x2cu);
    const uint32_t dest_n = rd16(dest_area + 0x2au), dest_links = rd32(dest_area + 0x2cu);
    for (uint32_t i = 0; i < start_n; ++i) {
        const uint32_t link = rd32(start_links + i * 4u);
        if (!fw_grow(w, link))
            return -1;
        /* From a zone the far side of the link counts (+0x30, with its doors), from a path the near one (+0xc). */
        const uint32_t set = fw_set(w, link, start_is_zone);
        if (set == w->dest_set)
            return 1;
        if (start_is_zone && fw_closed(w, set))
            continue;
        for (uint32_t j = 0; j < dest_n; ++j)
            if (rd32(dest_links + j * 4u) == link)
                return 1;
        if (!fw_push(w, link))
            return -1;
    }
    for (uint32_t q = 0; q < w->qn; ++q) {
        const uint32_t from = w->queue[q];
        const uint32_t count = rd16(w->adj + from * 8u + 2u), list = rd32(w->adj + from * 8u + 4u);
        for (uint32_t j = 0; j < count; ++j) {
            const uint32_t e = list + j * 0xcu, to = rd32(e + 8u);
            if (to == from || !fw_grow(w, to))
                continue;
            if (w->bad)
                return -1;
            if (w->seen[to])
                continue;
            /* The neighbour's flag picks the side: 0 the far one with its doors, else the near one. */
            const int far = rd8(e + 4u) == 0u;
            const uint32_t set = fw_set(w, to, far);
            if (set == w->dest_set)
                return 1;
            if (far && fw_closed(w, set))
                continue;
            if (!fw_push(w, to))
                return -1;
        }
    }
    *searched = w->qn;
    return w->bad ? -1 : 0;
}

static double fw_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

void g3n_10a41090(X86 *c) {
    if (!G3OPT(FINDWAY)) {
        fn_10a41090(c);
        return;
    }
    static int check = -1;
    if (check < 0) {
        const char *v = getenv("G3_FINDWAY_CHECK");
        check = v && *v == '1';
    }
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    const uint32_t esp = c->r[R_ESP];
    Fw w = {0};
    w.c = c;
    w.get_set = rd32(FW_GET_SET_IMPORT);
    w.map = c->r[R_ECX];
    w.links = rd32(w.map + 0x20u);
    w.adj = rd32(w.map + 0x2cu);
    w.doors = rd8(esp + 28u) != 0u;
    uint32_t searched = 0;
    const double t0 = fw_ms();
    const int reach = fw_reachable(&w, esp, &searched);
    const double t1 = fw_ms();
    free(w.seen);
    free(w.set_c);
    free(w.set_30);
    free(w.queue);
    memcpy(c->r, saved, sizeof saved);
    static unsigned shorts, mismatches;
    if (check) {
        fn_10a41090(c);
        const double t2 = fw_ms();
        const uint32_t answer = c->r[R_EAX] & 0xffu, flag = rd8(rd32(esp + 24u));
        const int bad = reach == 0 && answer == 1u;
        if (bad)
            ++mismatches;
        fprintf(stderr, "[g3] findway check: %s (%u links, %.2f ms); the original %u, deadlock %u, %.1f ms%s\n",
                reach > 0 ? "reachable" : reach == 0 ? "unreachable" : "not settled", searched, t1 - t0, answer, flag,
                t2 - t1, bad ? "  MISMATCH" : "");
        return;
    }
    if (reach != 0) {
        fn_10a41090(c);
        return;
    }
    /* The breaker's answer (10a41d9e): the way cleared at the start, deadlock true, 0. */
    const uint32_t way = rd32(esp + 20u), deadlock = rd32(esp + 24u);
    wr8(deadlock, 0u);
    guest_call(c, FW_CLEAR_WAY, &way, 1);
    memcpy(c->r, saved, sizeof saved);
    wr8(deadlock, 1u);
    if (shorts < 20u || shorts % 100u == 0u)
        fprintf(stderr, "[g3] findway: no way (%u links searched, %.2f ms), the breaker's answer at once (#%u)\n",
                searched, t1 - t0, shorts + 1u);
    ++shorts;
    c->r[R_EAX] = 0u;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4u + 0x1cu;
    recomp_return(c);
}
