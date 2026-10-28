/* native/occlusion.c - stage 4: occlusion results from the previous frame instead of a same-frame GPU wait.
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h). A native version
 * runs under G3OPT(OCCL) and ends as the translated function does (see its RET in
 * build/recomp/gen/aux-engine/). Design and numbers: docs/perf/occlusion-design.md.
 *
 * eCFrustumDatabase::ExtractFromFrustumOC (103fe590) is coherent hierarchical culling: hierarchy
 * nodes that were not visible last frame get a hardware occlusion query, the query goes into a
 * local queue of 0x18-byte entries {query, result, node, cell, cull status, distance}, and once
 * the traversal heap runs dry the queue head is resolved with ResolveOCQuery(query, wait=true)
 * (call at 103feb5b), which spins on GetData(FLUSH) until the GPU has run the half-drawn frame.
 *
 * Here that one call site answers from the result the same node got from a query of an earlier
 * frame, and the query just issued is kept in a pending list until a later frame reads it without
 * waiting. Nodes without a fresh result answer "visible". An earlier "visible" is always used (a
 * node that went behind an occluder is drawn a frame too long), an earlier "hidden" only while the
 * database's frustum (filter sphere and planes) is byte for byte last frame's: with the camera
 * moving, a node coming out from behind an occluder vanished for a frame or two (the user saw
 * elements disappear), so such a query waits for its own count as the original does, unless the
 * group occlmove is on: then such a node answers "visible" and its query goes to the pending list
 * like any other. Drawing a node that is in fact behind an occluder leaves the picture as it was
 * (the depth test hides it), so this never makes anything vanish; it costs the node's draws
 * instead of a same-frame GPU round trip, which in a window with frames queued for the display
 * took about 28% of the main thread (work/sample-live-1.txt, 2026-10-24). The non-waiting poll of the same queue (call at 103fe9df) answers "not ready" without
 * asking the device, since a query issued in this pass cannot have run yet and asking it costs a
 * render-thread sync and a command-buffer commit in the kit.
 *
 * Both call sites are recognised by the guest return address on the stack; the queue entry is in
 * ESI there (103fe880 LEA ESI,[EAX+EDX*8]). Every other caller (ResolveDynamicQuery,
 * UpdateIllumination, the entity queries at 103fe7d9) goes to the original. */
#include "chunk.h"
#include "g3_opt.h"

#include <string.h>

void fn_103fe3c0(X86 *c); /* Engine eCFrustumDatabase::IsQueryResultAvailable */
void fn_103fe3f0(X86 *c); /* Engine eCFrustumDatabase::GetQueryResult */
void fn_103fe590(X86 *c); /* Engine eCFrustumDatabase::ExtractFromFrustumOC */
void fn_10404390(X86 *c); /* Engine eCOcclusionProcessor::ResolveOCQuery */

/* Continuations of the two ResolveOCQuery calls on the query queue inside ExtractFromFrustumOC. */
#define OCCL_RET_POLL 0x103fe9e4u /* 103fe9df CALL, wait=false */
#define OCCL_RET_WAIT 0x103feb60u /* 103feb5b CALL, wait=true */
/* The other callers, for G3OCCLVIS (native/g3_opt.h). */
#define OCCL_RET_ENTITY 0x103fe7deu  /* 103fe7d9, entity queries in ExtractFromFrustumOC */
#define OCCL_RET_DYNAMIC 0x1040356cu /* 10403567, eCFrustumElementItem::ResolveDynamicQuery */
#define OCCL_RET_ILLUM 0x1034e4c4u   /* 1034e4bf, eCIlluminated_PS::UpdateIllumination */

#define OCCL_NOT_READY 0xffffffffu
#define OCCL_VISIBLE 0x7fffffffu /* above any threshold at eCFrustumDatabase+0x1b4, and not -1 */

/* A result is used while its query is at most this many frames old (frames of the database,
 * eCFrustumDatabase+4, which ExtractFromFrustum counts up before each extraction). */
#define OCCL_MAX_AGE 2u
/* A pending query not answered after this many frames is forgotten without asking the device
 * (its handle stays allocated in the device: a leak of one query, never a double owner). */
#define OCCL_DROP_AGE 16u

#define OCCL_HIST_BITS 15u
#define OCCL_HIST_SLOTS (1u << OCCL_HIST_BITS)
#define OCCL_PROBE 16u
#define OCCL_PENDING_MAX 8192u

typedef struct {
    uint32_t db, node; /* key; node 0 = empty */
    uint32_t frame;    /* database frame the query was issued in */
    uint32_t count;    /* samples that passed */
    uint32_t tick;     /* extraction counter at insertion, for eviction */
} OcclHist;

typedef struct {
    uint32_t proc;  /* eCOcclusionProcessor (ECX of the call) */
    uint32_t query; /* the handle, owned here until read */
    uint32_t db, node, frame;
} OcclPending;

typedef struct {
    uint64_t answered_hist;    /* wait-site calls answered from an earlier frame */
    uint64_t answered_visible; /* wait-site calls with no fresh result: "visible" */
    uint64_t waited;           /* wait-site calls hidden before with the frustum moved: the original */
    uint64_t moved_visible;    /* the same with occlmove on: "visible", no wait */
    uint64_t polls_skipped;    /* poll-site calls answered "not ready" without the device */
    uint64_t sweeps;           /* pending sweeps */
    uint64_t read;             /* pending queries read and handed back to the pool */
    uint64_t not_ready;        /* pending reads that were not ready yet */
    uint64_t dropped;          /* pending queries forgotten (too old or list full) */
} OcclStats;

static OcclHist occl_hist[OCCL_HIST_SLOTS];
static OcclPending occl_pending[OCCL_PENDING_MAX];
static uint32_t occl_pending_n;
static uint32_t occl_tick;
static OcclStats occl_stats;

/* The extraction running now (ExtractFromFrustumOC does not nest, but keep it exact if it did). */
static struct {
    uint32_t db, frame;
    int still; /* the database's frustum is the one of its previous extraction */
} occl_cur;

/* The frustum each database extracted with last: eCFrustumFilter at db+0x40, its sphere at
 * filter+0x38 and six planes at filter+0x60..0xbf (docs/perf/scene-extract.md). */
#define OCCL_FRUSTUM_SPHERE 0x78u
#define OCCL_FRUSTUM_PLANES 0xa0u
#define OCCL_FRUSTUM_BYTES (16u + 96u)
#define OCCL_FRUSTUM_DBS 16u
static struct {
    uint32_t db;
    uint8_t bytes[OCCL_FRUSTUM_BYTES];
} occl_frustum[OCCL_FRUSTUM_DBS];

/* Whether db's frustum is the one it had at its previous extraction; records the current one. */
static int occl_frustum_still(uint32_t db) {
    uint8_t now[OCCL_FRUSTUM_BYTES];
    memcpy(now, g_mem + db + OCCL_FRUSTUM_SPHERE, 16u);
    memcpy(now + 16u, g_mem + db + OCCL_FRUSTUM_PLANES, 96u);
    unsigned slot = db % OCCL_FRUSTUM_DBS;
    for (unsigned i = 0; i < OCCL_FRUSTUM_DBS; ++i)
        if (occl_frustum[i].db == db) {
            slot = i;
            break;
        } else if (!occl_frustum[i].db) {
            slot = i;
            break;
        }
    const int still = occl_frustum[slot].db == db && memcmp(occl_frustum[slot].bytes, now, sizeof now) == 0;
    occl_frustum[slot].db = db;
    memcpy(occl_frustum[slot].bytes, now, sizeof now);
    return still;
}

static uint32_t occl_slot(uint32_t db, uint32_t node) {
    uint32_t k = node ^ (db * 0x9e3779b1u);
    k ^= k >> 16;
    return (k * 0x85ebca6bu) >> (32u - OCCL_HIST_BITS);
}

static const OcclHist *occl_hist_find(uint32_t db, uint32_t node) {
    uint32_t s = occl_slot(db, node);
    for (uint32_t i = 0; i < OCCL_PROBE; ++i) {
        const OcclHist *h = &occl_hist[(s + i) & (OCCL_HIST_SLOTS - 1u)];
        if (h->node == node && h->db == db)
            return h;
        if (!h->node)
            return 0;
    }
    return 0;
}

/* Keeps the newest result per node; a full probe window gives up its oldest entry. */
static void occl_hist_put(uint32_t db, uint32_t node, uint32_t frame, uint32_t count) {
    uint32_t s = occl_slot(db, node);
    OcclHist *victim = 0;
    for (uint32_t i = 0; i < OCCL_PROBE; ++i) {
        OcclHist *h = &occl_hist[(s + i) & (OCCL_HIST_SLOTS - 1u)];
        if (h->node == node && h->db == db) {
            if ((int32_t)(frame - h->frame) >= 0) {
                h->frame = frame;
                h->count = count;
                h->tick = occl_tick;
            }
            return;
        }
        if (!h->node) {
            victim = h;
            break;
        }
        if (!victim || (int32_t)(h->tick - victim->tick) < 0)
            victim = h;
    }
    /* An evicted entry may sit before a key further along its chain; lookups stop at an empty
     * slot only, and eviction never empties one, so the chain stays intact. */
    victim->db = db;
    victim->node = node;
    victim->frame = frame;
    victim->count = count;
    victim->tick = occl_tick;
}

static void occl_pending_add(uint32_t proc, uint32_t query, uint32_t db, uint32_t node, uint32_t frame) {
    if (occl_pending_n == OCCL_PENDING_MAX) {
        memmove(occl_pending, occl_pending + 1, (OCCL_PENDING_MAX - 1u) * sizeof occl_pending[0]);
        --occl_pending_n;
        ++occl_stats.dropped;
    }
    occl_pending[occl_pending_n++] = (OcclPending){proc, query, db, node, frame};
}

/* ResolveOCQuery(query, wait) on the translated original, called from here: the guest stack gets
 * the two arguments and a return address that is a real call continuation of the same function's
 * caller (so recomp_return treats it as one), and everything but EAX comes back as it was. On
 * success the original reads the count and pushes the handle onto the processor's free list. */
static uint32_t occl_resolve(X86 *c, uint32_t proc, uint32_t query, uint32_t wait) {
    const uint32_t esp = c->r[4], eip = c->eip, ecx = c->r[1], edx = c->r[2], eax = c->r[0];
    c->r[4] -= 4u;
    wr32(c->r[4], wait);
    c->r[4] -= 4u;
    wr32(c->r[4], query);
    c->r[4] -= 4u;
    wr32(c->r[4], OCCL_RET_POLL);
    c->r[1] = proc;
    fn_10404390(c);
    const uint32_t r = c->r[0];
    c->r[4] = esp;
    c->eip = eip;
    c->r[0] = eax;
    c->r[1] = ecx;
    c->r[2] = edx;
    return r;
}

/* Reads, without waiting, every query this database issued in an earlier frame. A query issued in
 * this frame is left alone: its draws are still in the open command buffer. */
static void occl_sweep(X86 *c, uint32_t db, uint32_t frame) {
    uint32_t keep = 0;
    ++occl_stats.sweeps;
    for (uint32_t i = 0; i < occl_pending_n; ++i) {
        OcclPending p = occl_pending[i];
        const uint32_t age = frame - p.frame;
        if (p.db == db && age != 0u && age < 0x80000000u) {
            if (age > OCCL_DROP_AGE) {
                ++occl_stats.dropped;
                continue;
            }
            const uint32_t r = occl_resolve(c, p.proc, p.query, 0u);
            if (r != OCCL_NOT_READY) {
                ++occl_stats.read;
                occl_hist_put(p.db, p.node, p.frame, r);
                continue;
            }
            ++occl_stats.not_ready;
        }
        occl_pending[keep++] = p;
    }
    occl_pending_n = keep;
}

/* The answer for a node at the wait site: its newest result if fresh enough and either visible
 * (above the database's threshold at +0x1b4, the original's test) or from a still frustum, else
 * "visible" when there is none, else "visible" under occlmove, else OCCL_NOT_READY: ask the
 * device and wait, as the original. */
static uint32_t occl_answer(uint32_t db, uint32_t node, uint32_t frame) {
    const OcclHist *h = occl_hist_find(db, node);
    if (h && frame - h->frame <= OCCL_MAX_AGE) {
        if (rd32(db + 0x1b4u) < h->count || occl_cur.still) {
            ++occl_stats.answered_hist;
            return h->count;
        }
        if (G3OPT(OCCLMOVE)) {
            ++occl_stats.moved_visible;
            return OCCL_VISIBLE;
        }
        ++occl_stats.waited;
        return OCCL_NOT_READY;
    }
    ++occl_stats.answered_visible;
    return OCCL_VISIBLE;
}

/* IsQueryResultAvailable and GetQueryResult have no direct callers in Engine.dll (no CALL to either
 * in the listing); they stay the originals. */
void g3n_103fe3c0(X86 *c) {
    fn_103fe3c0(c);
}

void g3n_103fe3f0(X86 *c) {
    fn_103fe3f0(c);
}

/* ExtractFromFrustumOC itself stays the original. Before it runs, the queries held from earlier
 * frames of this database are read (without waiting), so their results are there for the wait site
 * and their handles are back in the pool before StartOCQuery looks for a free one. This runs with
 * the group off too, so switching it off mid-game returns what it holds. */
void g3n_103fe590(X86 *c) {
    const uint32_t db = c->r[1];
    const uint32_t saved_db = occl_cur.db, saved_frame = occl_cur.frame;
    const int saved_still = occl_cur.still;
    occl_cur.db = db;
    occl_cur.frame = db ? rd32(db + 4u) : 0u;
    occl_cur.still = db ? occl_frustum_still(db) : 0;
    ++occl_tick;
    if (occl_pending_n && db)
        occl_sweep(c, db, occl_cur.frame);
    fn_103fe590(c);
    occl_cur.db = saved_db;
    occl_cur.frame = saved_frame;
    occl_cur.still = saved_still;
}

/* ResolveOCQuery(this=ECX, void *query, bool wait), RET 8. */
void g3n_10404390(X86 *c) {
    const uint32_t esp = c->r[4];
    const uint32_t ret = rd32(esp), query = rd32(esp + 4u);
    if (g3_occl_vis) {
        const uint32_t site = ret == OCCL_RET_ENTITY ? 1u : ret == OCCL_RET_DYNAMIC ? 2u
                            : ret == OCCL_RET_ILLUM ? 4u : ret == OCCL_RET_POLL || ret == OCCL_RET_WAIT ? 8u : 0u;
        if (g3_occl_vis & site) { /* diagnostics: the original reads it, the caller sees "visible" */
            fn_10404390(c);
            if (c->r[0] != OCCL_NOT_READY)
                c->r[0] = OCCL_VISIBLE;
            return;
        }
    }
    if (!G3OPT(OCCL) || !occl_cur.db || !query || (ret != OCCL_RET_POLL && ret != OCCL_RET_WAIT)) {
        fn_10404390(c);
        return;
    }
    uint32_t value;
    if (ret == OCCL_RET_POLL) {
        ++occl_stats.polls_skipped;
        value = OCCL_NOT_READY;
    } else {
        const uint32_t proc = c->r[1];
        const uint32_t node = rd32(c->r[6] + 8u); /* ESI = queue entry, +8 the hierarchy node */
        value = occl_answer(occl_cur.db, node, occl_cur.frame);
        if (value == OCCL_NOT_READY) { /* hidden before, the camera moved: the count of this query */
            fn_10404390(c);
            return;
        }
        occl_pending_add(proc, query, occl_cur.db, node, occl_cur.frame);
    }
    /* As the original's exits: EAX the count, ECX/ESI/EDI as on entry (it pops them back). */
    c->r[0] = value;
    c->eip = ret;
    c->r[4] = esp + 12u;
    recomp_return(c);
}
