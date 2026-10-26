/* native/audio.c - sound occlusion updates (PUP Opt.SmartAudioOcclusionUpdate).
 *
 * Each g3n_<addr> stands in for the translated fn_<addr> (native/g3_overrides.h). A native version
 * runs under G3OPT(AUDIO) and ends as the translated function does (see its RET in
 * build/recomp/gen/aux-engine/chunk_102b4000.c). Design and numbers: docs/perf/audio-occlusion.md.
 *
 * eCAudioModule::UpdateOcclusion(FMOD::Channel *, bool) (102b48e0) casts two rays per 3D channel
 * through FUN_102b4490 (TraceRayNearestHit with an entity filter): source -> listener and listener
 * -> source. The distance between the two first hits is the wall thickness; it and the material
 * of the hit give a target occlusion, and the channel's occlusion moves towards it by 2*dt of the
 * way per call. eCAudioChannel::SetLocation calls it for every emitter every frame, and
 * eCAudioModule::UpdateOcclusion() (102b4810) for up to three playing channels per frame, so the
 * same rays are cast again and again while nothing moves.
 *
 * Here FUN_102b4490 keeps the answers of recent rays. A ray whose both ends lie within
 * max(AUDIO_EPS_ABS, AUDIO_EPS_REL * length) of a ray traced at most AUDIO_MAX_AGE_US ago, in
 * the same physics scene, gets that ray's answer without PhysX. Everything else, and everything
 * with the group off, goes to the original. UpdateOcclusion itself and the per-frame loop stay the
 * originals, so the smoothing, FMOD calls and channel order are untouched. */
#include "chunk.h"
#include "g3_opt.h"

#include <math.h>
#include <string.h>
#include <time.h>

void fn_102b4810(X86 *c); /* Engine eCAudioModule::UpdateOcclusion() (every channel, each frame) */
void fn_102b48e0(X86 *c); /* Engine eCAudioModule::UpdateOcclusion(FMOD::Channel *, bool) */
void fn_102b4490(X86 *c); /* Engine audio occlusion ray (TraceRayNearestHit) */

#define AUDIO_SCENE_PTR 0x10862dccu /* eCPhysicsScene::s_pCurrentInstance */
#define AUDIO_SCENE_FILTER 0xa0u    /* scene field the ray sets to its filter; the trace clears it */

/* World units are centimetres. 20 cm or 2% of the ray (1.1 degrees at the far end), whichever
 * is larger; a result lives 200 ms. Reasons in docs/perf/audio-occlusion.md. */
#define AUDIO_EPS_ABS 20.0f
#define AUDIO_EPS_REL 0.02f
#define AUDIO_MAX_AGE_US 200000u
#define AUDIO_CACHE_N 256u

typedef struct {
    uint64_t t;          /* traced at (g3n_audio_clock_us), 0 = empty */
    uint32_t scene;      /* eCPhysicsScene the ray ran in */
    float o[3], e[3];    /* origin and end (origin + direction) */
    float eps2;          /* squared tolerance for both ends */
    uint32_t hit;        /* AL of the original */
    uint32_t eax, ecx, edx; /* EAX/ECX/EDX as the original left them */
    uint32_t mat;        /* hit material ([report+0x64]) */
    uint32_t pos[3];     /* hit position, float bits */
} AudioRay;

typedef struct {
    uint64_t calls;    /* FUN_102b4490 calls with the group on */
    uint64_t hits;     /* answered from the cache */
    uint64_t traced;   /* went to the original */
    uint64_t uncached; /* traced but not kept (scene left its filter set, or scene changed) */
} G3nAudioStats;

static AudioRay audio_rays[AUDIO_CACHE_N];
G3nAudioStats g3n_audio_stats;

static uint64_t audio_host_clock_us(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000u;
}
/* Tests replace it. Never returns 0 in practice; 0 marks an empty slot. */
uint64_t (*g3n_audio_clock_us)(void) = audio_host_clock_us;

void g3n_audio_flush(void) {
    for (unsigned i = 0; i < AUDIO_CACHE_N; ++i)
        audio_rays[i].t = 0;
}

static float audio_d2(const float *a, const float *b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

/* The entry for this ray, or 0. NaN anywhere compares false and misses. */
static AudioRay *audio_find(uint32_t scene, const float *o, const float *e, uint64_t now) {
    for (unsigned i = 0; i < AUDIO_CACHE_N; ++i) {
        AudioRay *r = &audio_rays[i];
        if (!r->t || r->scene != scene || now - r->t > AUDIO_MAX_AGE_US)
            continue;
        if (audio_d2(r->o, o) <= r->eps2 && audio_d2(r->e, e) <= r->eps2)
            return r;
    }
    return 0;
}

/* An empty or expired slot, else the oldest. */
static AudioRay *audio_slot(uint64_t now) {
    AudioRay *v = &audio_rays[0];
    for (unsigned i = 0; i < AUDIO_CACHE_N; ++i) {
        AudioRay *r = &audio_rays[i];
        if (!r->t || now - r->t > AUDIO_MAX_AGE_US)
            return r;
        if (r->t < v->t)
            v = r;
    }
    return v;
}

void g3n_102b4810(X86 *c) {
    fn_102b4810(c);
}

void g3n_102b48e0(X86 *c) {
    fn_102b48e0(c);
}

/* FUN_102b4490: ECX = origin, EAX = direction (bCVector *, the segment origin..origin+dir),
 * EDI = hit position out, [ESP+4] = hit material out; returns AL = hit, RET 4. ESI preserved,
 * EBX/EBP/EDI untouched. On a hit EAX = (material out & ~0xff) | 1 and EDX = material, as the
 * original leaves them; on a miss EAX/EDX as the trace left them. ECX (scratch, the trace's
 * leftover) is the one the traced ray left. The physics scene's query
 * report is not refilled on an answer from the cache: nothing reads it after this function
 * (102b48e0 uses the outputs only) and the next trace resets it. */
void g3n_102b4490(X86 *c) {
    if (!G3OPT(AUDIO)) {
        fn_102b4490(c);
        return;
    }
    const uint32_t esp = c->r[4], ret = rd32(esp), matp = rd32(esp + 4u), out = c->r[7];
    const uint32_t scene = rd32(AUDIO_SCENE_PTR);
    float o[3], e[3];
    for (int k = 0; k < 3; ++k) {
        o[k] = rdf32(c->r[1] + 4u * k);
        e[k] = o[k] + rdf32(c->r[0] + 4u * k);
    }
    const uint64_t now = g3n_audio_clock_us();
    ++g3n_audio_stats.calls;

    const AudioRay *r = audio_find(scene, o, e, now);
    if (r) {
        ++g3n_audio_stats.hits;
        if (r->hit) {
            /* As 102b44c2..102b44dc: three FLD/FSTP pairs, the material through EDX. */
            for (int k = 0; k < 3; ++k) {
                float v;
                memcpy(&v, &r->pos[k], 4);
                fpush(c, (double)v);
                wrf32(out + 4u * k, fto_float(c, ST(c, 0)));
                fdrop(c);
            }
            c->r[2] = r->mat;
            wr32(matp, r->mat);
            c->r[0] = (matp & 0xffffff00u) | 1u;
        } else {
            c->r[0] = r->eax;
            c->r[2] = r->edx;
        }
        c->r[1] = r->ecx;
        c->eip = ret;
        c->r[4] = esp + 8u;
        recomp_return(c);
        return;
    }

    ++g3n_audio_stats.traced;
    fn_102b4490(c);
    /* A trace that did not run to the end (no PhysX scene yet, scene locked) leaves the filter
     * set; such an answer is not kept. */
    if (rd32(AUDIO_SCENE_PTR) != scene || !scene || rd32(scene + AUDIO_SCENE_FILTER) != 0u) {
        ++g3n_audio_stats.uncached;
        return;
    }
    const float len = sqrtf(audio_d2(o, e));
    if (!(len < 1e30f)) { /* NaN or infinite ends: never matches sensibly */
        ++g3n_audio_stats.uncached;
        return;
    }
    AudioRay *w = audio_slot(now);
    w->t = now ? now : 1u;
    w->scene = scene;
    memcpy(w->o, o, sizeof o);
    memcpy(w->e, e, sizeof e);
    const float eps = len * AUDIO_EPS_REL > AUDIO_EPS_ABS ? len * AUDIO_EPS_REL : AUDIO_EPS_ABS;
    w->eps2 = eps * eps;
    w->hit = c->r[0] & 0xffu;
    w->eax = c->r[0];
    w->ecx = c->r[1];
    w->edx = c->r[2];
    if (w->hit) {
        w->mat = rd32(matp);
        for (int k = 0; k < 3; ++k)
            w->pos[k] = rd32(out + 4u * k);
    }
}
