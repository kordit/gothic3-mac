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
void fn_103ffbe0(X86 *c); /* Engine eCFrustumDatabase::ExecuteRenderList */
void fn_1040c140(X86 *c); /* Engine eCRenderOp2::Render */
void fn_10468f60(X86 *c); /* Engine eCSpeedTreeAdmin::OnBeginRenderSolid */

void g3n_1040e430(X86 *c) {
    fn_1040e430(c);
}

/* ---- particles: DepthSortParticles at the end of the gathering ----------------------------------------------
 *
 * UpdateVertices 103b3630 (from RenderVertices 103b4c80, from the virtual OnRenderAlpha 103aee30 of an alpha render
 * op) sorts an emitter's particle array in place, back to front from the camera, once a frame (its +0x348 against
 * the world's frame number), before it writes the vertices. The array is the logic's own (EmitParticles, Spawn,
 * TickParticles walk it in that order). Moved: before the alpha list is drawn, each emitter an op of it will draw is
 * sorted, by the same code with the same camera, and the sort in UpdateVertices finds it done.
 *
 * An instanced emitter (+0x22d set) draws through its master: that one is sorted when it exists already (sn_master);
 * a master GetMasterInstance 103c0cb0 has yet to make (an entity cloned in the render, itself a write to the logic's
 * state, not moved yet) and an empty emitter (filled by OnRenderAlpha's virtual +0x124) are left to the drawing.
 * Measured (30.10.2026, chodzenie, pinned, 60 s): 14286 sorts moved, 2 left; frames as the reference's but for the
 * known single sky tile (tools/frames_diff.py). */
#define SN_WORLD 0x10862a88u /* the object whose +0x5e8 is the frame number (native/particles.c ENGINE_WORLD) */
#define SN_PARTICLE_VFT 0x106bdcbcu /* eCParticle_PS's vtable (RTTI: COL 107021b8, stage/Engine.dll) */
#define SN_DEPTHSORT 0x103afa70u

static uint32_t sn_sorted[4096], sn_nsorted; /* emitters sorted after this Execute's alpha list was */
uint64_t g3_snap_particles_moved, g3_snap_particles_left; /* sorts moved; sorts still made in the drawing */
static int sn_presorting;

static int sn_presorted(uint32_t ps) {
    for (uint32_t i = 0; i < sn_nsorted; ++i)
        if (sn_sorted[i] == ps)
            return 1;
    return 0;
}

/* The alpha list (eCFrustumDatabase +0x13c, eCRenderOp2 records of 0x18 bytes) is drawn by ExecuteRenderList 103ffbe0
 * (10400530.c:1061), sorted first only while the database's +0x193 is set (:1058); its ops are made inside Execute,
 * so the list exists only from there. An op's +0 is its render context; a legacy one (eCRenderContextLegacy::Render
 * 1040d8b0) calls its object's (ctx +0x10) OnRenderAlpha, slot +0x100, when the op's +4 is 1. For an emitter of its
 * own that is OnRenderAlpha 103aee30 -> RenderVertices -> UpdateVertices(this). So the sorts are made at the entry of
 * ExecuteRenderList for that list, in the list's order, which is the order the drawing would make them in. */
#define SN_LEGACY_VFT 0x106bdb54u /* eCRenderContextLegacy's vtable (RTTI) */

/* An instance's master as eCParticleModule::GetMasterInstance 103c0cb0 finds it, without making one: the key is the
 * instance's entity's creator (vtable +0x6c GetAnimationRenderHook-shaped getter = [ps+0xc], then GetCreator 10488a80
 * = [entity+0x1b4]), looked up by FUN_103c1ab0 in the module's map at +0x20 (buckets) / +0x24 (count): bucket
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
    uint32_t r[8];
    for (int i = 0; i < 8; ++i)
        r[i] = c->r[i];
    const uint32_t eip = c->eip, frame = rd32(rd32(SN_WORLD) + 0x5e8u);
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
    for (int i = 0; i < 8; ++i)
        c->r[i] = r[i];
    c->eip = eip;
}

/* The second alpha list (+0x184), which Execute draws with its own loop of eCRenderOp2::Render 1040c140 right before
 * the first (10400530.c:999-1006, while the database's +0x1c6 and its render target are set): its sorts are made
 * when that loop reaches its first op. */
void g3n_1040c140(X86 *c) {
    /* thiscall: ECX the op, [ESP+4] the database, [ESP+8] the alpha flag */
    const uint32_t op = c->r[1], db = rd32(c->r[4] + 4u);
    if (G3OPT(SNAP) && db && op == rd32(db + 0x184u) && rd32(db + 0x188u) != 0u)
        sn_presort_particles(c, op, rd32(db + 0x188u));
    fn_1040c140(c);
}

void g3n_103ffbe0(X86 *c) {
    /* thiscall: ECX the database, [ESP+4] the list, [ESP+8] the alpha flag */
    const uint32_t db = c->r[1], list = rd32(c->r[4] + 4u);
    if (G3OPT(SNAP) && list == db + 0x13cu)
        sn_presort_particles(c, rd32(list), rd32(list + 4u));
    fn_103ffbe0(c);
}

void g3n_10400530(X86 *c) {
    sn_nsorted = 0;
    fn_10400530(c);
    sn_nsorted = 0;
    static uint32_t calls;
    if (G3OPT(SNAP) && ++calls % 1200 == 0)
        fprintf(stderr, "g3: snap: particle sorts moved %llu, left in the drawing %llu\n",
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
