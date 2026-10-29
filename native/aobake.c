/* native/aobake.c - the offline tool behind native/aocache.c: every SpeedTree's ambient occlusion computed once, by
 * the game's own code, without the game (D-052, docs/perf/ao-drzew.md, tools/g3ao.sh).
 *
 * With RECOMP_AO_BAKE=<folder> the game binary takes the process over once its modules have attached (the kit's
 * recomp_entry_takeover, kit/runtime/native_seam.h): the executable's entry never runs, so there is no window, no
 * world, no profile and no save. <folder>/list.txt names one tree file a line (a raw .spt, unpacked by tools/g3ao.sh
 * from the player's archives into <folder>/spt/). For each one this does what the game does from the tree's file to
 * its three eCAOMeshBase::Compute calls, with the same functions in the same order and the same arguments, and
 * without what has no bearing on them (the vertex buffers, the materials, the render buffer object):
 *
 *   eCWraper_SpeedTreeRT::Create 1046aa80   the CSpeedTreeRT (operator new 0x9c, constructor 10515020)
 *   eCWraper_SpeedTreeRT::Read 1046b200     LoadTree 105160c0 on the whole file, the scaling matrix, the eight setters
 *                                           with their register arguments, Compute 105159b0, then the two writes after
 *   eCSpeedTreeRenderBuffer::CreateGemoetry 10463220   GetGeometry 10518d90 (4, -1, -1, 0), the group: the leaves
 *                                           as spheres (eCAOSpeedTreeLeafMesh, +0x34 1046a310 with 1, 1) and the
 *                                           ground (eCAOCustomMesh, 25 disks, Create 10359070)
 *   CreateBranchGeometry 10463590           GetGeometry (1, 0, -1, -1), its loop over the branch LODs, the
 *                                           eCAOPerTriangleMesh16 (Create 102ae470) and its Compute with the group
 *   CreateFrondGeometry 10464260            the same with (2, -1, 0, -1) and the frond LODs
 *   CreateLeafGeometry 10464ee0             GetGeometry (4, -1, -1, 0), its loop over the leaf LODs, once more
 *                                           (4, -1, -1, 0), the leaf mesh (+0x34 with 0, 0) and its Compute with itself
 *
 * Each Compute goes through the dispatch to g3n_10357ba0 with g3n_aocache_store set: the original computes and the
 * result goes to ~/Library/Caches/Gothic3Recomp/ao/ under the key of what it read. A result the game later asks for
 * by the same key is this one; anything the tool did not reproduce exactly has another key, so the game computes as
 * before and says so in game.log. The x87 control word is the game's at a tree's load, 0x027f (the executable's CRT
 * sets PC = 53 in __setdefaultprecision 00415d67; the kit's d3d9 leaves it), and is part of the key.
 *
 * One tree is one part: <folder>/done/<name>.done after its three results are on disk, <name>.started before it begins
 * (a tree that crashed the tool is not tried again on a restart). Progress goes to RECOMP_AO_BAKE_LOG. */
#include "chunk.h"
#include "g3_opt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");
uint32_t heap_alloc(uint32_t size, _Bool zero, uint32_t align) __asm__("__Z10heap_allocjbj");
_Bool heap_free(uint32_t addr) __asm__("__Z9heap_freej");
extern int g3n_aocache_store;
extern uint32_t g3n_aocache_last_disks;
extern unsigned g3n_aocache_unknown;
void g3n_10357ba0(X86 *c);

#define VT_GROUP 0x1068ff58u
#define VT_TRIANGLES 0x1069013cu
#define VT_CUSTOM 0x106901acu
#define VT_LEAVES 0x106d0c74u

static FILE *g_log;
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    const time_t t = time(NULL);
    char when[16];
    strftime(when, sizeof when, "%H:%M:%S", localtime(&t));
    fprintf(stderr, "[aobake] %s\n", line);
    if (g_log) {
        fprintf(g_log, "%s %s\n", when, line);
        fflush(g_log);
    }
}

static uint32_t call(X86 *c, uint32_t fn, uint32_t ecx, uint32_t edx, uint32_t eax, const uint32_t *args, int n) {
    c->r[0] = eax;
    c->r[1] = ecx;
    c->r[2] = edx;
    return guest_call(c, fn, args, n);
}
#define ARGS(...) (const uint32_t[]){__VA_ARGS__}, (int)(sizeof((const uint32_t[]){__VA_ARGS__}) / 4)
#define NOARGS NULL, 0

static uint32_t zalloc(uint32_t n) {
    return heap_alloc(n, 1, 16);
}
static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* SGeometry: 10514d50 (EAX) clears it, GetGeometry 10518d90 (ECX the struct, EDX the tree, four arguments) fills it. */
static uint32_t geometry(X86 *c) {
    const uint32_t g = zalloc(0x140u);
    call(c, 0x10514d50u, 0, 0, g, NOARGS);
    return g;
}
static void get(X86 *c, uint32_t g, uint32_t tree, uint32_t a, uint32_t b, uint32_t cc, uint32_t d) {
    call(c, 0x10518d90u, g, tree, 0, ARGS(a, b, cc, d));
}

static void group_add(uint32_t group, uint32_t mesh) {
    const uint32_t n = rd32(group + 0x8u);
    wr32(rd32(group + 0x4u) + 4u * n, mesh);
    wr32(group + 0x8u, n + 1u);
}

/* The receiving mesh's Compute (its vtable +4 is 10357ba0, which the dispatch sends to aocache.c), called from C
 * with the return address of CreateBranchGeometry's call: through guest_call the original's RET would reach the
 * guest_call sentinel and leave g3n_10357ba0 by longjmp before it stores the result. */
static void compute(X86 *c, uint32_t mesh, uint32_t emitters, const char *what, const char *name) {
    const unsigned unknown = g3n_aocache_unknown;
    if (rd32(rd32(mesh) + 4u) != 0x10357ba0u) {
        say("  %s: %s, a vtable without eCAOMeshBase::Compute, not stored", name, what);
        return;
    }
    const uint32_t esp = c->r[4];
    wr32(esp - 4u, emitters);
    wr32(esp - 8u, 0x1046386fu);
    c->r[4] = esp - 8u;
    c->r[1] = mesh;
    g3n_10357ba0(c);
    c->r[4] = esp;
    say("  %s: %s, %u disks%s", name, what, g3n_aocache_last_disks,
        g3n_aocache_unknown != unknown ? ", NOT STORED (a structure aocache.c does not know)" : "");
}

static uint32_t triangles(X86 *c, uint32_t idx, uint32_t nidx, uint32_t a, uint32_t b, uint32_t nverts) {
    const uint32_t m = zalloc(0x330u);
    call(c, 0x102ad920u, m, 0, 0, NOARGS);
    wr32(m, VT_TRIANGLES);
    call(c, 0x102ae470u, m, 0, 0, ARGS(idx, nidx, a, b, nverts, 0, 1));
    return m;
}

static uint32_t leaves(X86 *c, uint32_t tree, uint32_t g, uint32_t spheres) {
    const uint32_t m = zalloc(0x2b0u);
    call(c, 0x10358fd0u, m, 0, 0, NOARGS);
    wr32(m, VT_LEAVES);
    const uint32_t size = rd32(call(c, 0x10519fb0u, 0, 0, 0, ARGS(tree)));
    call(c, 0x1046a310u, m, 0, 0, ARGS(g + 0x78u, size, spheres, spheres));
    return m;
}

static int bake(X86 *c, const uint8_t *file, uint32_t size, const char *name) {
    c->fpu_cw = 0x027f;
    c->fpu_sw &= (uint16_t)~0x3fu;
    /* eCWraper_SpeedTreeRT::Create */
    const uint32_t tree = call(c, 0x10564654u, 0, 0, 0, ARGS(0x9cu));
    if (!tree)
        return 0;
    call(c, 0x10515020u, 0, 0, 0, ARGS(tree));
    wr8(0x10862e10u, 1);
    /* eCWraper_SpeedTreeRT::Read */
    const uint32_t buf = heap_alloc(size ? size : 1u, 0, 16);
    memcpy(g_mem + buf, file, size);
    const uint32_t loaded = call(c, 0x105160c0u, buf, 0, 0, ARGS(tree, size)) & 0xffu;
    heap_free(buf);
    if (!loaded) {
        say("  %s: LoadTree refused the file", name);
        return 0;
    }
    const uint32_t m = zalloc(0x40u);
    call(c, rd32(0x105f6c84u), m, 0, 0, ARGS(rd32(0x1068677cu))); /* bCMatrix::SetToScaling */
    call(c, 0x10517340u, 0, 0, 0, NOARGS);
    call(c, 0x10516830u, 0, 0, 0, ARGS(tree));
    call(c, 0x10516960u, 0, 0, 0, ARGS(tree));
    call(c, 0x10516a90u, 0, 0, 0, ARGS(tree));
    call(c, 0x105170e0u, 0, 0, 0, ARGS(tree));
    call(c, 0x10516fc0u, 0, 0, 0, ARGS(tree));
    call(c, 0x10517210u, 0, 0, 0, ARGS(tree));
    call(c, 0x10516e90u, 0, 0, 0, ARGS(tree));
    call(c, 0x105159b0u, tree, 0, m, ARGS(tree, m));
    wr8(rd32(tree + 0x10u) + 0x14u, 1);
    {
        /* (top - bottom) / [106e8468] scales the two floats at [tree]+0x40 and +0x44: products of two floats, exact
         * in double, so one rounding to float each, as FMUL under PC = 53 and FSTP float give */
        const uint32_t box = rd32(tree + 0x40u), t = rd32(tree);
        const float h = (float)((double)rdf32(box + 0x10u) - (double)rdf32(box + 0x4u));
        const float q = (float)((double)h / rdf64(0x106e8468u));
        const float a = rdf32(t + 0x40u), b = rdf32(t + 0x44u);
        wrf32(t + 0x44u, (float)((double)q * (double)b));
        wrf32(t + 0x40u, (float)((double)a * (double)q));
    }
    /* CreateGemoetry: the emitters */
    const uint32_t ga = geometry(c);
    get(c, ga, tree, 4, 0xffffffffu, 0xffffffffu, 0);
    const uint32_t group = zalloc(0x14u);
    wr32(group, VT_GROUP);
    wr32(group + 0x4u, zalloc(0x10u));
    wr32(group + 0xcu, 4);
    wr8(group + 0x10u, 1);
    group_add(group, leaves(c, tree, ga, 1));
    {
        const double k = rdf64(0x106e8638u);
        const float r = (float)(rdf64(0x106e8b90u) / (double)rdf32(rd32(0x105f63fcu)));
        const uint32_t disks = zalloc(25u * 0x20u);
        uint32_t at = disks;
        for (int z = -2; z <= 2; ++z) {
            const float zf = (float)((double)z * k);
            for (int x = -2; x <= 2; ++x, at += 0x20u) {
                wrf32(at, (float)((double)x * k));
                wrf32(at + 0x4u, 0.0f);
                wrf32(at + 0x8u, zf);
                wrf32(at + 0xcu, 0.0f);
                wrf32(at + 0x10u, 1.0f);
                wrf32(at + 0x14u, 0.0f);
                wrf32(at + 0x18u, r);
            }
        }
        const uint32_t ground = zalloc(0x2acu);
        wr32(ground, VT_CUSTOM);
        for (uint32_t i = 0; i < 6u; ++i)
            call(c, 0x10359490u, ground + 0x10u + 0x6cu * i, 0, 0, NOARGS);
        call(c, rd32(0x105f6714u), ground + 0x29cu, 0, 0, NOARGS); /* bCSphere::bCSphere */
        /* the game passes a copy of the array by value: data, count, capacity */
        call(c, 0x10359070u, ground, 0, 0, ARGS(0, disks, 25, 25));
        group_add(group, ground);
    }
    /* CreateBranchGeometry */
    const uint32_t gb = geometry(c);
    get(c, gb, tree, 1, 0, 0xffffffffu, 0xffffffffu);
    if (rd16(gb + 0x10u)) {
        const uint32_t lods = rd16(rd32(tree) + 0x70u), g2 = geometry(c);
        for (uint32_t i = 0; i < lods; ++i)
            get(c, g2, tree, 1, i, 0xffffffffu, 0xffffffffu);
        compute(c, triangles(c, rd32(rd32(gb + 0xcu)), rd16(rd32(gb + 0x8u)), rd32(gb + 0x24u), rd32(gb + 0x18u),
                             rd16(gb + 0x10u)),
                group, "branches", name);
    }
    /* CreateFrondGeometry */
    const uint32_t gf = geometry(c);
    get(c, gf, tree, 2, 0xffffffffu, 0, 0xffffffffu);
    if (rd16(gf + 0x4cu)) {
        const uint32_t lods = rd16(tree + 0x60u), g2 = geometry(c);
        for (uint32_t i = 0; i < lods; ++i)
            get(c, g2, tree, 2, 0xffffffffu, i, 0xffffffffu);
        compute(c, triangles(c, rd32(rd32(gf + 0x48u)), rd16(rd32(gf + 0x44u)), rd32(gf + 0x60u), rd32(gf + 0x54u),
                             rd16(gf + 0x4cu)),
                group, "fronds", name);
    }
    /* CreateLeafGeometry */
    const uint32_t gl = geometry(c);
    get(c, gl, tree, 4, 0xffffffffu, 0xffffffffu, 0);
    if ((uint16_t)(rd32(gl + 0x84u) * 4u)) {
        const uint32_t lods = rd16(rd32(tree) + 0xb4u), g2 = geometry(c);
        for (uint32_t i = 0; i < lods; ++i)
            get(c, g2, tree, 4, 0xffffffffu, 0xffffffffu, i);
        get(c, g2, tree, 4, 0xffffffffu, 0xffffffffu, 0);
        const uint32_t self = leaves(c, tree, g2, 0);
        compute(c, self, self, "leaves", name);
    }
    return 1;
}

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}
static void touch(const char *p) {
    FILE *f = fopen(p, "w");
    if (f)
        fclose(f);
}

int g3n_fmodcheck(X86 *c);

int recomp_entry_takeover(X86 *c) {
    const char *check = getenv("RECOMP_FMOD_CHECK"); /* native/fmodcheck.c, the other tool of this binary */
    if (check && *check)
        return g3n_fmodcheck(c);
    const char *dir = getenv("RECOMP_AO_BAKE");
    if (!dir || !*dir)
        return 0;
    const char *log = getenv("RECOMP_AO_BAKE_LOG");
    if (log && *log)
        g_log = fopen(log, "a");
    char path[2048], name[1024];
    snprintf(path, sizeof path, "%s/list.txt", dir);
    FILE *list = fopen(path, "r");
    if (!list) {
        say("no %s", path);
        return 1;
    }
    snprintf(path, sizeof path, "%s/done", dir);
    mkdir(path, 0755);
    unsigned total = 0, done = 0, skipped = 0, failed = 0;
    while (fgets(name, sizeof name, list))
        total += name[0] != '\n';
    rewind(list);
    g3_opt |= G3OPT_AOCACHE | G3OPT_AO;
    g3n_aocache_store = 1;
    say("start: %u trees from %s", total, dir);
    unsigned n = 0;
    while (fgets(name, sizeof name, list)) {
        name[strcspn(name, "\r\n")] = 0;
        if (!name[0])
            continue;
        ++n;
        char fin[2048], start[2048];
        snprintf(fin, sizeof fin, "%s/done/%s.done", dir, name);
        snprintf(start, sizeof start, "%s/done/%s.started", dir, name);
        if (exists(fin)) {
            ++skipped;
            continue;
        }
        if (exists(start)) {
            say("%u/%u %s: began in an earlier run and did not finish, left out", n, total, name);
            ++failed;
            continue;
        }
        touch(start);
        snprintf(path, sizeof path, "%s/spt/%s", dir, name);
        FILE *f = fopen(path, "rb");
        uint8_t *data = NULL;
        long len = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            len = ftell(f);
            fseek(f, 0, SEEK_SET);
            data = malloc(len > 0 ? (size_t)len : 1u);
            if (!data || fread(data, 1, (size_t)len, f) != (size_t)len)
                len = -1;
            fclose(f);
        }
        const clock_t t0 = clock();
        say("%u/%u %s (%ld bytes)", n, total, name, len);
        if (len < 0 || !data || !bake(c, data, (uint32_t)len, name)) {
            say("%u/%u %s: failed", n, total, name);
            ++failed;
        } else {
            touch(fin);
            ++done;
            say("%u/%u %s: done in %.1f s", n, total, name, (double)(clock() - t0) / CLOCKS_PER_SEC);
        }
        free(data);
    }
    fclose(list);
    say("end: %u done now, %u done before, %u failed or left out, of %u", done, skipped, failed, total);
    if (g_log)
        fclose(g_log);
    return 1;
}
