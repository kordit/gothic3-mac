/* native/loading.c - the engine's image cache loads on a thread of its own again (docs/perf/wielordzeniowosc.md,
 * stage 4, milestone 7.7).
 *
 * Community Patch 1.75's eCApplication::OnInitializeEngine (1026c3e0) sets every cache's thread count in
 * eSSetupEngine to 0 (1026c3e0.c:340-347) and leaves threading on (+0x84 = 1, which Game's PostInitialize hands to
 * eCThreadPool::EnableAllThreads). A cache with 0 threads creates no pool (eCImageCache::Create 10451f30), so
 * eCThreadPool::InsertWorkItem (10453c70) runs each item in place: a texture the renderer asks for lazily
 * (eCBaseCache::CacheInResource 1044fa80, behaviour 2) is read, inflated and built inside the frame that first draws
 * it. The player's log of 27.10: 2-5 frames over 50 ms in every 10 s of walking, the slowest 100-250 ms.
 *
 * g3n_1026c3e0 gives the image cache (+0x68) LD_IMAGE_THREADS after the original has run. eCApplication::CreateEngine
 * (10270e40) passes the count to SetThreadNumber, the pool's thread (eCThreadPool::g_ThreadFunc 10454110) takes the
 * queued loads, and the frame draws without the texture until it is in, as the game did before the patch. The
 * thread is a guest thread under the baton like any other, so guest code still never runs on two cores at once;
 * what runs beside the game thread is the inflate of the entry (archive.c), which lends the baton for its host-only
 * work. Group "load"; off, the counts stay the patch's.
 *
 * What the renderer draws with is a different path. The texture component's bind (FUN_104a1260, a shader component's
 * vtable +8 under ExecutePass) finds the image not loaded (state other than 6), sets its behaviour to 1 and caches it
 * in on the spot: read, inflate and the texture built inside the draw. Walking with World Texture Mix (work/walk-probe.sh,
 * 27.10): 47 such loads of 8-25 ms in 2 minutes, up to 94 ms of them in one frame. g3n_104a1260 instead asks for the
 * image lazily (eCResourceBase_PS::Load(3), so behaviour 2, which CacheInResource queues to the image cache's thread),
 * and the draw goes without it: the unit gets no texture (SetTexture(unit, NULL)) and the engine's records of what is
 * bound to that unit are cleared, so the next bind of any image there really binds it. Once the loader has the
 * image in, the next draw binds it as usual. LD_SYNC_MS of loading per frame still goes the original's way. */
#include "chunk.h"
#include "g3_opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void fn_1026c3e0(X86 *c); /* Engine eCApplication::OnInitializeEngine(eSSetupEngine &) */
void fn_104a1260(X86 *c); /* Engine: a texture component binds its image to a unit */
uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");
int g3_on_game_thread(void);
void fn_1044f1d0(X86 *c); /* Engine eCBaseCache::eCThreadPoolWorkerItem_Do::DoWork */
uint64_t g3_phase_now(void);
void g3_phase(const char *name, uint64_t t0);

#define LD_SETUP_IMAGE_THREADS 0x68u
#define LD_IMAGE_THREADS 1u
#define LD_RES_LOAD 0x10411d80u          /* eCResourceBase_PS::Load(behaviour), thiscall, RET 4 */
#define LD_DX9_DEVICE_VTABLE 0x1068aafcu /* eCDX9Device: +8 the IDirect3DDevice9 */
#define LD_ACTIVE_IMAGE 0x10862bb0u      /* eCGfxImageData::ms_pActiveImageData[unit] */

static int ld_pool;                       /* the image cache was given its thread */
static uint64_t ld_frame_sync_ns;         /* the original's loads in this frame */
static uint64_t ld_sync_budget_ns = ~0ull; /* G3_TEX_SYNC_MS, read once */
uint64_t g3_load_deferred, g3_load_queued; /* binds drawn without their image, images queued, for the log */
static uint32_t ld_frame;                  /* frames since start */

/* Images already queued: until the loader thread starts one its state stays 2, and every draw of it would queue it
 * again (DoWork loads unconditionally). An entry is good for LD_REQUEUE frames, then a still missing image may go
 * again (cached out and needed anew, or an item that failed). */
#define LD_SLOTS 2048u
#define LD_REQUEUE 600u
static uint32_t ld_res[LD_SLOTS], ld_when[LD_SLOTS];

static int ld_queued_recently(uint32_t res) {
    uint32_t h = (res >> 3) * 2654435761u % LD_SLOTS;
    for (uint32_t k = 0; k < 16u; ++k, h = (h + 1u) % LD_SLOTS) {
        if (ld_res[h] == res)
            return ld_frame - ld_when[h] < LD_REQUEUE;
        if (ld_res[h] == 0u)
            return 0;
    }
    return 0;
}

static void ld_note_queued(uint32_t res) {
    uint32_t h = (res >> 3) * 2654435761u % LD_SLOTS, oldest = h;
    for (uint32_t k = 0; k < 16u; ++k, h = (h + 1u) % LD_SLOTS) {
        if (ld_res[h] == res || ld_res[h] == 0u) {
            oldest = h;
            break;
        }
        if (ld_frame - ld_when[h] > ld_frame - ld_when[oldest])
            oldest = h;
    }
    ld_res[oldest] = res;
    ld_when[oldest] = ld_frame;
}

void g3n_1026c3e0(X86 *c) {
    const uint32_t setup = rd32(c->r[R_ESP] + 4u);
    fn_1026c3e0(c);
    if (G3OPT(LOAD) && rd32(setup + LD_SETUP_IMAGE_THREADS) == 0u) {
        wr32(setup + LD_SETUP_IMAGE_THREADS, LD_IMAGE_THREADS);
        ld_pool = 1;
        fprintf(stderr, "[g3] load: the image cache gets %u loader thread\n", LD_IMAGE_THREADS);
    }
}

/* A queued item's work on a loader thread, as the phase "bg load": wall time, so it includes the stretches in which
 * the game thread had the baton. Under the baton like the phases' other callers. */
void g3n_1044f1d0(X86 *c) {
    const uint32_t item = c->r[R_ECX];
    const uint32_t res = rd32(item + 0x10u), kind = rd32(item + 8u);
    const uint64_t t0 = g3_phase_now();
    fn_1044f1d0(c);
    const uint64_t ns = g3_phase_now() - t0;
    g3_phase("bg load", t0);
    static int told;
    if (ns > 40000000u && told < 100) {
        ++told;
        fprintf(stderr, "[g3] load: a loader item took %.1f ms (kind %u, resource vtable %08x)\n", ns / 1e6, kind,
                res ? rd32(res) : 0u);
    }
}

/* A new frame (DoRender, phases.c): the per-frame allowance for loads done the original's way starts again. */
void g3_load_frame(void) {
    ld_frame_sync_ns = 0;
    ++ld_frame;
}

static uint64_t ld_now(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* FUN_104a1260 (thiscall ECX = the component; stack: the shader state, the unit's parameters; RET 8; AL = bound).
 * The component's image object is [ECX+0x34] (state +0x10, 6 = in; its data entity from vtable +0x70, whose +0x48 is
 * the resource), the unit is [param+0x28], the device state [state+4] (+0x7c4 + unit * 4: the image bound there) and
 * the device wrapper [[state+8]+0xc]. */
void g3n_104a1260(X86 *c) {
    uint32_t *const r = c->r;
    const uint32_t comp = r[R_ECX], esp = r[R_ESP];
    const uint32_t img = rd32(comp + 0x34u);
    if (!G3OPT(LOAD) || !ld_pool || img == 0u || rd32(img + 0x10u) == 6u || !g3_on_game_thread()) {
        fn_104a1260(c);
        return;
    }
    if (ld_sync_budget_ns == ~0ull) {
        const char *e = getenv("G3_TEX_SYNC_MS");
        ld_sync_budget_ns = (uint64_t)(e ? atof(e) * 1e6 : 0.0);
    }
    if (ld_frame_sync_ns < ld_sync_budget_ns) {
        const uint64_t t0 = ld_now();
        fn_104a1260(c);
        ld_frame_sync_ns += ld_now() - t0;
        return;
    }
    const uint32_t state = rd32(esp + 4u), unit_par = rd32(esp + 8u);
    const uint32_t saved[8] = {r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]};
    r[R_ECX] = img;
    const uint32_t ent = guest_call(c, rd32(rd32(img) + 0x70u), 0, 0);
    const uint32_t res = ent ? rd32(ent + 0x48u) : 0u;
    /* Behaviour 0 never queues (eCResourceBase_PS::Load keeps it), and a locked resource (+0x20 bit 0) is not
     * loaded by the original either: both go the original's way. */
    if (res == 0u || rd32(res + 0x18u) == 0u || (rd8(res + 0x20u) & 1u)) {
        for (int i = 0; i < 8; ++i)
            r[i] = saved[i];
        fn_104a1260(c);
        return;
    }
    if (rd32(res + 0x10u) != 3u && !ld_queued_recently(res)) {
        const uint32_t lazy = 3u;
        r[R_ECX] = res;
        guest_call(c, LD_RES_LOAD, &lazy, 1);
        ld_note_queued(res);
        ++g3_load_queued;
    }
    ++g3_load_deferred;
    const uint32_t unit = rd32(unit_par + 0x28u);
    if (unit < 16u) {
        const uint32_t dev_state = rd32(state + 4u), wrapper = rd32(rd32(state + 8u) + 0xcu);
        wr32(dev_state + 0x7c4u + unit * 4u, 0u);
        wr32(LD_ACTIVE_IMAGE + unit * 4u, 0u);
        if (wrapper && rd32(wrapper) == LD_DX9_DEVICE_VTABLE) {
            const uint32_t dev = rd32(wrapper + 8u);
            if (dev) {
                const uint32_t args[3] = {dev, unit, 0u};
                guest_call(c, rd32(rd32(dev) + 0x104u), args, 3); /* IDirect3DDevice9::SetTexture(unit, NULL) */
            }
        }
    }
    for (int i = 0; i < 8; ++i)
        r[i] = saved[i];
    r[R_EAX] &= 0xffffff00u; /* AL = 0: nothing bound, as when the image data has no texture */
    c->eip = rd32(esp);
    r[R_ESP] = esp + 12u;
    recomp_return(c);
}
