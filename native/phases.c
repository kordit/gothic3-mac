/* native/phases.c - where a frame's time goes (docs/perf/wielordzeniowosc.md, stage 0).
 *
 * eCApplication::OnRun (10276720) runs Process (10276490: entities, modules, PhysX) and then DoRender (10276100:
 * camera, render system, shadows, GUI, present). Each is timed here and reported to the kit as a phase
 * (recomp_phase_add, kit/dx/d3d9.cpp), which prints the phases' ms per frame after each frame-times line, beside
 * its own waits on the renderer thread ("present wait", "query wait", both inside "render"). Process is wrapped
 * in mods.c (G3Fixes hook 18), which calls g3_phase. */
#include "chunk.h"

#include <stdint.h>
#include <time.h>

void recomp_phase_add(const char *name, uint64_t ns); /* kit/dx/d3d9.cpp */
void fn_10276100(X86 *c);                              /* Engine eCApplication::DoRender */

uint64_t g3_phase_now(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void g3_phase(const char *name, uint64_t t0) {
    recomp_phase_add(name, g3_phase_now() - t0);
}

void g3n_10276100(X86 *c) {
    const uint64_t t0 = g3_phase_now();
    fn_10276100(c);
    g3_phase("render", t0);
}
