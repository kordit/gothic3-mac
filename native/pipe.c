/* native/pipe.c - stage 5 (docs/perf/wielordzeniowosc.md, "Etap 5, plan wykonania"): the logic and the render of a
 * frame at once, RECOMP_G3_PIPE=1 (off by default).
 *
 * eCApplication::OnRun (10276720) runs Process (10276490, the logic: entities, modules, PhysX) and then DoRender
 * (10276100: camera, render system, shadows, GUI, present). With the mode on, Process called from OnRun (it returns
 * to 1027677c) is handed to a guest thread of its own and returns at once; OnRun goes on to DoRender, which the main
 * thread runs without the baton (recomp_host_work_begin: it becomes a free thread, its shims go through the kit's
 * free_dispatch) while the logic thread runs Process with it. After DoRender the main thread waits for the logic to
 * end and takes the baton back, so nothing else of the frame (the message pump, a load) runs beside the logic.
 *
 * The logic thread is made by the guest's CreateThread on the first frame, with Process as its start and PIPE_MAGIC as
 * the parameter: g3n_10276490 (mods.c) sees the parameter and spends the thread's life in pipe_logic_loop instead.
 * The render reads what the logic of the same frame is writing; that is the mode's risk (no snapshot yet).
 *
 * What the two threads share:
 * - Direct3D 9: its shims run under a recursive lock of the kit's instead of the baton (recomp_d3d_shared).
 * - MemHeap (SharedBase) has its own critical section. A block the logic frees while the render runs could still be
 *   read by the render, so MemHeap::Free (10020090) and FreeShortUsage (10020470) from the logic thread are queued
 *   meanwhile and run after the join, by the main thread, through the translated originals with the argument and a
 *   return address pushed (guest_call would set up a longjmp record each time).
 * - The kit's heap has a lock (memory.cpp), rtti.c's table a sequence count, the frame phases a lock. */
#include "chunk.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");
uint32_t imports_resolve(const char *dll, const char *name) __asm__("__Z15imports_resolvePKcS0_");
bool recomp_host_work_begin(void);
void recomp_host_work_end(void);
void recomp_d3d_shared(bool on);
bool recomp_exit_pending(void);
void fn_10020090(X86 *c); /* SharedBase MemHeap::Free(void *), stdcall, RET 4 */
void fn_10020470(X86 *c); /* SharedBase MemHeap::FreeShortUsage(void *), stdcall, RET 4 */
void g3_process_body(X86 *c); /* mods.c: Process as it runs without the mode */

#define PIPE_MAGIC 0x50495045u     /* "PIPE": the logic thread's start parameter */
#define PIPE_PROCESS_RET 0x1027677cu /* OnRun, after CALL Process */
#define PIPE_STACK 0x100000u

static int pipe_mode = -1;
static pthread_mutex_t pipe_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pipe_cv = PTHREAD_COND_INITIALIZER;
static int pipe_started;          /* main thread only */
static int pipe_ready;            /* the logic thread waits for work */
static int pipe_job, pipe_done = 1;
static uint32_t pipe_app;
static _Thread_local int pipe_on_logic;
static atomic_int pipe_rendering; /* DoRender runs beside a logic job */
static uint64_t pipe_frames, pipe_wait_ns, pipe_deferred;

/* Frees from the logic thread while the render runs. */
struct pipe_free {
    uint32_t ptr, fn; /* fn: 0 MemHeap::Free, 1 FreeShortUsage */
};
static struct pipe_free *pipe_q;
static size_t pipe_qn, pipe_qcap;

static uint64_t pipe_now(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

int g3_pipe_on(void) {
    if (pipe_mode < 0) {
        const char *e = getenv("RECOMP_G3_PIPE");
        pipe_mode = e && *e == '1';
        if (pipe_mode)
            fprintf(stderr, "[g3] pipe: the logic on a thread of its own beside the render (RECOMP_G3_PIPE=1)\n");
    }
    return pipe_mode;
}

/* The logic thread's life: wait for a job without the baton, run Process with it, say it is done. */
static void pipe_logic_loop(X86 *c) {
    pipe_on_logic = 1;
    fprintf(stderr, "[g3] pipe: logic thread up\n");
    pthread_mutex_lock(&pipe_m);
    pipe_ready = 1;
    pthread_cond_broadcast(&pipe_cv);
    pthread_mutex_unlock(&pipe_m);
    for (;;) {
        const bool lent = recomp_host_work_begin();
        pthread_mutex_lock(&pipe_m);
        while (!pipe_job && !recomp_exit_pending()) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&pipe_cv, &pipe_m, &ts);
        }
        const int job = pipe_job;
        pipe_job = 0;
        const uint32_t app = pipe_app;
        pthread_mutex_unlock(&pipe_m);
        if (lent)
            recomp_host_work_end();
        if (!job)
            break; /* the game is ending */
        const uint32_t esp = c->r[R_ESP];
        c->r[R_ESP] = esp - 4u;
        wr32(esp - 4u, PIPE_PROCESS_RET);
        c->r[R_ECX] = app;
        g3_process_body(c); /* its RET pops the return address */
        c->r[R_ESP] = esp;
        pthread_mutex_lock(&pipe_m);
        pipe_done = 1;
        pthread_cond_broadcast(&pipe_cv);
        pthread_mutex_unlock(&pipe_m);
    }
    fprintf(stderr, "[g3] pipe: logic thread ends\n");
}

/* The first frame with the mode on: CreateThread(NULL, PIPE_STACK, Process, PIPE_MAGIC, 0, NULL) from the main thread,
 * under the baton, and a wait until the thread waits for work. 0 when it could not be made. */
static int pipe_start(X86 *c) {
    const uint32_t create = imports_resolve("KERNEL32.dll", "CreateThread");
    if (!create) {
        fprintf(stderr, "[g3] pipe: no CreateThread, the mode stays off\n");
        return 0;
    }
    const uint32_t args[6] = {0u, PIPE_STACK, 0x10276490u, PIPE_MAGIC, 0u, 0u};
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    const uint32_t h = guest_call(c, create, args, 6);
    memcpy(c->r, saved, sizeof saved);
    if (!h) {
        fprintf(stderr, "[g3] pipe: CreateThread failed, the mode stays off\n");
        return 0;
    }
    /* The new thread needs the baton to start: lend it until the thread says it waits. */
    const bool lent = recomp_host_work_begin();
    pthread_mutex_lock(&pipe_m);
    while (!pipe_ready)
        pthread_cond_wait(&pipe_cv, &pipe_m);
    pthread_mutex_unlock(&pipe_m);
    if (lent)
        recomp_host_work_end();
    recomp_d3d_shared(true);
    return 1;
}

/* g3n_10276490's first look (mods.c): 1 when the call was taken care of here. The logic thread's start never returns
 * from Process the usual way: it returns from the thread's start routine (stdcall, one argument). */
int g3_pipe_process(X86 *c) {
    if (!g3_pipe_on())
        return 0;
    const uint32_t esp = c->r[R_ESP];
    if (!pipe_on_logic && rd32(esp + 4u) == PIPE_MAGIC && pipe_started == 1 && !pipe_ready) {
        pipe_logic_loop(c);
        c->r[R_EAX] = 0u;
        c->eip = rd32(esp);
        c->r[R_ESP] = esp + 8u;
        recomp_return(c);
        return 1;
    }
    if (pipe_on_logic || rd32(esp) != PIPE_PROCESS_RET)
        return 0;
    if (!pipe_started) {
        pipe_started = 1;
        if (!pipe_start(c)) {
            pipe_started = 2;
            pipe_mode = 0;
            return 0;
        }
    }
    pthread_mutex_lock(&pipe_m);
    pipe_app = c->r[R_ECX];
    pipe_done = 0;
    pipe_job = 1;
    pthread_cond_broadcast(&pipe_cv);
    pthread_mutex_unlock(&pipe_m);
    atomic_store(&pipe_rendering, 1);
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4u;
    recomp_return(c);
    return 1;
}

static void pipe_run_free(X86 *c, uint32_t fn, uint32_t ptr) {
    const uint32_t esp = c->r[R_ESP], sp = esp - 8u;
    wr32(sp, PIPE_PROCESS_RET);
    wr32(sp + 4u, ptr);
    c->r[R_ESP] = sp;
    if (fn)
        fn_10020470(c);
    else
        fn_10020090(c);
    c->r[R_ESP] = esp;
}

/* g3n_10276100 around DoRender: 1 from pipe_render_begin when the render runs beside a logic job; then
 * pipe_render_end after it. */
int g3_pipe_render_begin(void) {
    if (!g3_pipe_on() || !atomic_load(&pipe_rendering))
        return 0;
    if (!recomp_host_work_begin()) {
        /* The logic job is out and only the baton's lending lets it run: without it both would wait forever. */
        fprintf(stderr, "[g3] pipe: the main thread could not lend the baton for the render; stopping\n");
        abort();
    }
    return 1;
}

void g3_pipe_render_end(X86 *c) {
    const uint64_t t0 = pipe_now();
    pthread_mutex_lock(&pipe_m);
    while (!pipe_done)
        pthread_cond_wait(&pipe_cv, &pipe_m);
    pthread_mutex_unlock(&pipe_m);
    atomic_store(&pipe_rendering, 0);
    recomp_host_work_end();
    pipe_wait_ns += pipe_now() - t0;
    /* The logic's frees from while the render ran, now that nothing reads them. */
    const uint32_t saved[8] = {c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]};
    for (size_t i = 0; i < pipe_qn; ++i)
        pipe_run_free(c, pipe_q[i].fn, pipe_q[i].ptr);
    pipe_deferred += pipe_qn;
    pipe_qn = 0;
    memcpy(c->r, saved, sizeof saved);
    if (++pipe_frames % 600u == 0u)
        fprintf(stderr, "[g3] pipe: %llu frames, the render waited %.2f ms a frame for the logic, %llu frees deferred\n",
                (unsigned long long)pipe_frames, pipe_wait_ns / 1e6 / 600.0, (unsigned long long)pipe_deferred);
    if (pipe_frames % 600u == 0u)
        pipe_wait_ns = 0;
}

/* MemHeap::Free and FreeShortUsage: from the logic thread while the render runs, queued (pipe_render_end). */
static int pipe_defer(X86 *c, uint32_t fn) {
    if (!pipe_on_logic || !atomic_load(&pipe_rendering))
        return 0;
    const uint32_t esp = c->r[R_ESP], ptr = rd32(esp + 4u);
    if (ptr) {
        pthread_mutex_lock(&pipe_m);
        if (pipe_qn == pipe_qcap) {
            pipe_qcap = pipe_qcap ? pipe_qcap * 2u : 1024u;
            pipe_q = realloc(pipe_q, pipe_qcap * sizeof *pipe_q);
        }
        pipe_q[pipe_qn].ptr = ptr;
        pipe_q[pipe_qn].fn = fn;
        ++pipe_qn;
        pthread_mutex_unlock(&pipe_m);
    }
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 8u;
    recomp_return(c);
    return 1;
}

void g3n_10020090(X86 *c) {
    if (!pipe_defer(c, 0u))
        fn_10020090(c);
}

void g3n_10020470(X86 *c) {
    if (!pipe_defer(c, 1u))
        fn_10020470(c);
}
