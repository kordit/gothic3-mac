/* native/phases.c - where a frame's time goes (docs/perf/wielordzeniowosc.md, stage 0).
 *
 * eCApplication::OnRun (10276720) runs Process (10276490: entities, modules, PhysX) and then DoRender (10276100:
 * camera, render system, shadows, GUI, present). Each is timed here and reported to the kit as a phase
 * (recomp_phase_add, kit/dx/d3d9.cpp), which prints the phases' ms per frame after each frame-times line, beside
 * its own waits on the renderer thread ("present wait", "query wait", both inside "render"). Process is wrapped
 * in mods.c (G3Fixes hook 18), which calls g3_phase.
 *
 * MEMTRACE=<frames>,<seconds> in the trace control file (D-054 step 1): after <seconds> from the line's arrival, the
 * next <frames> iterations trace which guest memory Process and DoRender each read and write (kit/runtime/memtrace.h),
 * as "logic N" and "render N" in MEMTRACE_OUT=<file> (memtrace.txt in the working directory without it). */
#include "chunk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int recomp_memtrace_begin(void);                          /* kit/runtime/memtrace.h */
void recomp_memtrace_end(const char *label, const char *path);

void recomp_phase_add(const char *name, uint64_t ns); /* kit/dx/d3d9.cpp */
void fn_10276100(X86 *c);                              /* Engine eCApplication::DoRender */

uint64_t g3_phase_now(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void g3_phase(const char *name, uint64_t t0) {
    recomp_phase_add(name, g3_phase_now() - t0);
}

static uint32_t mt_frames, mt_done; /* the control text's frames, and how many are traced */
static uint64_t mt_from;            /* g3_phase_now from which they are */
static char mt_path[512] = "memtrace.txt";

void g3_memtrace_control(const char *text) {
    const char *key = strstr(text, "MEMTRACE=");
    const char *out = strstr(text, "MEMTRACE_OUT=");
    if (out && (out == text || out[-1] == '\n')) {
        size_t n = strcspn(out + 13, "\r\n");
        if (n >= sizeof mt_path)
            n = sizeof mt_path - 1;
        memcpy(mt_path, out + 13, n);
        mt_path[n] = 0;
    }
    if (!key || (key != text && key[-1] != '\n')) {
        mt_frames = 0;
        return;
    }
    char *end;
    const uint32_t frames = (uint32_t)strtoul(key + 9, &end, 10);
    const double seconds = *end == ',' ? strtod(end + 1, NULL) : 0;
    if (frames != mt_frames || !mt_from) {
        mt_frames = frames;
        mt_done = 0;
        mt_from = g3_phase_now() + (uint64_t)(seconds * 1e9);
        fprintf(stderr, "g3: memtrace %u frames after %.0f s to %s\n", frames, seconds, mt_path);
    }
}

/* Process and DoRender of one iteration, traced one after the other while frames are left. */
int g3_memtrace_frame(void) {
    return mt_done < mt_frames && g3_phase_now() >= mt_from;
}
void g3_memtrace_end(const char *what) {
    char label[48];
    snprintf(label, sizeof label, "%s %u", what, mt_done);
    recomp_memtrace_end(label, mt_path);
}

void g3n_10276100(X86 *c) {
    const uint64_t t0 = g3_phase_now();
    const int traced = g3_memtrace_frame() && recomp_memtrace_begin();
    fn_10276100(c);
    if (traced) {
        g3_memtrace_end("render");
        ++mt_done;
    }
    g3_phase("render", t0);
}
