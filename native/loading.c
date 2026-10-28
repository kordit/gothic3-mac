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
#include <string.h>
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
 * again (cached out and needed anew, or an item that failed).
 *
 * Session 16 part 2, the player's "shadows are gone": an image drawn without its texture is black (SetTexture NULL),
 * and after a load the cave's terrain was black for about 6 s under a busy CPU (work/look-str-heap) while the one
 * loader thread caught up. One still missing LD_FALLBACK frames (about 0.75 s) after it was queued is bound the
 * original's way, in the draw, as before stage 4; so is every bind in the first seconds after a load (LD_GRACE_NS). */
#define LD_SLOTS 2048u
#define LD_REQUEUE 600u
#define LD_FALLBACK 45u
static uint32_t ld_res[LD_SLOTS], ld_when[LD_SLOTS];
uint64_t g3_load_fallback; /* binds that waited too long and went the original's way */

/* Frames since `res` was queued, or ~0u when it was not (or so long ago that it may go again). */
static uint32_t ld_queued_age(uint32_t res) {
    uint32_t h = (res >> 3) * 2654435761u % LD_SLOTS;
    for (uint32_t k = 0; k < 16u; ++k, h = (h + 1u) % LD_SLOTS) {
        if (ld_res[h] == res)
            return ld_frame - ld_when[h] < LD_REQUEUE ? ld_frame - ld_when[h] : ~0u;
        if (ld_res[h] == 0u)
            return ~0u;
    }
    return ~0u;
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
        /* G3_LOAD_THREADS=<n> (1-4): another count, for a measurement. */
        const char *e = getenv("G3_LOAD_THREADS");
        const uint32_t n = e && atoi(e) >= 1 && atoi(e) <= 4 ? (uint32_t)atoi(e) : LD_IMAGE_THREADS;
        wr32(setup + LD_SETUP_IMAGE_THREADS, n);
        ld_pool = 1;
        fprintf(stderr, "[g3] load: the image cache gets %u loader thread%s\n", n, n == 1 ? "" : "s");
    }
    /* G3_CACHE_THREADS=mesh,anim,sound,material,collision,vpool (session 18, a measurement): those caches get a
     * loader thread too. The player's stutters walking in the Reddock cave were DoLoad reading archive entries
     * inside the frame (kit hostprof spike stacks); eCApplication::CreateEngine 10270e40 names the counts (Image,
     * Material, Mesh, Sound, Vertexpool, CollisionMesh, Animation) at these offsets of eSSetupEngine. */
    static const struct {
        const char *name;
        uint32_t at;
    } caches[] = {{"vpool", 0x64u}, {"material", 0x6cu}, {"mesh", 0x70u}, {"sound", 0x74u}, {"collision", 0x7cu}, {"anim", 0x80u}};
    const char *more = getenv("G3_CACHE_THREADS");
    for (size_t i = 0; G3OPT(LOAD) && more && i < sizeof caches / sizeof caches[0]; ++i)
        if (strstr(more, caches[i].name) && rd32(setup + caches[i].at) == 0u) {
            wr32(setup + caches[i].at, 1u);
            fprintf(stderr, "[g3] load: the %s cache gets a loader thread (G3_CACHE_THREADS)\n", caches[i].name);
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
    const int game = g3_on_game_thread();
    if (ns > (game ? 15000000u : 40000000u) && told < 200) {
        ++told;
        fprintf(stderr, "[g3] load: a loader item took %.1f ms (kind %u, resource vtable %08x, %s)\n", ns / 1e6, kind,
                res ? rd32(res) : 0u, game ? "on the game thread" : "on a loader thread");
    }
}

static uint64_t ld_now(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* For LD_GRACE_NS after a stop of more than LD_GAP_NS between frames (a loading screen, a save's load), binds go the
 * original's way: the first frames of a newly loaded world would otherwise draw everything not in yet without its
 * texture, black, for the seconds the one loader thread needs (session 16 part 2, the player's cave after F9). */
#define LD_GAP_NS 500000000ull
#define LD_GRACE_NS 4000000000ull
static uint64_t ld_last_frame_ns, ld_grace_until_ns;
static int ld_grace;

/* A new frame (DoRender, phases.c): the per-frame allowance for loads done the original's way starts again. */
void g3_load_frame(void) {
    ld_frame_sync_ns = 0;
    ++ld_frame;
    const uint64_t now = ld_now();
    if (ld_last_frame_ns && now - ld_last_frame_ns > LD_GAP_NS)
        ld_grace_until_ns = now + LD_GRACE_NS;
    ld_grace = now < ld_grace_until_ns;
    ld_last_frame_ns = now;
}

/* FUN_104a1260 (thiscall ECX = the component; stack: the shader state, the unit's parameters; RET 8; AL = bound).
 * The component's image object is [ECX+0x34] (state +0x10, 6 = in; its data entity from vtable +0x70, whose +0x48 is
 * the resource), the unit is [param+0x28], the device state [state+4] (+0x7c4 + unit * 4: the image bound there) and
 * the device wrapper [[state+8]+0xc]. */
void g3n_104a1260(X86 *c) {
    uint32_t *const r = c->r;
    const uint32_t comp = r[R_ECX], esp = r[R_ESP];
    const uint32_t img = rd32(comp + 0x34u);
    if (!G3OPT(LOAD) || !ld_pool || img == 0u || rd32(img + 0x10u) == 6u || ld_grace || !g3_on_game_thread()) {
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
    const uint32_t age = ld_queued_age(res);
    if (age != ~0u && age >= LD_FALLBACK) {
        static int told;
        if (told < 40) {
            ++told;
            fprintf(stderr, "[g3] load: an image still not in %u frames after it was queued (resource vtable %08x, "
                            "state %u, behaviour %u, image state %u): bound in the draw\n",
                    age, rd32(res), rd32(res + 0x10u), rd32(res + 0x18u), rd32(img + 0x10u));
        }
        ++g3_load_fallback;
        for (int i = 0; i < 8; ++i)
            r[i] = saved[i];
        fn_104a1260(c);
        return;
    }
    if (age == ~0u) {
        if (rd32(res + 0x10u) != 3u) { /* 3: queued already, by the engine */
            const uint32_t lazy = 3u;
            r[R_ECX] = res;
            guest_call(c, LD_RES_LOAD, &lazy, 1);
            ++g3_load_queued;
        }
        ld_note_queued(res); /* from now on its wait counts towards LD_FALLBACK */
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
