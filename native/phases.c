/* native/phases.c - where a frame's time goes (docs/perf/wielordzeniowosc.md, stage 0).
 *
 * eCApplication::OnRun (10276720) runs Process (10276490: entities, modules, PhysX) and then DoRender (10276100:
 * camera, render system, shadows, GUI, present). Each is timed here and reported to the kit as a phase
 * (recomp_phase_add, kit/dx/d3d9.cpp), which prints the phases' ms per frame after each frame-times line, beside
 * its own waits on the renderer thread ("present wait", "query wait", both inside "render"). Process is wrapped
 * in mods.c (G3Fixes hook 18), which calls g3_phase.
 *
 * "load" is eCBaseCache::CacheInResource (1044fa80) on the game thread, outermost call only (a mesh caches in
 * its textures): a resource read, inflated and built in the middle of a frame (stage 4). It is inside logic or
 * render, wherever the resource was first needed. */
#include "chunk.h"

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

void recomp_phase_add(const char *name, uint64_t ns); /* kit/dx/d3d9.cpp */
void fn_10276100(X86 *c);                              /* Engine eCApplication::DoRender */
void fn_1044fa80(X86 *c);                              /* Engine eCBaseCache::CacheInResource */

static pthread_t g3_game_thread;
static _Thread_local int g3_load_depth;

uint64_t g3_phase_now(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void g3_phase(const char *name, uint64_t t0) {
    recomp_phase_add(name, g3_phase_now() - t0);
}

void g3_load_frame(void); /* loading.c */
extern uint64_t g3_load_deferred, g3_load_queued;

void g3n_10276100(X86 *c) {
    g3_game_thread = pthread_self();
    g3_load_frame();
    static uint64_t frames, told_deferred;
    if (++frames % 600u == 0u && g3_load_deferred != told_deferred) {
        fprintf(stderr, "[g3] load: %llu texture binds without their image so far, %llu images queued\n",
                (unsigned long long)g3_load_deferred, (unsigned long long)g3_load_queued);
        told_deferred = g3_load_deferred;
    }
    const uint64_t t0 = g3_phase_now();
    fn_10276100(c);
    g3_phase("render", t0);
}

/* The guest stack above esp, scanned for what look like return addresses into Engine, SharedBase and Game. */
static void g3_stack_chain(uint32_t esp, char *chain, size_t cap) {
    int n = 0;
    chain[0] = 0;
    for (uint32_t a = esp, k = 0; a < esp + 0x600u && k < 12 && n < (int)cap - 10; a += 4u) {
        const uint32_t v = rd32(a);
        if ((v >= 0x10001000u && v < 0x10600000u) || (v >= 0x10900000u && v < 0x10b00000u))
            n += snprintf(chain + n, cap - n, " %08x", v), ++k;
    }
}

int g3_on_game_thread(void) {
    return pthread_equal(pthread_self(), g3_game_thread);
}

void g3n_1044fa80(X86 *c) {
    if (g3_load_depth++ || !pthread_equal(pthread_self(), g3_game_thread)) {
        fn_1044fa80(c);
        --g3_load_depth;
        return;
    }
    const uint32_t cache = c->r[R_ECX], ent = rd32(c->r[R_ESP] + 4u);
    const uint32_t res = ent ? rd32(ent + 0x48u) : 0u;
    const uint32_t behaviour = res ? rd32(res + 0x18u) : 0u, state = res ? rd32(res + 0x10u) : 0u;
    if (state == 6u) { /* in already: the call only updates the cache's records, untimed (it is per bind) */
        fn_1044fa80(c);
        --g3_load_depth;
        return;
    }
    const uint64_t t0 = g3_phase_now();
    fn_1044fa80(c);
    --g3_load_depth;
    const uint64_t ns = g3_phase_now() - t0;
    recomp_phase_add("load", ns);
    /* Stage 4: which cache (its vtable) and resource class took a frame's worth, and in what cache behaviour. */
    static int told;
    if (ns > 30000000u && told < 50) {
        ++told;
        char chain[200];
        g3_stack_chain(c->r[R_ESP], chain, sizeof chain);
        fprintf(stderr, "[g3] slow load %.1f ms: cache %08x (vtable %08x), resource vtable %08x, behaviour %u, state %u;"
                        " stack%s\n", ns / 1e6, cache, rd32(cache), res ? rd32(res) : 0u, behaviour, state, chain);
    }
}
