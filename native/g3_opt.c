/* native/g3_opt.c - the switches for Gothic 3's native replacements.
 *
 * G3OPT=<groups> in the trace control file (RECOMP_TRACE_CONTROL, read once a second) turns
 * groups on while the game runs: a comma list of rand, particles, occl, d3d, str, mem, audio, vec, scene, leaf, shadow, occlmove, pdo, vfs, cull, ai, move, uv, exec, zlib, anim, load, rtti, lod, "all" or "none", and
 * "-name" to drop one ("all,-occl"). Without the key the start value stays: RECOMP_G3OPT, else
 * "all". Each group's g3n_ functions run their native code only while its bit is set, so one
 * run can compare a group on and off standing in one place (FRAMES=10 logs the frame time). */
#include "g3_opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile uint32_t g3_opt = 0xffffffffu;

static const struct {
    const char *name;
    uint32_t bit;
} groups[] = {{"rand", G3OPT_RAND}, {"particles", G3OPT_PARTICLES}, {"occl", G3OPT_OCCL}, {"d3d", G3OPT_D3D}, {"str", G3OPT_STR}, {"mem", G3OPT_MEM}, {"audio", G3OPT_AUDIO}, {"vec", G3OPT_VEC}, {"scene", G3OPT_SCENE}, {"leaf", G3OPT_LEAF}, {"shadow", G3OPT_SHADOW}, {"occlmove", G3OPT_OCCLMOVE}, {"pdo", G3OPT_PDO}, {"vfs", G3OPT_VFS}, {"cull", G3OPT_CULL}, {"ai", G3OPT_AI}, {"move", G3OPT_MOVE}, {"anim", G3OPT_ANIM}, {"uv", G3OPT_UV}, {"exec", G3OPT_EXEC}, {"zlib", G3OPT_ZLIB}, {"load", G3OPT_LOAD}, {"rtti", G3OPT_RTTI}, {"lod", G3OPT_LOD}};

static uint32_t parse(const char *list, uint32_t value) {
    char word[32];
    while (*list) {
        size_t n = strcspn(list, ",\r\n");
        size_t len = n < sizeof word - 1 ? n : sizeof word - 1;
        memcpy(word, list, len);
        word[len] = 0;
        const int minus = word[0] == '-';
        const char *w = word + minus;
        uint32_t bit = 0;
        if (!strcmp(w, "all"))
            bit = 0xffffffffu;
        else if (!strcmp(w, "none"))
            bit = 0, value = 0;
        for (size_t i = 0; i < sizeof groups / sizeof groups[0]; ++i)
            if (!strcmp(w, groups[i].name))
                bit = groups[i].bit;
        value = minus ? value & ~bit : value | bit;
        list += n;
        if (*list != ',')
            break;
        ++list;
    }
    return value;
}

static void report(const char *why) {
    char line[256];
    int n = snprintf(line, sizeof line, "g3opt (%s):", why);
    for (size_t i = 0; i < sizeof groups / sizeof groups[0] && n > 0 && n < (int)sizeof line - 16; ++i)
        n += snprintf(line + n, sizeof line - n, " %s %s", groups[i].name, (g3_opt & groups[i].bit) ? "on" : "off");
    fprintf(stderr, "%s\n", line);
}

static uint32_t g3_start = 0xffffffffu;
void g3_console_control(const char *text); /* native/console.c: G3CMD= lines */
void g3_mods_control(const char *text);    /* native/mods.c: G3LOC= lines */
volatile uint32_t g3_occl_vis;

/* kit/runtime/cpu.cpp: every applied control text. */
extern void (*recomp_trace_game_hook)(const char *text);
static void control(const char *text) {
    const char *key = strstr(text, "G3OPT=");
    if (key && (key == text || key[-1] == '\n'))
        g3_opt = parse(key + 6, 0);
    else
        g3_opt = g3_start;
    const char *vis = strstr(text, "G3OCCLVIS=");
    g3_occl_vis = vis && (vis == text || vis[-1] == '\n') ? (uint32_t)strtoul(vis + 10, 0, 0) : 0u;
    report("trace control");
    g3_console_control(text);
    g3_mods_control(text);
    if (g3_occl_vis)
        fprintf(stderr, "g3opt: occlusion counts forced visible at sites %#x\n", g3_occl_vis);
}

__attribute__((constructor)) static void g3_opt_init(void) {
    const char *env = getenv("RECOMP_G3OPT");
    if (env)
        g3_start = parse(env, 0);
    g3_opt = g3_start;
    recomp_trace_game_hook = control;
    report("start");
}
