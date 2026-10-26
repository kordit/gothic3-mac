// Remaster A4 design input (TEST ONLY): exact wall-clock totals for the game thread's draw and effect sub-phases,
// because sampled profiles ranked them wrongly. RECOMP_REMASTER_PROF=1 turns it on; unset, each scope costs one
// predictable branch. Game thread only (no atomics). frame() prints per-frame averages every 600 frames and resets.
#pragma once

#include "os.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace rprof {
enum Slot : int {
    DRAW,         // gpu_draw total (shim side of one draw, including the queue)
    DRAW_PROG,    // target, viewport, program keys and lookups
    DRAW_STREAMS, // vertex buffer mirrors (gpu_sync_buffer)
    DRAW_SAMPLERS,// 16-stage texture mirror loop (gpu_sync)
    QUEUE,        // host_d9_draw: command copy, predecode, payload and state copies, hand-over
    PASS,         // apply_pass total
    PASS_STATES,  // pass states (apply_state, preshaders)
    PASS_TEXTURES,// pass texture bindings (name lookups)
    PASS_CONSTS,  // bind_constants (vertex + pixel)
    COUNT
};
inline const char *const names[COUNT] = {"draw", "draw.prog", "draw.streams", "draw.samplers", "queue",
                                          "pass", "pass.states", "pass.textures", "pass.consts"};
inline bool on() {
    static const bool v = [] {
        const char *e = getenv("RECOMP_REMASTER_PROF");
        return e && *e == '1';
    }();
    return v;
}
inline uint64_t acc[COUNT], cnt[COUNT];

// Remaster P1 (TEST ONLY, prop-pools measurement, review/2026-10-07-props-measurement-design.md): per-frame
// attribution of effect-pass work by kind. A pool pass is the pass named "clean_pools" in an effect with a
// CleanPoolCount parameter. A restore is a BeginPass(0) on the effect whose pool pass was the last pass applied.
// Kinds are exclusive. With RECOMP_REMASTER_PROF_CSV=<path> one row per presented frame is written, in batches.
enum Kind : int { K_BP, K_CC, K_RS, K_PBP, K_PCC, K_N };
inline const char *const kind_names[K_N] = {"bp", "cc", "restore", "pool_bp", "pool_cc"};
struct FrameRec {
    uint64_t frame_id, t_ns, cpu_ns;
    uint32_t n[K_N], full_binds[K_N], inc_binds[K_N], states[K_N];
    uint64_t ns[K_N], ns_states[K_N], ns_tex[K_N], ns_consts[K_N];
    uint32_t draws, pool_draws, fx_switches;
    uint64_t draw_ns, pool_draw_ns;
};
inline FrameRec cur;
inline int kind = K_BP;            // kind of the apply_pass in progress (game thread only)
inline bool pool_active = false;   // the pool pass is the pass in effect (from its apply until EndPass/another apply)
inline uint32_t last_pool_fx = 0;  // effect (self id) whose pool pass was the last pass applied
inline uint32_t last_fx = 0;       // effect of the last BeginPass (effect switches)
inline uint64_t frame_id_next = 0; // host frame id of the present in progress (set by Dev_Present)
inline uint64_t klap(Slot s, uint64_t t0) {   // lap() that also charges the current kind's sub-phase
    if (!t0)
        return 0;
    uint64_t n = os_monotonic_ns(), d = n - t0;
    acc[s] += d;
    ++cnt[s];
    if (s == PASS_STATES) cur.ns_states[kind] += d;
    else if (s == PASS_TEXTURES) cur.ns_tex[kind] += d;
    else if (s == PASS_CONSTS) cur.ns_consts[kind] += d;
    return n;
}
inline const char *csv_path() {
    static const char *p = [] { const char *e = getenv("RECOMP_REMASTER_PROF_CSV"); return e && *e ? e : nullptr; }();
    return p;
}
inline void csv_flush(const FrameRec *r, size_t n) {
    static bool header = false;
    FILE *f = std::fopen(csv_path(), "a");
    if (!f)
        return;
    if (!header) {
        header = true;
        std::fprintf(f, "frame_id,t_s,cpu_ms,draws,draw_ms,pool_draws,pool_draw_ms,fx_switches");
        for (int k = 0; k < K_N; ++k)
            std::fprintf(f, ",%s_n,%s_ms,%s_states_ms,%s_tex_ms,%s_consts_ms,%s_full,%s_inc,%s_states", kind_names[k],
                         kind_names[k], kind_names[k], kind_names[k], kind_names[k], kind_names[k], kind_names[k],
                         kind_names[k]);
        std::fprintf(f, "\n");
    }
    for (size_t i = 0; i < n; ++i) {
        const FrameRec &x = r[i];
        std::fprintf(f, "%llu,%.6f,%.4f,%u,%.4f,%u,%.4f,%u", (unsigned long long)x.frame_id, x.t_ns / 1e9,
                     x.cpu_ns / 1e6, x.draws, x.draw_ns / 1e6, x.pool_draws, x.pool_draw_ns / 1e6, x.fx_switches);
        for (int k = 0; k < K_N; ++k)
            std::fprintf(f, ",%u,%.4f,%.4f,%.4f,%.4f,%u,%u,%u", x.n[k], x.ns[k] / 1e6, x.ns_states[k] / 1e6,
                         x.ns_tex[k] / 1e6, x.ns_consts[k] / 1e6, x.full_binds[k], x.inc_binds[k], x.states[k]);
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

struct Scope {
    Slot s;
    uint64_t t0 = 0;
    explicit Scope(Slot slot) : s(slot) {
        if (on())
            t0 = os_monotonic_ns();
    }
    ~Scope() {
        if (t0) {
            const uint64_t d = os_monotonic_ns() - t0;
            acc[s] += d;
            ++cnt[s];
            if (s == DRAW) {   // P1: shim side of one draw (inclusive of its queue hand-over)
                ++cur.draws;
                cur.draw_ns += d;
                if (pool_active) {
                    ++cur.pool_draws;
                    cur.pool_draw_ns += d;
                }
            }
        }
    }
};
// Lap timing inside one function: t0 = start(); ...; t0 = lap(SLOT, t0); (both are no-ops when off).
inline uint64_t start() {
    return on() ? os_monotonic_ns() : 0;
}
inline uint64_t lap(Slot s, uint64_t t0) {
    if (!t0)
        return 0;
    uint64_t n = os_monotonic_ns();
    acc[s] += n - t0;
    ++cnt[s];
    return n;
}
inline void frame() {
    static uint64_t frames = 0;
    if (!on())
        return;
    if (csv_path()) {   // P1: one row per presented frame, written 600 at a time
        static FrameRec ring[600];
        static size_t used = 0;
        static uint64_t cpu_prev = 0;
        const uint64_t cpu = os_thread_cpu_ns();
        cur.frame_id = frame_id_next;
        cur.t_ns = os_monotonic_ns();
        cur.cpu_ns = cpu_prev && cpu >= cpu_prev ? cpu - cpu_prev : 0;
        cpu_prev = cpu;
        ring[used++] = cur;
        if (used == 600) {
            csv_flush(ring, used);
            used = 0;
        }
    }
    cur = FrameRec{};
    if (++frames % 600)
        return;
    std::fprintf(stderr, "[remaster] prof: frame %llu per-frame ms:", (unsigned long long)frames);
    for (int i = 0; i < COUNT; ++i)
        std::fprintf(stderr, " %s %.3f (%.0f)", names[i], acc[i] / 1e6 / 600.0, cnt[i] / 600.0);
    std::fprintf(stderr, "\n");
    for (int i = 0; i < COUNT; ++i)
        acc[i] = cnt[i] = 0;
}
} // namespace rprof
