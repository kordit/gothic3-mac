/* native/savegame.c - F5 keeps the previous quick save (Game.dll gCHUDFileManager save 109b9e90).
 *
 * The game has one quick-save slot: F5 (HandleMessage 1098d9e0, message 0x8001) saves under the name
 * "QuickSave", F9 (0x8002) loads that name, so every F5 overwrote the last one. The player asked for
 * F5 to add a save instead (session 12). Renaming the save that F9 loads would break F9, so before a
 * save named "QuickSave" the existing pair (QuickSave.g3savcpx and .g3savcpxdat in the profile's
 * Documents/gothic3) moves to the next free slot name the save menu uses, <world>_NNNN, and the save
 * then goes on as before. F9 still loads the newest; the older ones are in the load menu. The prefix
 * is the one the existing slots carry (G3_World_01 when there are none). G3_QUICKSAVE_KEEP=0 turns it
 * off.
 *
 * 109b9e90 is stdcall (file manager [ESP+4], bCString *name [ESP+8], its text at +0), RET 8; the
 * wrapper only looks at the name and then runs the original with the registers as they came; when the
 * save fails (AL = 0) and no quick save is left, the kept pair moves back. */
#include "chunk.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void fn_109b9e90(X86 *c); /* gCHUDFileManager: save the session under a name */
const char *mods_overlay_profile_dir(void);

static int sg_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static char sg_from[2][1100], sg_to[2][1400]; /* the last move, to undo if the save fails */
static int sg_moved;

static void sg_keep_quicksave(void) {
    sg_moved = 0;
    const char *profile = mods_overlay_profile_dir();
    if (!profile || !*profile)
        return;
    char dir[1024], qs[1100], qsd[1100];
    snprintf(dir, sizeof dir, "%s/Documents/gothic3", profile);
    snprintf(qs, sizeof qs, "%s/QuickSave.g3savcpx", dir);
    snprintf(qsd, sizeof qsd, "%s/QuickSave.g3savcpxdat", dir);
    if (!sg_exists(qs))
        return;
    /* The highest <prefix>_NNNN.g3savcpx in the folder, and its prefix. */
    char prefix[256] = "G3_World_01";
    int top = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != 0) {
            const char *n = e->d_name;
            const size_t len = strlen(n), ext = strlen(".g3savcpx");
            if (len < ext + 6 || strcmp(n + len - ext, ".g3savcpx"))
                continue;
            const char *num = n + len - ext - 4;
            if (num[-1] != '_' || num - 1 - n >= (long)sizeof prefix)
                continue;
            int v = 0, ok = 1;
            for (int i = 0; i < 4; ++i)
                ok &= num[i] >= '0' && num[i] <= '9', v = v * 10 + (num[i] - '0');
            if (!ok)
                continue;
            if (v > top) {
                top = v;
                memcpy(prefix, n, (size_t)(num - 1 - n));
                prefix[num - 1 - n] = 0;
            }
        }
        closedir(d);
    }
    for (int n = top + 1; n < 10000; ++n) {
        char to[1400], tod[1400];
        snprintf(to, sizeof to, "%s/%s_%04d.g3savcpx", dir, prefix, n);
        snprintf(tod, sizeof tod, "%s/%s_%04d.g3savcpxdat", dir, prefix, n);
        if (sg_exists(to) || sg_exists(tod))
            continue;
        if (sg_exists(qsd) && rename(qsd, tod) != 0) {
            fprintf(stderr, "[g3] quicksave: could not keep the previous one (%s)\n", tod);
            return;
        }
        if (rename(qs, to) != 0) {
            if (sg_exists(tod))
                rename(tod, qsd); /* put the pair back together */
            fprintf(stderr, "[g3] quicksave: could not keep the previous one (%s)\n", to);
            return;
        }
        snprintf(sg_from[0], sizeof sg_from[0], "%s", qs);
        snprintf(sg_to[0], sizeof sg_to[0], "%s", to);
        snprintf(sg_from[1], sizeof sg_from[1], "%s", qsd);
        snprintf(sg_to[1], sizeof sg_to[1], "%s", tod);
        sg_moved = 1;
        fprintf(stderr, "[g3] quicksave: the previous quick save is now %s_%04d\n", prefix, n);
        return;
    }
}

void g3n_109b9e90(X86 *c) {
    const uint32_t name = rd32(c->r[R_ESP] + 8u);
    const uint32_t text = name ? rd32(name) : 0u;
    static const char want[] = "QuickSave";
    int quick = text != 0u;
    for (uint32_t i = 0; quick && i < sizeof want; ++i)
        quick = rd8(text + i) == (uint8_t)want[i];
    const char *env = getenv("G3_QUICKSAVE_KEEP");
    const int keep = quick && !(env && !strcmp(env, "0"));
    if (keep)
        sg_keep_quicksave();
    fn_109b9e90(c);
    /* A failed save (AL = 0) that left no quick save: the kept one goes back, so F9 still loads it. */
    if (keep && sg_moved && (c->r[R_EAX] & 0xffu) == 0u && !sg_exists(sg_from[0])) {
        for (int i = 0; i < 2; ++i)
            if (sg_exists(sg_to[i]))
                rename(sg_to[i], sg_from[i]);
        fprintf(stderr, "[g3] quicksave: the save failed, the previous quick save is back\n");
    }
    sg_moved = 0;
}
