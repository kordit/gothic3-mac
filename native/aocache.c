/* native/aocache.c - a SpeedTree's ambient occlusion from a result computed once, outside the game (group aocache;
 * D-052, docs/perf/ao-drzew.md).
 *
 * The boundary is the whole eCAmbientOcclusionPreprocessor::eCAOMeshBase::Compute (Engine 10357ba0, thiscall, one
 * argument: the emitters, RET 4), which every Create*Geometry of eCSpeedTreeRenderBuffer calls through the receiving
 * mesh's vtable (+4) right after building it: the branches' and fronds' eCAOPerTriangleMesh16 against the group
 * CreateGemoetry made (the tree's leaves as spheres, the ground as 25 disks), the leaves' eCAOSpeedTreeLeafMesh
 * against itself. For a single receiving mesh the original takes from the emitters the meshes whose bounding sphere
 * is near (FindMeshesOfInterest 10357a90) and, if any, walks its six receiving trees (ComputePass 102ac6a0 or
 * 10359290), writing one float per receiving disk at disk+0x1c (ComputeOcclusion 10357c90: 1 - the harmonics' first
 * term * [106e8790] + ms_fShadowSoftness, clamped to [0, 1]) and counting the disks in ms_iDiskCounter (10862db0).
 * That is everything it leaves behind: the harmonics live on its stack, GetResults reads the disks afterwards.
 *
 * The key is a 128-bit hash of what the computation reads, in its walk order and without addresses: the x87 control
 * word, the Engine and SharedBase constants and the two sqrt flags native/ao.c reads, ms_fShadowSoftness, the
 * receiver's class, bounding sphere, flags and all its disk records (0x20 bytes each, the +0x1c each one starts with
 * included), its six trees as the receiving walk sees them (flags, children, disk indices), and each emitter mesh in
 * the group's order with the same plus its trees as the emitting walk sees them (flags, the node as one disk, then
 * children or disk indices). A class, a group or a tree this file does not know, a node whose disk array is not its
 * mesh's own, or more nodes than any tree has, and the original runs as before (native/ao.c inside it).
 *
 * A ready result is ~/Library/Caches/Gothic3Recomp/ao/<key>.ao, written by the offline tool (native/aobake.c,
 * tools/g3ao.sh) or by the game itself the first time it meets an input: the receiver's disk count, whether the walk
 * ran, the counter it ended with, the x87 exception flags it raised (sticky, so they are or-ed into the status word),
 * and each disk's +0x1c. With one, those are written and the call returns; without one, or with another disk count,
 * the original computes as today and its result is stored (session 27: the player's first game had none of the
 * tool's 250 keys, with other disk counts, so the tool does not yet rebuild the game's input exactly; the log line
 * says what the input was, to compare). The x87 condition codes
 * are not reproduced (no caller reads them before its own compare), nor EAX, ECX, EDX and XMM0, which the callers
 * never read after the call. The original also builds and frees a small group on the heap; a ready result does not.
 *
 * native/tests/aocache_test.c: the ready path against the original on random receivers and emitter groups, bit for
 * bit in every disk, the counter and the exception flags, and a changed input misses. */
#include "chunk.h"
#include "g3_opt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void fn_10357ba0(X86 *c);

#define VT_GROUP 0x1068ff58u     /* eCAOMeshGroup */
#define VT_TRIANGLES 0x1069013cu /* eCAOPerTriangleMesh16 */
#define VT_CUSTOM 0x106901acu    /* eCAOCustomMesh */
#define VT_LEAVES 0x106d0c74u    /* eCWraper_SpeedTreeRT::eCAOSpeedTreeLeafMesh (an eCAOCustomMesh) */
#define AO_DISK_COUNTER 0x10862db0u
#define AO_SOFTNESS 0x10862dbcu /* eCAmbientOcclusionPreprocessor::ms_fShadowSoftness */
#define MAX_NODES 4000000u
#define MAX_DISKS 4000000u

typedef struct {
    uint64_t h1, h2;
    uint32_t nodes;
    int bad;
} Key;

static void put(Key *k, uint32_t w) {
    for (int i = 0; i < 4; ++i) {
        k->h1 = (k->h1 ^ ((w >> (8 * i)) & 0xffu)) * 0x100000001b3ull;
    }
    k->h2 = (k->h2 ^ w) * 0x9e3779b97f4a7c15ull;
    k->h2 ^= k->h2 >> 29;
}
static void put_mem(Key *k, uint32_t a, uint32_t n) {
    for (uint32_t i = 0; i + 4 <= n; i += 4)
        put(k, rd32(a + i));
}

/* A mesh this file knows: where its disks, trees and sphere are. */
typedef struct {
    uint32_t vt, disks, count, nodes, sphere, skip, leaves;
} Mesh;

static int mesh_of(uint32_t m, Mesh *o) {
    if (!m)
        return 0;
    const uint32_t vt = rd32(m);
    if (vt == VT_TRIANGLES) {
        *o = (Mesh){vt, m + 0x48u, rd32(m + 0x4cu), m + 0x54u, m + 0x2e0u, 0, 0};
        return 1;
    }
    if (vt == VT_CUSTOM || vt == VT_LEAVES) {
        *o = (Mesh){vt, m + 0x4u, rd32(m + 0x8u), m + 0x10u, m + 0x29cu, m + 0x298u, vt == VT_LEAVES ? m + 0x2acu : 0};
        return 1;
    }
    return 0;
}

/* The receiving walk (eCAOTreeNode::Compute 10359a90): flag 4 ends it, flag 2 gives the disks by index, else the
 * eight children from +0x54 down. */
static void tree_receiving(Key *k, uint32_t node, uint32_t arr, int depth) {
    if (k->bad || ++k->nodes > MAX_NODES || depth > 64) {
        k->bad = 1;
        return;
    }
    const uint32_t f = rd32(node + 0x68u);
    put(k, f);
    if (f & 4u)
        return;
    if ((f & 2u) == 2u) {
        const int32_t n = (int32_t)rd32(node + 0x60u);
        put(k, (uint32_t)n);
        if (n > 0) {
            if (rd32(node + 0x58u) != arr || (uint32_t)n > MAX_DISKS) {
                k->bad = 1;
                return;
            }
            put_mem(k, rd32(node + 0x5cu), 4u * (uint32_t)n);
        }
        return;
    }
    for (uint32_t at = 0x54u; at >= 0x38u; at -= 4u) {
        const uint32_t child = rd32(node + at);
        put(k, child != 0);
        if (child)
            tree_receiving(k, child, arr, depth + 1);
    }
}

/* The emitting walk (native/ao.c walk): flag 8 or nothing, the node as one disk (+0x00..+0x1c), then the children
 * from +0x54 down or, with flag 2, the disks by index. */
static void tree_emitting(Key *k, uint32_t node, uint32_t arr, int depth) {
    if (k->bad || ++k->nodes > MAX_NODES || depth > 64) {
        k->bad = 1;
        return;
    }
    const uint32_t f = rd32(node + 0x68u);
    put(k, f);
    if (!(f & 8u))
        return;
    put_mem(k, node, 0x20u);
    if (!((f >> 1) & 1u)) {
        for (uint32_t at = 0x54u; at >= 0x38u; at -= 4u) {
            const uint32_t child = rd32(node + at);
            put(k, child != 0);
            if (child)
                tree_emitting(k, child, arr, depth + 1);
        }
        return;
    }
    const int32_t n = (int32_t)rd32(node + 0x60u);
    put(k, (uint32_t)n);
    if (n > 0) {
        if (rd32(node + 0x58u) != arr || (uint32_t)n > MAX_DISKS) {
            k->bad = 1;
            return;
        }
        put_mem(k, rd32(node + 0x5cu), 4u * (uint32_t)n);
    }
}

/* Class, sphere, flags and every disk record; the disks are indexed, so a record's place in the array is its name. */
static void mesh_head(Key *k, const Mesh *m) {
    put(k, m->vt);
    put_mem(k, m->sphere, 0x10u);
    put(k, m->skip ? rd8(m->skip) : 0xffffffffu);
    put(k, m->leaves ? rd8(m->leaves) : 0xffffffffu);
    put(k, m->count);
    if (m->count > MAX_DISKS) {
        k->bad = 1;
        return;
    }
    if (m->count)
        put_mem(k, rd32(m->disks), 0x20u * m->count);
}

static void key_of(X86 *c, uint32_t self, uint32_t emitters, const Mesh *r, Key *k) {
    *k = (Key){0xcbf29ce484222325ull, 0x6a09e667f3bcc909ull, 0, 0};
    put(k, 0x414f4b31u); /* "AOK1" */
    put(k, c->fpu_cw);
    static const uint32_t consts[] = {
        0x106e8790u, 0x106e8794u, 0x106e8648u, 0x106e864cu, 0x106e86e8u, 0x106e86ecu, 0x106e85b8u, 0x106e85bcu,
        0x106e85c0u, 0x106e85c4u, 0x106e85c8u, 0x106e85ccu, 0x106e85d0u, 0x106e85d4u, 0x106e85d8u, 0x106e85dcu,
        0x106e85e0u, 0x106e85e4u, 0x10686158u, 0x1068615cu, 0x1068612cu, 0x10861e38u, 0x100d4118u, 0x1008b4b0u,
        0x10090a38u, AO_SOFTNESS};
    for (size_t i = 0; i < sizeof consts / sizeof consts[0]; ++i)
        put(k, rd32(consts[i]));
    put(k, rd32(rd32(0x105f6864u))); /* SharedBase g_fEpsilon */
    put(k, rd32(rd32(0x105f63fcu))); /* g_fPI */
    mesh_head(k, r);
    for (uint32_t i = 0; i < 6u && !k->bad; ++i)
        tree_receiving(k, r->nodes + 0x6cu * i, r->disks, 0);
    /* The emitters: a group's meshes in its order (its ComputeOcclusionFactor walks them from the last), or one mesh,
     * the receiver itself for the leaves. */
    uint32_t list[64], n = 0;
    if (rd32(emitters) == VT_GROUP) {
        n = rd32(emitters + 0x8u);
        if (n > 64u) {
            k->bad = 1;
            return;
        }
        for (uint32_t i = 0; i < n; ++i)
            list[i] = rd32(rd32(emitters + 0x4u) + 4u * i);
    } else
        list[n++] = emitters;
    put(k, rd32(emitters) == VT_GROUP ? 0x47525550u : 0x4f4e4521u);
    put(k, n);
    for (uint32_t i = 0; i < n && !k->bad; ++i) {
        Mesh e;
        if (!mesh_of(list[i], &e)) {
            k->bad = 1;
            return;
        }
        if (list[i] == self) {
            put(k, 0x53454c46u); /* "SELF": its records are above */
        } else
            mesh_head(k, &e);
        for (uint32_t t = 0; t < 6u && !k->bad; ++t)
            tree_emitting(k, e.nodes + 0x6cu * t, e.disks, 0);
    }
}

/* ---- the files ---------------------------------------------------------------------------------------------- */

#define AO_MAGIC 0x314f4133u /* "3AO1" */
typedef struct {
    uint32_t magic, count, ran, counter, raised, reserved;
} Head;

static int path_of(const Key *k, char *out, size_t n, int dir_only) {
    const char *home = getenv("HOME");
    if (!home || !*home)
        return 0;
    if (dir_only)
        return snprintf(out, n, "%s/Library/Caches/Gothic3Recomp/ao", home) < (int)n;
    return snprintf(out, n, "%s/Library/Caches/Gothic3Recomp/ao/%016llx%016llx.ao", home, (unsigned long long)k->h1,
                    (unsigned long long)k->h2) < (int)n;
}

unsigned g3n_aocache_hits, g3n_aocache_misses, g3n_aocache_unknown; /* the log line and the tests */
int g3n_aocache_store;   /* aobake.c: compute and write instead of reading */
uint32_t g3n_aocache_last_disks; /* the last receiver's disk count (aobake.c's log) */

static int apply(X86 *c, const Key *k, const Mesh *r) {
    char path[1024];
    if (!path_of(k, path, sizeof path, 0))
        return 0;
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    Head h;
    int ok = read(fd, &h, sizeof h) == (ssize_t)sizeof h && h.magic == AO_MAGIC && h.count == r->count;
    float *v = NULL;
    if (ok && h.count) {
        v = malloc(4u * (size_t)h.count);
        ok = v && read(fd, v, 4u * (size_t)h.count) == (ssize_t)(4u * (size_t)h.count);
    }
    close(fd);
    if (ok) {
        const uint32_t base = rd32(r->disks);
        for (uint32_t i = 0; i < h.count; ++i)
            wrf32(base + 0x20u * i + 0x1cu, v[i]);
        if (h.ran)
            wr32(AO_DISK_COUNTER, h.counter);
        c->fpu_sw |= (uint16_t)(h.raised & 0x3fu);
    }
    free(v);
    return ok;
}

static void store(X86 *c, const Key *k, const Mesh *r, uint32_t ran, uint32_t counter, uint32_t raised) {
    char dir[1024], path[1024], part[1100];
    if (!path_of(k, dir, sizeof dir, 1) || !path_of(k, path, sizeof path, 0)) {
        fprintf(stderr, "[g3] ao: no HOME, the result is not stored\n");
        return;
    }
    snprintf(part, sizeof part, "%s.part", path);
    /* The folder is made only when the file cannot be: a mkdir per path component on every store was 35 ms of a
     * player's 240 stores. */
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 && errno == ENOENT) {
        for (char *p = dir + 1; *p; ++p)
            if (*p == '/') {
                *p = 0;
                mkdir(dir, 0755);
                *p = '/';
            }
        mkdir(dir, 0755);
        fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd < 0) {
        fprintf(stderr, "[g3] ao: cannot write %s (%s)\n", part, strerror(errno));
        return;
    }
    /* The whole file in one write: a write per float was 585 ms of the same game's 2.8 s freeze. */
    const size_t size = sizeof(Head) + 4u * (size_t)r->count;
    uint8_t *buf = malloc(size);
    int ok = buf != NULL;
    if (ok) {
        const Head h = {AO_MAGIC, r->count, ran, counter, raised, 0};
        memcpy(buf, &h, sizeof h);
        const uint32_t base = rd32(r->disks);
        for (uint32_t i = 0; i < r->count; ++i) {
            const uint32_t v = rd32(base + 0x20u * i + 0x1cu);
            memcpy(buf + sizeof h + 4u * i, &v, 4);
        }
        ok = write(fd, buf, size) == (ssize_t)size;
    }
    free(buf);
    ok = fsync(fd) == 0 && ok;
    close(fd);
    if (ok && rename(part, path) == 0)
        return;
    fprintf(stderr, "[g3] ao: cannot write %s (%s)\n", path, strerror(errno));
    unlink(part);
    (void)c;
}

/* What the log says about an input, to compare a game's with the tool's: the control word, the emitters' disk counts
 * and the receiver's first disk. */
static void describe(X86 *c, uint32_t self, uint32_t emitters, const Mesh *r, char *out, size_t n) {
    int len = snprintf(out, n, "cw %04x, emitters", c->fpu_cw);
    if (rd32(emitters) == VT_GROUP) {
        const uint32_t count = rd32(emitters + 0x8u);
        for (uint32_t i = 0; i < count && i < 8u && len > 0 && len < (int)n; ++i) {
            Mesh e;
            const uint32_t m = rd32(rd32(emitters + 0x4u) + 4u * i);
            len += snprintf(out + len, n - len, " %u", mesh_of(m, &e) ? e.count : 0xffffffffu);
        }
    } else
        len += snprintf(out + len, n - len, " %s", emitters == self ? "itself" : "one mesh");
    if (r->count && len > 0 && len < (int)n) {
        const uint32_t d = rd32(r->disks);
        snprintf(out + len, n - len, ", first disk %08x %08x %08x", rd32(d), rd32(d + 4u), rd32(d + 8u));
    }
}

/* eCAOMeshBase::Compute(eCAOMeshBase *emitters) */
void g3n_10357ba0(X86 *c) {
    const uint32_t self = c->r[1], emitters = rd32(c->r[4] + 4u);
    Mesh r;
    if (!G3OPT(AOCACHE) || !mesh_of(self, &r)) {
        fn_10357ba0(c);
        return;
    }
    Key k;
    key_of(c, self, emitters, &r, &k);
    g3n_aocache_last_disks = r.count;
    if (k.bad) {
        ++g3n_aocache_unknown;
        fn_10357ba0(c);
        return;
    }
    if (!g3n_aocache_store && apply(c, &k, &r)) {
        ++g3n_aocache_hits;
        c->eip = rd32(c->r[4]);
        c->r[4] += 8u;
        recomp_return(c);
        return;
    }
    char what[256];
    describe(c, self, emitters, &r, what, sizeof what);
    if (!g3n_aocache_store) {
        ++g3n_aocache_misses;
        fprintf(stderr, "[g3] ao: no ready result for a receiver of %u disks (key %016llx%016llx; %s), computing it "
                "and storing it\n", r.count, (unsigned long long)k.h1, (unsigned long long)k.h2, what);
    } else
        fprintf(stderr, "[g3] ao: computing a receiver of %u disks (key %016llx%016llx; %s)\n", r.count,
                (unsigned long long)k.h1, (unsigned long long)k.h2, what);
    /* The original computes as before and the result is kept under the key of what it read, so the same input never
     * has to be computed on this machine again. Only the walk's own flags are recorded, and a counter it can only
     * have reset. */
    const uint16_t sw = c->fpu_sw;
    const uint32_t counter = rd32(AO_DISK_COUNTER);
    c->fpu_sw &= (uint16_t)~0x3fu;
    wr32(AO_DISK_COUNTER, 0xa0c0ffeeu);
    fn_10357ba0(c);
    const uint32_t after = rd32(AO_DISK_COUNTER), ran = after != 0xa0c0ffeeu;
    const uint32_t raised = c->fpu_sw & 0x3fu;
    c->fpu_sw |= (uint16_t)(sw & 0x3fu);
    if (!ran)
        wr32(AO_DISK_COUNTER, counter);
    store(c, &k, &r, ran, after, raised);
}
