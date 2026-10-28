/* native/lod.c - CreateLoD's vertex lookup through a hash (group lod).
 *
 * A quick load of the player's save spends about 2.2 s of the game thread in CreateLoD (Engine 102a3300), most of it
 * in FUN_10548ad0 under FillHardBuffersAndPrimitves (work/sample-f9-s1.txt): for each corner of a triangle it walks
 * the builder's vertex records (ESI+8, 0x14 bytes each, the key at +0x10) from index [ESI+0x3c] to the count [ESI],
 * looking for the first record with the corner's key. Every new vertex makes the walk longer, so a mesh costs the
 * square of its vertices.
 *
 * native_site_10548c3a stands at the walk's start (start < count; EBP = start, EDX = count, ECX = the key, AL = 0) and
 * answers from a hash of key to record indices kept for the builder: records are hashed as they appear (they are only
 * ever appended; a builder whose count went down, or another builder, starts the hash again). Found: on at 10548c53,
 * the walk's match, with EBP and EBX at the record as the walk has them there (EDI = the records, as the walk loads).
 * Not found: on at 10548cb7 with the registers the walk leaves (EBP = EBX / 0x14 = the count, AL = 0).
 *
 * G3_LOD_CHECK=1 walks as the original does as well and logs any other answer. */
#include "chunk.h"
#include "g3_opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOD_FOUND 0x10548c53u
#define LOD_NOT_FOUND 0x10548cb7u

static uint32_t lod_esi;          /* the builder the hash is for */
static uint32_t lod_n;            /* records hashed */
static uint32_t lod_cap;          /* entries in lod_next */
static uint32_t *lod_next;        /* per record: the previous record in its bucket, or ~0u */
static uint32_t *lod_head;        /* per bucket: the latest record, or ~0u */
static uint32_t lod_buckets;      /* a power of two */
static int lod_check = -1;
uint64_t g3_lod_hits, g3_lod_walks;

static inline uint32_t lod_hash(uint32_t key) {
    return (key * 2654435761u) >> 7;
}

static void lod_rehash(uint32_t buckets) {
    free(lod_head);
    lod_buckets = buckets;
    lod_head = malloc(buckets * sizeof *lod_head);
    memset(lod_head, 0xff, buckets * sizeof *lod_head);
    lod_n = 0; /* lod_sync hashes every record again */
}

/* The hash brought up to `count` records of the builder at `esi`, their array at `arr`. */
static void lod_sync(uint32_t esi, uint32_t arr, uint32_t count) {
    if (esi != lod_esi || count < lod_n) {
        lod_esi = esi;
        lod_n = 0;
        if (!lod_head)
            lod_rehash(4096u);
        else
            memset(lod_head, 0xff, lod_buckets * sizeof *lod_head);
    }
    if (count > lod_cap) {
        uint32_t cap = lod_cap ? lod_cap : 4096u;
        while (cap < count)
            cap *= 2u;
        lod_next = realloc(lod_next, cap * sizeof *lod_next);
        lod_cap = cap;
    }
    if (count > lod_buckets / 2u) {
        uint32_t b = lod_buckets;
        while (b < count * 2u)
            b *= 2u;
        lod_rehash(b);
    }
    for (uint32_t i = lod_n; i < count; ++i) {
        const uint32_t h = lod_hash(rd32(arr + i * 0x14u + 0x10u)) & (lod_buckets - 1u);
        lod_next[i] = lod_head[h];
        lod_head[h] = i;
    }
    lod_n = count;
}

/* The first record in [start, count) with `key`, or ~0u. Buckets hold the latest record first. */
static uint32_t lod_find(uint32_t arr, uint32_t start, uint32_t key) {
    uint32_t best = ~0u;
    for (uint32_t i = lod_head[lod_hash(key) & (lod_buckets - 1u)]; i != ~0u && i >= start; i = lod_next[i])
        if (rd32(arr + i * 0x14u + 0x10u) == key)
            best = i;
    return best;
}

uint32_t native_site_10548c3a(X86 *c) {
    if (!G3OPT(LOD))
        return 0;
    uint32_t *const r = c->r;
    const uint32_t esi = r[R_ESI], start = r[R_EBP], count = r[R_EDX], key = r[R_ECX];
    const uint32_t arr = rd32(esi + 8u);
    if (count > 0x01000000u || start >= count)
        return 0;
    lod_sync(esi, arr, count);
    uint32_t found = lod_find(arr, start, key);
    if (lod_check < 0) {
        const char *e = getenv("G3_LOD_CHECK");
        lod_check = e && *e == '1';
    }
    if (lod_check) {
        uint32_t walk = ~0u;
        for (uint32_t i = start; i < count; ++i)
            if (rd32(arr + i * 0x14u + 0x10u) == key) {
                walk = i;
                break;
            }
        static int told;
        if (walk != found && told < 50) {
            ++told;
            fprintf(stderr, "[g3] lod: key %08x from %u of %u (builder %08x): the walk %d, the hash %d\n", key, start,
                    count, esi, (int)walk, (int)found);
        }
        found = walk;
    }
    ++g3_lod_walks;
    if (found == ~0u) {
        r[R_EBP] = count;
        r[R_EBX] = count * 0x14u;
        r[R_EAX] &= 0xffffff00u;
        return LOD_NOT_FOUND;
    }
    ++g3_lod_hits;
    r[R_EBP] = found;
    r[R_EBX] = found * 0x14u;
    r[R_EDI] = arr;
    r[R_EAX] &= 0xffffff00u;
    return LOD_FOUND;
}
