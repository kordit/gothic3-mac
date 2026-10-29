/* native/g3_opt.c - the switches for Gothic 3's native replacements.
 *
 * G3OPT=<groups> in the trace control file (RECOMP_TRACE_CONTROL, read once a second) turns
 * groups on while the game runs: a comma list of rand, particles, d3d, str, mem, vec, scene, leaf, shadow, pdo, move, uv, exec, zlib, anim, createlod, ao, "all"
 * or "none", and "-name" to drop one ("all,-pdo"). Without the key the start value stays: RECOMP_G3OPT, else
 * G3OPT_DEFAULT, the one setup the player plays and every bench measures (session 19). Each group's
 * g3n_ functions run their native code only while its bit is set (a diagnosis may turn one off; the
 * player's game and the benches never do). */
#include "g3_opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile uint32_t g3_opt = 0xffffffffu; /* g3_opt_init sets the start value */

static const struct {
    const char *name;
    uint32_t bit;
} groups[] = {{"rand", G3OPT_RAND}, {"particles", G3OPT_PARTICLES}, {"d3d", G3OPT_D3D}, {"str", G3OPT_STR}, {"mem", G3OPT_MEM}, {"vec", G3OPT_VEC}, {"scene", G3OPT_SCENE}, {"leaf", G3OPT_LEAF}, {"shadow", G3OPT_SHADOW}, {"pdo", G3OPT_PDO}, {"move", G3OPT_MOVE}, {"anim", G3OPT_ANIM}, {"uv", G3OPT_UV}, {"exec", G3OPT_EXEC}, {"zlib", G3OPT_ZLIB}, {"createlod", G3OPT_CREATELOD}, {"ao", G3OPT_AO}};

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

#define G3OPT_DEFAULT 0xffffffffu
static uint32_t g3_start = G3OPT_DEFAULT;
void g3_console_control(const char *text); /* native/console.c: G3CMD= lines */
void g3_mods_control(const char *text);    /* native/mods.c: G3LOC= lines */

/* kit/runtime/cpu.cpp: every applied control text. */
extern void (*recomp_trace_game_hook)(const char *text);
static void control(const char *text) {
    const char *key = strstr(text, "G3OPT=");
    if (key && (key == text || key[-1] == '\n'))
        g3_opt = parse(key + 6, 0);
    else
        g3_opt = g3_start;
    report("trace control");
    g3_console_control(text);
    g3_mods_control(text);
}

__attribute__((constructor)) static void g3_opt_init(void) {
    const char *env = getenv("RECOMP_G3OPT");
    if (env)
        g3_start = parse(env, 0);
    g3_opt = g3_start;
    recomp_trace_game_hook = control;
    report("start");
}
