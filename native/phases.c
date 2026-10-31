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
uint32_t host_pinned_clock_value(void) __asm__("__Z23host_pinned_clock_valuev"); /* kit/runtime/win32.h */
void recomp_memtrace_end(const char *label, const char *path);

void recomp_phase_add(const char *name, uint64_t ns); /* kit/dx/d3d9.cpp */
void recomp_pinned_uncounted(int on);                 /* kit/runtime/kernel32.cpp: a pinned run's counting */
void fn_10276100(X86 *c);                              /* Engine eCApplication::DoRender */

uint64_t g3_phase_now(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void g3_phase(const char *name, uint64_t t0) {
    recomp_phase_add(name, g3_phase_now() - t0);
}

static uint32_t mt_frames, mt_done; /* the control text's frames, and how many are traced */
static uint64_t mt_from;            /* g3_phase_now from which they are */
static uint32_t mt_from_clock;      /* or the pinned clock from which they are (MEMTRACE=<frames>,t<ms>) */
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
    /* <frames>,t<ms>: from the frame whose pinned clock (RECOMP_PIN_CLOCK) reaches <ms>, the same frame in every run
     * of one script, where seconds of the machine's time are not (session 31: walka's frame 5098, t=83252). */
    const int by_clock = *end == ',' && end[1] == 't';
    const double seconds = *end == ',' && !by_clock ? strtod(end + 1, NULL) : 0;
    if (frames != mt_frames || !mt_from) {
        mt_frames = frames;
        mt_done = 0;
        mt_from = g3_phase_now() + (uint64_t)(seconds * 1e9);
        mt_from_clock = by_clock ? (uint32_t)strtoul(end + 2, NULL, 10) : 0;
        if (by_clock)
            fprintf(stderr, "g3: memtrace %u frames from guest clock %u ms to %s\n", frames, mt_from_clock, mt_path);
        else
            fprintf(stderr, "g3: memtrace %u frames after %.0f s to %s\n", frames, seconds, mt_path);
    }
}

/* Process and DoRender of one iteration, traced one after the other while frames are left. */
int g3_memtrace_frame(void) {
    if (mt_from_clock)
        return mt_done < mt_frames && host_pinned_clock_value() >= mt_from_clock;
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
    /* In a pinned run the drawing's calls and clock polls are not counted (kit/runtime/kernel32.cpp,
     * recomp_pinned_uncounted): the game's time, and where the other threads run, must not follow how often the
     * render asks. */
    recomp_pinned_uncounted(1);
    fn_10276100(c);
    recomp_pinned_uncounted(0);
    if (traced) {
        g3_memtrace_end("render");
        ++mt_done;
    }
    g3_phase("render", t0);
}
