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
#include "g3_opt.h"

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

/* RECOMP_G3_RANDWATCH=<address hex>,<from ms>,<to ms> (diagnostics, pinned runs): the dword at the address (the game
 * thread's MSVCRT _holdrand, its _tiddata +0x14) before and after each DoRender whose pinned clock is in the range, so
 * two runs show whether the render drew a different number of random numbers from the logic's stream. */
static uint32_t rw_addr, rw_from, rw_to;
static void rw_note(const char *when) {
    static int armed = -1;
    if (armed < 0) {
        const char *v = getenv("RECOMP_G3_RANDWATCH");
        armed = v && sscanf(v, "%x,%u,%u", &rw_addr, &rw_from, &rw_to) == 3;
    }
    const uint32_t t = armed ? host_pinned_clock_value() : 0;
    if (armed && t >= rw_from && t <= rw_to)
        fprintf(stderr, "g3: randwatch clock %u %s render: %08x\n", t, when, rd32(rw_addr));
}

/* Group nooc (D-057): the engine's occlusion culling off, as the console's caps.EnableOcclusionCulling=0 sets it
 * (eCApplication::OnCaps 10270170: the render admin's +0x11b). With it on, the gathering (ExtractFromFrustumOC
 * 103fe590) issues occlusion queries and waits for the GPU's answers in the middle of its PVS walk; measured on the
 * player's setup (31.10.2026, work/s31/oc*.sh), culling off is faster in both scenes: the cave 91-109 fps against
 * 86-96, Ardea's fight 65-71 against 56-62, the query waits (1-2.6 ms a frame) gone, the GPU's work smaller though
 * 2-80% more is drawn. And the gathering no longer needs the GPU, which D-054's second thread needs. Applied before
 * each frame's drawing, so a save, a load or the console turning it back on is undone at the next frame; the group
 * off puts back what the engine had. The render admin is FUN_10277ac0's cache, valid once 10874bd4 bit 0 is set. */
#define OC_ADMIN 0x10874bd0u
#define OC_ADMIN_SET 0x10874bd4u
static int oc_saved = -1; /* the engine's value before the group cleared it, -1 while it has not */
static void oc_apply(void) {
    if (!(rd32(OC_ADMIN_SET) & 1u) || !rd32(OC_ADMIN))
        return;
    const uint32_t flag = rd32(OC_ADMIN) + 0x11bu;
    if (G3OPT(NOOC)) {
        if (rd8(flag)) {
            if (oc_saved < 0)
                oc_saved = rd8(flag);
            wr8(flag, 0);
        }
    } else if (oc_saved >= 0) {
        wr8(flag, (uint8_t)oc_saved);
        oc_saved = -1;
    }
}

void g3n_10276100(X86 *c) {
    const uint64_t t0 = g3_phase_now();
    const int traced = g3_memtrace_frame() && recomp_memtrace_begin();
    oc_apply();
    rw_note("before");
    /* In a pinned run the drawing's calls and clock polls are not counted (kit/runtime/kernel32.cpp,
     * recomp_pinned_uncounted): the game's time, and where the other threads run, must not follow how often the
     * render asks. */
    recomp_pinned_uncounted(1);
    fn_10276100(c);
    recomp_pinned_uncounted(0);
    rw_note("after");
    if (traced) {
        g3_memtrace_end("render");
        ++mt_done;
    }
    g3_phase("render", t0);
}
