/* native/archive.c - FileSystem.dll's packed-file read 10da33c0, decompressed in one go (group zlib).
 *
 * Session 12, the player's fight with World Texture Mix (work/play.log 14:15-14:20): single frames of
 * 120-355 ms, 2-4 every 10 s, with the game thread's CPU time as long as the frame and no page-ins.
 * The samples taken after them (work/play-sample-*-spike-*.txt) have eCResourceImage_PS::DoLoad
 * 104131b0 under ExecutePass: a texture drawn for the first time is read from its archive and
 * inflated in the middle of the frame, and the PVS prefetcher does next to nothing (0.0-0.1%).
 * About 80% of DoLoad is inflate, which native/zlib.c runs at about 250 MB/s because it has to keep
 * zlib 1.2.5's state byte for byte between the 128 KB chunks this function feeds it.
 *
 * 10da33c0 (fastcall: ECX = &bytes read, EDX = the destination memory file; stack: the archive entry,
 * the source stream, &bytes produced; RET 0xc) reads the entry's packed bytes from the source stream
 * 128 KB at a time and inflates them into a buffer it appends to the destination (10da2530). Nothing
 * outside it sees the z_stream: it lives in the function's frame, and inflateInit/inflateEnd only
 * allocate and free its state. So for an entry that is compressed and not encrypted, g3n_10da33c0
 * makes the same calls on both streams in the same order (size, seek, the destination's reset,
 * the append, then per chunk the source's +4 call, Read, the +4 call again), keeps the packed bytes
 * on the host, and decompresses them with libdeflate in one call. libdeflate checks the stream's
 * adler32, so a success is the same bytes inflate gives. The results are the original's: bytes read
 * (every chunk read), bytes produced, AL = produced == the entry's size; a damaged stream returns
 * AL = 0 as inflate's error does. What differs: inflate's state is not allocated (D-027), and a
 * stream that ends before its last chunk has every chunk read instead of stopping at that one (the
 * entries are written whole by the packer, so the end of the stream is the end of the entry).
 * Any other entry (stored, encrypted, empty) runs the original. A result of 256 KB or more is also
 * kept in a decompressed-entry cache on disk (below), checked before inflating.
 *
 * Stage 4 (native/loading.c): on a cache's loader thread, an entry of AR_LEND bytes or more is looked up in the
 * disk cache and inflated without the baton (recomp_host_work_begin), so the game thread runs guest code meanwhile.
 * That stretch touches only host memory and the destination buffer the guest has just grown for this entry. The
 * game thread keeps the baton: the inflate is on its own critical path either way, and lending the baton would add
 * the wait for it to come back. */
#include "chunk.h"
#include "g3_opt.h"
#include "third_party/libdeflate.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void fn_10da33c0(X86 *c); /* FileSystem: read a packed archive entry */
int g3_on_game_thread(void);  /* phases.c */
_Bool recomp_host_work_begin(void); /* kit/runtime/kernel32.cpp */
void recomp_host_work_end(void);

/* kit/runtime (C++): guest.h guest_call (pushes the arguments, runs the function, restores ESP). */
uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");

#define AR_APPEND 0x10da2530u   /* destination memory file (ESI): grow by [ESP+4], return the new bytes */
#define AR_CHUNK 0x20000u       /* the original's input chunk */
#define AR_FRAME 0x20100u       /* guest stack this function takes below ESP: the chunk and two locals */
#define AR_LEND (64u << 10)     /* a loader thread inflates this much or more without the baton */

static inline uint8_t *ar_host(uint32_t a) {
    return (uint8_t *)g_mem + a;
}
static inline int ar_span(uint32_t lo, uint32_t n) {
    return n == 0u || ((uint64_t)lo + n <= 0x100000000ull && RECOMP_STRING_FAST_ADDR(lo));
}

/* A virtual call on a stream through its virtual base, as the original spells it:
 * ECX = obj + [[obj+4]+8] + 4, the function at [[ECX] + slot]. EAX (EDX in *hi) comes back. */
static uint32_t ar_vcall(X86 *c, uint32_t obj, uint32_t slot, const uint32_t *args, int n, uint32_t *hi) {
    const uint32_t self = obj + rd32(rd32(obj + 4u) + 8u) + 4u;
    c->r[R_ECX] = self;
    const uint32_t eax = guest_call(c, rd32(rd32(self) + slot), args, n);
    if (hi)
        *hi = c->r[R_EDX];
    return eax;
}

static _Thread_local struct libdeflate_decompressor *ar_dec;

/* The decompressed-entry cache: a texture the game reads again (in this session after its image
 * cache dropped it, or in a later one) is a file read instead of an inflate. The key is the packed
 * bytes' crc32 with both sizes and the stream's own adler32 of the result (its last four bytes), so a
 * changed archive or mod can never hand back stale bytes. Files go to
 * ~/Library/Caches/Gothic3Recomp/inflate (RECOMP_INFLATE_CACHE overrides, "0" turns the cache off),
 * written on a background thread through a temporary name, at most AR_CACHE_MAX bytes in all. */
#define AR_CACHE_MIN (256u << 10)          /* smaller entries inflate faster than a file opens */
#define AR_CACHE_MAX (12ull << 30)
static char ar_dir[1024];
static int ar_dir_state;                   /* 0 not looked up, 1 usable, -1 off */
static uint64_t ar_cache_bytes;            /* what this session wrote */
static int ar_writers;                     /* background writes in progress */
static pthread_mutex_t ar_lock = PTHREAD_MUTEX_INITIALIZER;

static int ar_cache_dir(void) {
    if (ar_dir_state)
        return ar_dir_state > 0;
    ar_dir_state = -1;
    const char *env = getenv("RECOMP_INFLATE_CACHE");
    if (env && !strcmp(env, "0"))
        return 0;
    if (env && *env)
        snprintf(ar_dir, sizeof ar_dir, "%s", env);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home)
            return 0;
        snprintf(ar_dir, sizeof ar_dir, "%s/Library/Caches/Gothic3Recomp", home);
        mkdir(ar_dir, 0755);
        snprintf(ar_dir, sizeof ar_dir, "%s/Library/Caches/Gothic3Recomp/inflate", home);
    }
    mkdir(ar_dir, 0755);
    struct stat st;
    if (stat(ar_dir, &st) == 0 && S_ISDIR(st.st_mode))
        ar_dir_state = 1;
    return ar_dir_state > 0;
}
static void ar_cache_name(char *out, size_t n, uint32_t crc, uint32_t packed, uint32_t size, uint32_t adler) {
    snprintf(out, n, "%s/%08x-%08x-%08x-%08x", ar_dir, crc, packed, size, adler);
}
/* The cached bytes into out (exactly size of them), 1 on a hit. */
static int ar_cache_read(const char *name, uint8_t *out, uint32_t size) {
    FILE *f = fopen(name, "rb");
    if (!f)
        return 0;
    const int ok = fread(out, 1, size, f) == size && fgetc(f) == EOF;
    fclose(f);
    return ok;
}
struct ar_job {
    char name[1100];
    uint8_t *bytes;
    uint32_t size;
};
static void *ar_write(void *p) {
    struct ar_job *j = (struct ar_job *)p;
    char tmp[1120];
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", j->name, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(j->bytes, 1, j->size, f) == j->size;
    if (f)
        ok = fclose(f) == 0 && ok;
    if (ok)
        ok = rename(tmp, j->name) == 0;
    if (!ok)
        unlink(tmp);
    free(j->bytes);
    free(j);
    pthread_mutex_lock(&ar_lock);
    --ar_writers;
    pthread_mutex_unlock(&ar_lock);
    return 0;
}
/* A copy of the result goes to the cache on its own thread; skipped when four writes are already
 * running or the session's share is used up (nothing is lost, the entry inflates next time). */
static void ar_cache_store(const char *name, const uint8_t *bytes, uint32_t size) {
    pthread_mutex_lock(&ar_lock);
    const int go = ar_writers < 4 && ar_cache_bytes + size <= AR_CACHE_MAX;
    if (go)
        ++ar_writers, ar_cache_bytes += size;
    pthread_mutex_unlock(&ar_lock);
    if (!go)
        return;
    struct ar_job *j = (struct ar_job *)malloc(sizeof *j);
    uint8_t *copy = j ? (uint8_t *)malloc(size) : 0;
    pthread_t t;
    pthread_attr_t a;
    int started = 0;
    if (copy) {
        memcpy(copy, bytes, size);
        snprintf(j->name, sizeof j->name, "%s", name);
        j->bytes = copy;
        j->size = size;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&t, &a, ar_write, j) == 0;
        pthread_attr_destroy(&a);
    }
    if (!started) {
        free(copy);
        free(j);
        pthread_mutex_lock(&ar_lock);
        --ar_writers;
        pthread_mutex_unlock(&ar_lock);
    }
}

void g3n_10da33c0(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    const uint32_t read_out = c->r[R_ECX], dst = c->r[R_EDX];
    const uint32_t entry = rd32(esp + 4u), src = rd32(esp + 8u), made_out = rd32(esp + 12u);
    if (!G3OPT(ZLIB) || !ar_span(entry, 0x60u) || !ar_span(read_out, 4u) || !ar_span(made_out, 4u) ||
        rd32(entry + 0x5cu) == 0u || rd32(entry + 0x58u) != 0u || rd32(entry + 0x50u) == 0u ||
        rd32(entry + 0x54u) != 0u) {
        fn_10da33c0(c);
        return;
    }
    const uint32_t saved[8] = {c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]};
    const uint32_t ret = rd32(esp);
    uint32_t al = 0;
    wr32(read_out, 0u);
    wr32(made_out, 0u);
    /* Our frame: the chunk at [base, base + 128 KB), the +4 call's 8-byte local after it. */
    const uint32_t base = esp - AR_FRAME, local = base + AR_CHUNK;
    c->r[R_ESP] = base;
    uint32_t hi = 0;
    const uint32_t lo = ar_vcall(c, src, 0u, 0, 0, &hi);
    if (lo == 0u && hi == 0u) { /* nothing to read: the original returns AL = 1 */
        al = 1;
        goto done;
    }
    {
        const uint32_t seek[3] = {rd32(entry + 0x40u), rd32(entry + 0x44u), 0u};
        if ((ar_vcall(c, src, 8u, seek, 3, 0) & 0xffu) == 0u)
            goto done;
    }
    const uint32_t packed = rd32(entry + 0x48u), size = rd32(entry + 0x50u);
    {
        const uint32_t zero[3] = {0u, 0u, 0u};
        ar_vcall(c, dst, 8u, zero, 3, 0);
    }
    c->r[R_ESI] = dst;
    const uint32_t out = guest_call(c, AR_APPEND, &size, 1);
    if (!ar_span(out, size) || !ar_span(base, AR_CHUNK + 8u))
        goto done; /* the original would inflate into it anyway; a bad buffer faults there too */
    uint8_t *in = packed ? (uint8_t *)malloc(packed) : 0;
    if (packed && !in)
        goto done;
    uint32_t got = 0;
    while (got < packed) {
        wr32(local, 0u);
        wr32(local + 4u, 0u);
        ar_vcall(c, src, 4u, &local, 1, 0);
        const uint32_t want = packed - got > AR_CHUNK ? AR_CHUNK : packed - got;
        const uint32_t args[2] = {base, want};
        uint32_t n = ar_vcall(c, src, 0xcu, args, 2, 0);
        if (n > want)
            n = want;
        memcpy(in + got, ar_host(base), n);
        got += n;
        wr32(read_out, got);
        ar_vcall(c, src, 4u, &local, 1, 0);
        if (n == 0u)
            break;
    }
    /* The stream's adler32 of the whole result is its last four bytes (big-endian). */
    const int lent = size >= AR_LEND && !g3_on_game_thread() && recomp_host_work_begin();
    char name[1100];
    int cached = 0;
    if (size >= AR_CACHE_MIN && got == packed && got >= 6u && ar_cache_dir()) {
        const uint32_t adler = (uint32_t)in[got - 4u] << 24 | (uint32_t)in[got - 3u] << 16 |
                               (uint32_t)in[got - 2u] << 8 | in[got - 1u];
        ar_cache_name(name, sizeof name, (uint32_t)libdeflate_crc32(0, in, got), packed, size, adler);
        cached = 1;
        if (ar_cache_read(name, ar_host(out), size)) {
            if (lent)
                recomp_host_work_end();
            wr32(made_out, size);
            al = 1;
            free(in);
            goto done;
        }
    }
    if (!ar_dec)
        ar_dec = libdeflate_alloc_decompressor();
    size_t used = 0, made = 0;
    const int ok = ar_dec && libdeflate_zlib_decompress_ex(ar_dec, in, got, ar_host(out), size, &used, &made) ==
                                 LIBDEFLATE_SUCCESS;
    if (ok && made == size && cached && used == got)
        ar_cache_store(name, ar_host(out), size);
    if (lent)
        recomp_host_work_end();
    if (ok) {
        wr32(made_out, (uint32_t)made);
        al = made == size;
    }
    free(in);
done:
    for (int i = 0; i < 8; ++i)
        c->r[i] = saved[i];
    c->r[R_EAX] = al;
    c->eip = ret;
    c->r[R_ESP] = esp + 16u;
    recomp_return(c);
}
