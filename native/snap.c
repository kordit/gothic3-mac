/* native/snap.c - the render's split into gathering and execution, where its writes to the logic's state move
 * (D-054 points 2 and 3, docs/PLAN.md step 3, docs/perf/migawka.md). Group `snap` (G3OPT_SNAP), off by default.
 *
 * eCRenderSystemAdmin::Render 1040e430 refreshes the frustum data, runs the PVS prefetcher, gathers the render list
 * (ExtractFromFrustum 103fee70) and then executes it (eCFrustumDatabase::Execute 10400530). The boundary between
 * the two halves is the entry of Execute. What Execute writes into the logic's state (the trace: particles sorted in
 * place by DepthSortParticles 103afa70, the SpeedTree wind advanced in OnBeginRenderSolid 10468f60, ...) is to be
 * done at that boundary instead, in the same order, so that one thread gives the same pictures bit for bit (step 3),
 * and later the gathering can end the logic's iteration while the execution runs beside the next one (step 6).
 *
 * Each function here runs the translated original while the group is off; with it on, the moved work is added one
 * function at a time, each checked against the reference runs (tools/bench.sh BENCH_PIN/BENCH_HASH). */
#include "chunk.h"
#include "g3_opt.h"

#include <stdio.h>

uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");

void fn_1040e430(X86 *c); /* Engine eCRenderSystemAdmin::Render */
void fn_10400530(X86 *c); /* Engine eCFrustumDatabase::Execute */
void fn_103afa70(X86 *c); /* Engine eCParticle_PS::DepthSortParticles */
void fn_104035c0(X86 *c); /* Engine eCFrustumElementItem::Request */
void fn_10468f60(X86 *c); /* Engine eCSpeedTreeAdmin::OnBeginRenderSolid */


/* ---- particles: DepthSortParticles at the end of the gathering ----------------------------------------------
 *
 * UpdateVertices 103b3630 (from RenderVertices 103b4c80, from the virtual OnRenderAlpha 103aee30 of an alpha render
 * op) sorts an emitter's particle array in place, back to front from the camera, once a frame (its +0x348 against
 * the world's frame number), before it writes the vertices. The array is the logic's own (EmitParticles, Spawn,
 * TickParticles walk it in that order). Moved: right after the gathering (below), each emitter an op of the two
 * alpha lists will draw is sorted, by the same code with the same camera, and the sort in UpdateVertices finds it
 * done.
 *
 * An instanced emitter (+0x22d set) draws through its master: that one is sorted when it exists already (sn_master);
 * a master GetMasterInstance 103c0cb0 has yet to make (an entity cloned in the render, itself a write to the logic's
 * state, not moved yet) and an empty emitter (filled by OnRenderAlpha's virtual +0x124) are left to the drawing.
 * Measured (30.10.2026, chodzenie, pinned, 60 s): 14286 sorts moved, 2 left; frames as the reference's but for the
 * known single sky tile (tools/frames_diff.py). */
#define SN_WORLD 0x10862a88u /* the object whose +0x5e8 is the frame number (native/particles.c ENGINE_WORLD) */
#define SN_PARTICLE_VFT 0x106bdcbcu /* eCParticle_PS's vtable (RTTI: COL 107021b8, stage/Engine.dll) */
#define SN_DEPTHSORT 0x103afa70u

static uint32_t sn_sorted[4096], sn_nsorted; /* emitters sorted after this Execute's gathering */
uint64_t g3_snap_particles_moved, g3_snap_particles_left; /* sorts moved; sorts still made in the drawing */
static int sn_presorting;

static int sn_presorted(uint32_t ps) {
    for (uint32_t i = 0; i < sn_nsorted; ++i)
        if (sn_sorted[i] == ps)
            return 1;
    return 0;
}

/* The gathering is the loop at the head of Execute 10400530 (10400720..1040073d) that asks every element of the
 * database for its render ops: eCFrustumElementItem::Request 104035c0 (db, flags, db + 0x124), which calls each render
 * context's fill functions, +0x7c into the solid lists (+0x148, +0x160), +0x80 into the alpha lists (+0x13c, +0x16c),
 * +0x84 into +0x184, and draws nothing. Before it Execute only sets up the device (render target, viewport, clear,
 * fog, BeginScene); after it, it draws. With the group on, the loop is made here, before Execute, with the flags
 * Execute would pass; the particles drawn by the two alpha lists are sorted right after it; and Request returns at
 * once when Execute makes the loop itself. That is the boundary step 6 cuts at: everything up to here can end the
 * logic's iteration.
 *
 * Execute's flags (10400530.c:192-210): 2, or 6 with +0x193; |0x10 with +0x1bd; |0x20 with +0x1c6 while the device
 * draws to the application's own target (+0xb88 set and GetRenderTarget giving +0xb94); |1 with +0x1c4; |0x40 with
 * +0x1ae; |0x80 with +0x1cc. The device's target is what eCGfxMixerAdmin::GetRenderTarget 102eb350 asks the API
 * device (gfx admin +0x5b8, +0xc) for, which for eCDX9Device (vtable +0x21c = 10307b30) is its +0x24; under any other
 * device the database is left to Execute as it is (g3_snap_gather_left counts them). An op's +0 is its render context; a legacy one
 * (eCRenderContextLegacy::Render 1040d8b0) calls its object's (ctx +0x10) OnRenderAlpha, slot +0x100, when the op's +4
 * is 1. For an emitter that is OnRenderAlpha 103aee30 -> RenderVertices -> UpdateVertices(this or its master). */
#define SN_APP 0x10862a88u /* eCApplication::ms_pApplication */
#define SN_REQUEST 0x104035c0u
#define SN_GFX 0x10874bc0u     /* FUN_10277b80's cached eCGfxAdmin, valid once 10874bc4 bit 0 is set */
#define SN_GFX_SET 0x10874bc4u
#define SN_DX9_GET_RENDER_TARGET 0x10307b30u /* eCDX9Device::GetRenderTarget: [this+0x24], [this+0x28] */
#define SN_LEGACY_VFT 0x106bdb54u /* eCRenderContextLegacy's vtable (RTTI) */

uint64_t g3_snap_gathered, g3_snap_gather_left; /* Executes gathered here; left to Execute */
static uint32_t sn_gathered_db; /* the database whose Request loop is done, while its Execute runs */

/* An instance's master as eCParticleModule::GetMasterInstance 103c0cb0 finds it, without making one: the key is the
 * instance's entity's creator (the particle system's vtable +0x6c getter = [ps+0xc], then GetCreator 10488a80 =
 * [entity+0x1b4]), looked up by FUN_103c1ab0 in the module's map at +0x20 (buckets) / +0x24 (count): bucket
 * (key >> 4) % count, nodes {key, master, ..., next at +0x14}. 0 when there is none yet: the drawing makes it then. */
#define SN_PARTICLE_MODULE 0x10878c20u /* FUN_103b9210's cached eCParticleModule, valid once 10878c24 bit 0 is set */
#define SN_PARTICLE_MODULE_SET 0x10878c24u

static uint32_t sn_master(uint32_t ps) {
    if (!(rd32(SN_PARTICLE_MODULE_SET) & 1u))
        return 0;
    const uint32_t mod = rd32(SN_PARTICLE_MODULE), entity = rd32(ps + 0xcu);
    if (!mod || !entity)
        return 0;
    const uint32_t key = rd32(entity + 0x1b4u), buckets = rd32(mod + 0x20u), count = rd32(mod + 0x24u);
    if (!key || !buckets || !count)
        return 0;
    for (uint32_t n = rd32(buckets + ((key >> 4) % count) * 4u); n; n = rd32(n + 0x14u))
        if (rd32(n) == key)
            return rd32(n + 4u);
    return 0;
}

static void sn_presort_particles(X86 *c, uint32_t base, uint32_t num) {
    const uint32_t frame = rd32(rd32(SN_WORLD) + 0x5e8u);
    for (uint32_t i = 0; i < num && sn_nsorted < sizeof sn_sorted / sizeof sn_sorted[0]; ++i) {
        const uint32_t op = base + i * 0x18u, ctx = rd32(op);
        if (!ctx || rd32(ctx) != SN_LEGACY_VFT || rd32(op + 4u) != 1u || (rd8(op + 0x10u) & 1u))
            continue;
        const uint32_t ps = rd32(ctx + 0x10u);
        if (!ps || rd32(ps) != SN_PARTICLE_VFT)
            continue;
        uint32_t target = ps;
        if (rd8(ps + 0x22du) != 0u) {
            /* instanced: RenderVertices 103b4c80 updates the master, and only while the instance's +0x20 is set */
            if (rd8(ps + 0x20u) == 0u)
                continue;
            target = sn_master(ps);
            if (!target)
                continue;
        }
        if (rd32(target + 0x31cu) == 0u || rd32(target + 0x348u) == frame || sn_presorted(target))
            continue;
        c->r[1] = target;
        sn_presorting = 1;
        guest_call(c, SN_DEPTHSORT, 0, 0);
        sn_presorting = 0;
        sn_sorted[sn_nsorted++] = target;
        ++g3_snap_particles_moved;
    }
}

/* Execute's Request loop and the particles' sorts, before Execute; 0 when the database is left to Execute. */
static int sn_gather(X86 *c, uint32_t db) {
    if (rd32(db + 0x134u) == 0u)
        return 0; /* Execute returns at once */
    const uint32_t app = rd32(SN_APP);
    int own_target = 0; /* Execute's bVar1: the device draws to the application's own target */
    if (rd32(app + 0xb88u) != 0u) {
        if (!(rd32(SN_GFX_SET) & 1u) || !rd32(SN_GFX))
            return 0;
        const uint32_t device = rd32(rd32(SN_GFX) + 0x5b8u + 0xcu);
        if (!device || rd32(rd32(device) + 0x21cu) != SN_DX9_GET_RENDER_TARGET)
            return 0;
        own_target = rd32(device + 0x24u) == rd32(app + 0xb94u);
    }
    uint32_t flags = rd8(db + 0x193u) == 1u ? 6u : 2u;
    if (rd8(db + 0x1c6u) == 1u && own_target)
        flags |= 0x20u;
    if (rd8(db + 0x1bdu) == 1u)
        flags |= 0x10u;
    if (rd8(db + 0x1c4u) == 1u)
        flags |= 1u;
    if (rd8(db + 0x1aeu) == 1u)
        flags |= 0x40u;
    if (rd8(db + 0x1ccu) == 1u)
        flags |= 0x80u;
    const uint32_t n = rd32(db + 0x20u), items = rd32(db + 0x1cu);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t args[3] = {db, flags, db + 0x124u};
        c->r[1] = rd32(items + i * 4u);
        guest_call(c, SN_REQUEST, args, 3);
    }
    const uint32_t lists[2] = {0x13cu, 0x184u};
    for (int k = 0; k < 2; ++k)
        sn_presort_particles(c, rd32(db + lists[k]), rd32(db + lists[k] + 4u));
    return 1;
}

void g3n_104035c0(X86 *c) {
    if (sn_gathered_db && rd32(c->r[4] + 4u) == sn_gathered_db) {
        c->r[0] = 1u;
        c->eip = rd32(c->r[4]); /* RET 0xc */
        c->r[4] += 4u + 0xcu;
        recomp_return(c);
        return;
    }
    fn_104035c0(c);
}

/* ---- the far view: its gathering out of the near view's drawing ---------------------------------------------
 *
 * The near view's Execute calls eCCameraBase::RenderLowPoly 102bec80 in the middle of its drawing (10400530.c:589,
 * while the database's +0x1ab is clear and it has a camera, +0x134): the camera's second database (+0xec) gets the
 * near one's (+0xe8) frustum and the application's settings, the clip planes move out, and eCRenderSystemAdmin::Render
 * 1040e430 gathers that database and executes it, nested; then the planes come back. With the group on, RenderLowPoly
 * is called once more right after the near view's gathering in "gather only" mode: its Render runs as it is, but its
 * Execute only gathers (sn_gather) and returns. When the drawing reaches RenderLowPoly, its Render goes straight to
 * Execute (as Render does after its gathering: +0x90 = the database), whose Request loop is skipped. */
#define SN_RENDER_LOW_POLY 0x102bec80u

static int sn_gather_only;       /* inside RenderLowPoly called for its gathering */
static uint32_t sn_far_db;       /* the far database gathered for this near Execute, drawn later */
static int sn_far_done;          /* its Execute gathered in gather only mode */
uint64_t g3_snap_far_gathered;

void g3n_10400530(X86 *c);

static void sn_ret(X86 *c, uint32_t args) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + args;
    recomp_return(c);
}

void g3n_1040e430(X86 *c) {
    /* thiscall: ECX the render system admin, [ESP+4] the database */
    const uint32_t admin = c->r[1], db = rd32(c->r[4] + 4u);
    if (G3OPT(SNAP) && !sn_gather_only && db && db == sn_far_db) {
        /* what Render does after its gathering, as a jump: +0x90 = the database, then Execute returns to Render's
         * caller (Render's RET 4: the return address moves over the argument) */
        wr32(admin + 0x90u, db);
        const uint32_t ret = rd32(c->r[4]);
        c->r[4] += 4u;
        wr32(c->r[4], ret);
        c->r[1] = db;
        g3n_10400530(c);
        return;
    }
    fn_1040e430(c);
}

void g3n_10400530(X86 *c) {
    static int depth;
    const uint32_t db = c->r[1], outer = sn_gathered_db;
    if (G3OPT(SNAP) && sn_gather_only) {
        /* the far view's Execute, gathering only */
        uint32_t r[8];
        for (int i = 0; i < 8; ++i)
            r[i] = c->r[i];
        sn_far_done = sn_gather(c, db);
        for (int i = 0; i < 8; ++i)
            c->r[i] = r[i];
        sn_ret(c, 0u);
        return;
    }
    ++depth;
    sn_gathered_db = 0;
    if (G3OPT(SNAP)) {
        uint32_t r[8];
        for (int i = 0; i < 8; ++i)
            r[i] = c->r[i];
        const uint32_t eip = c->eip;
        if (db == sn_far_db) {
            sn_gathered_db = db; /* gathered before the near view drew */
        } else if (sn_gather(c, db)) {
            sn_gathered_db = db;
            ++g3_snap_gathered;
            const uint32_t cam = rd32(db + 0x134u);
            if (depth == 1 && rd8(db + 0x1abu) == 0u && cam && rd32(cam + 0xe8u) == db && rd8(cam + 0xe1u) != 1u) {
                sn_gather_only = 1;
                sn_far_done = 0;
                c->r[1] = cam;
                guest_call(c, SN_RENDER_LOW_POLY, 0, 0);
                sn_gather_only = 0;
                if (sn_far_done) {
                    sn_far_db = rd32(cam + 0xecu);
                    ++g3_snap_far_gathered;
                }
            }
        } else {
            ++g3_snap_gather_left;
        }
        for (int i = 0; i < 8; ++i)
            c->r[i] = r[i];
        c->eip = eip;
    }
    fn_10400530(c);
    sn_gathered_db = outer;
    if (--depth == 0) {
        sn_nsorted = 0;
        sn_far_db = 0;
    }
    static uint32_t calls;
    if (G3OPT(SNAP) && ++calls % 1200 == 0)
        fprintf(stderr, "g3: snap: gathered before Execute %llu (far view %llu), left to it %llu; particle sorts moved "
                "%llu, left in the drawing %llu\n", (unsigned long long)g3_snap_gathered,
                (unsigned long long)g3_snap_far_gathered, (unsigned long long)g3_snap_gather_left,
                (unsigned long long)g3_snap_particles_moved, (unsigned long long)g3_snap_particles_left);
}

void g3n_103afa70(X86 *c) {
    if (!sn_presorting && sn_nsorted && sn_presorted(c->r[1])) {
        c->eip = rd32(c->r[4]); /* RET: thiscall, no arguments */
        c->r[4] += 4u;
        recomp_return(c);
        return;
    }
    if (G3OPT(SNAP) && !sn_presorting)
        ++g3_snap_particles_left;
    fn_103afa70(c);
}

void g3n_10468f60(X86 *c) {
    fn_10468f60(c);
}
