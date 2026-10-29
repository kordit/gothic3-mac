/* native/fmodcheck.c - tools/g3fmodcheck.sh: each of the game's Ogg sounds made by FMOD Ex as the game makes it and
 * from native/fmodogg.c's PCM, side by side, without the game (D-053, docs/perf/fmod-ogg.md).
 *
 * With RECOMP_FMOD_CHECK=<folder> native/aobake.c's recomp_entry_takeover hands the process to g3n_fmodcheck once
 * the modules have attached: no window, no world, no profile. It makes its own FMOD system (FMOD_System_Create
 * 113ddc50, setOutput NOSOUND, init with 32 channels) and for each name in <folder>/list.txt (a file under
 * <folder>/ogg/) calls System::createSound twice: with the game's mode (3D | SOFTWARE | CREATESAMPLE | ACCURATETIME |
 * IGNORETAGS) and the file in memory, so fmodex decodes it with its own libvorbis; and with fmodogg.c's PCM as
 * OPENMEMORY | OPENRAW, as the game's call does now. Then for both: the result, the length in PCM bytes, the format,
 * the channels, the bits, the default frequency, and the PCM through Sound::lock: how many 16-bit samples differ and
 * by how much at most. One line per file to <folder>/results.tsv as soon as it is done; <folder>/started.txt names a
 * file before it begins, so one that crashed the tool is left out on the next run. */
#include "chunk.h"
#include "g3_opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");
uint32_t heap_alloc(uint32_t size, _Bool zero, uint32_t align) __asm__("__Z10heap_allocjbj");
_Bool heap_free(uint32_t addr) __asm__("__Z9heap_freej");
int g3n_fmodogg_decode(const uint8_t *file, size_t size, uint8_t **pcm, uint32_t *bytes, int *channels, int *rate);

#define F_SYSTEM_CREATE 0x113ddc50u /* FMOD_System_Create(FMOD_SYSTEM **) */
#define F_SET_OUTPUT 0x113bed10u    /* System::setOutput(FMOD_OUTPUTTYPE) */
#define F_INIT 0x113bf3d0u          /* System::init(int, FMOD_INITFLAGS, void *) */
#define F_CREATE_SOUND 0x113bf8d0u  /* System::createSound(const char *, FMOD_MODE, FMOD_CREATESOUNDEXINFO *, Sound **) */
#define F_GET_LENGTH 0x113be0a0u    /* Sound::getLength(unsigned *, FMOD_TIMEUNIT) */
#define F_GET_FORMAT 0x113be0f0u    /* Sound::getFormat(type *, format *, int *channels, int *bits) */
#define F_GET_DEFAULTS 0x113bdc60u  /* Sound::getDefaults(float *frequency, float *volume, float *pan, int *priority) */
#define F_LOCK 0x113bdb50u          /* Sound::lock(offset, length, void **, void **, unsigned *, unsigned *) */
#define F_UNLOCK 0x113bdbb0u        /* Sound::unlock(void *, void *, unsigned, unsigned) */
#define F_RELEASE 0x113bdaf0u       /* Sound::release() */
#define GAME_MODE (0x4150u | 0x2000000u)
#define OPENMEMORY 0x800u
#define OPENRAW 0x1000u
#define OUTPUT_NOSOUND 2u
#define TIMEUNIT_PCMBYTES 4u

typedef struct {
    uint32_t result, length, type, format, channels, bits, freq_bits, sound;
    double ms;
} Made;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* scr: 0x40 bytes of guest memory for the out parameters */
static void make(X86 *c, uint32_t sys, uint32_t data, uint32_t size, uint32_t mode, int channels, int rate,
                 uint32_t scr, Made *m) {
    const uint32_t ex = heap_alloc(0x88u, 1, 16);
    wr32(ex, 0x88u);
    wr32(ex + 4u, size);
    if (mode & OPENRAW) {
        wr32(ex + 0x0cu, (uint32_t)channels);
        wr32(ex + 0x10u, (uint32_t)rate);
        wr32(ex + 0x14u, 2u);
    }
    memset(m, 0, sizeof *m);
    wr32(scr, 0);
    const double t0 = now_ms();
    m->result = guest_call(c, F_CREATE_SOUND, (const uint32_t[]){sys, data, mode, ex, scr}, 5);
    m->ms = now_ms() - t0;
    heap_free(ex);
    m->sound = rd32(scr);
    if (m->result || !m->sound)
        return;
    guest_call(c, F_GET_LENGTH, (const uint32_t[]){m->sound, scr + 4u, TIMEUNIT_PCMBYTES}, 3);
    m->length = rd32(scr + 4u);
    guest_call(c, F_GET_FORMAT, (const uint32_t[]){m->sound, scr + 8u, scr + 12u, scr + 16u, scr + 20u}, 5);
    m->type = rd32(scr + 8u);
    m->format = rd32(scr + 12u);
    m->channels = rd32(scr + 16u);
    m->bits = rd32(scr + 20u);
    guest_call(c, F_GET_DEFAULTS, (const uint32_t[]){m->sound, scr + 24u, scr + 28u, scr + 32u, scr + 36u}, 5);
    m->freq_bits = rd32(scr + 24u);
}

/* The sample's PCM through Sound::lock, into host memory (NULL if it cannot be locked). */
static uint8_t *pcm_of(X86 *c, const Made *m, uint32_t scr) {
    if (!m->sound || !m->length)
        return NULL;
    if (guest_call(c, F_LOCK, (const uint32_t[]){m->sound, 0, m->length, scr, scr + 4u, scr + 8u, scr + 12u}, 7))
        return NULL;
    const uint32_t p1 = rd32(scr), p2 = rd32(scr + 4u), l1 = rd32(scr + 8u), l2 = rd32(scr + 12u);
    uint8_t *out = NULL;
    if (l1 + l2 == m->length && (out = malloc(m->length)) != NULL) {
        memcpy(out, g_mem + p1, l1);
        if (l2)
            memcpy(out + l1, g_mem + p2, l2);
    }
    guest_call(c, F_UNLOCK, (const uint32_t[]){m->sound, p1, p2, l1, l2}, 5);
    return out;
}

static int listed(const char *text, const char *name) {
    const size_t n = strlen(name);
    for (const char *p = text; p && (p = strstr(p, name)) != NULL; p += n)
        if ((p == text || p[-1] == '\n') && (p[n] == '\t' || p[n] == '\n'))
            return 1;
    return 0;
}
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return calloc(1, 1);
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (s) {
        s[fread(s, 1, (size_t)n, f)] = 0;
    }
    fclose(f);
    return s;
}

int g3n_fmodcheck(X86 *c) {
    const char *dir = getenv("RECOMP_FMOD_CHECK");
    char path[2048], name[1024];
    snprintf(path, sizeof path, "%s/check.log", dir);
    FILE *log = fopen(path, "a");
    if (!log)
        return 1;
#define SAY(...)                                                                                                      \
    do {                                                                                                              \
        time_t t_ = time(NULL);                                                                                        \
        char w_[16];                                                                                                   \
        strftime(w_, sizeof w_, "%H:%M:%S", localtime(&t_));                                                           \
        fprintf(log, "%s ", w_);                                                                                       \
        fprintf(log, __VA_ARGS__);                                                                                     \
        fputc('\n', log);                                                                                              \
        fflush(log);                                                                                                   \
    } while (0)
    snprintf(path, sizeof path, "%s/list.txt", dir);
    FILE *list = fopen(path, "r");
    if (!list) {
        SAY("no %s", path);
        fclose(log);
        return 1;
    }
    snprintf(path, sizeof path, "%s/results.tsv", dir);
    char *done = slurp(path);
    FILE *res = fopen(path, "a");
    snprintf(path, sizeof path, "%s/started.txt", dir);
    char *started = slurp(path);
    FILE *start = fopen(path, "a");
    if (!res || !start || !done || !started) {
        SAY("cannot open the results");
        return 1;
    }
    if (!done[0])
        fprintf(res, "name\tbytes\tresult\tresult_native\tlength\tlength_native\ttype\ttype_native\tformat\t"
                     "format_native\tchannels\tchannels_native\tbits\tbits_native\tfreq\tfreq_native\tsamples_differ\t"
                     "max_diff\tms_fmod\tms_native\n");

    const uint32_t scr = heap_alloc(0x40u, 1, 16);
    uint32_t r = guest_call(c, F_SYSTEM_CREATE, (const uint32_t[]){scr}, 1);
    const uint32_t sys = rd32(scr);
    const uint32_t r_out = sys ? guest_call(c, F_SET_OUTPUT, (const uint32_t[]){sys, OUTPUT_NOSOUND}, 2) : 99;
    const uint32_t r_init = sys ? guest_call(c, F_INIT, (const uint32_t[]){sys, 32, 0, 0}, 4) : 99;
    SAY("FMOD system %08x: create %u, setOutput %u, init %u", sys, r, r_out, r_init);
    if (r || r_init || !sys)
        return 1;
    g3_opt |= G3OPT_FMODOGG;

    unsigned n = 0, total = 0, same = 0, bad = 0, skipped = 0;
    while (fgets(name, sizeof name, list))
        total += name[0] != '\n';
    rewind(list);
    double fmod_ms = 0, native_ms = 0;
    while (fgets(name, sizeof name, list)) {
        name[strcspn(name, "\r\n")] = 0;
        if (!name[0])
            continue;
        ++n;
        if (listed(done, name)) {
            ++skipped;
            continue;
        }
        if (listed(started, name)) {
            SAY("%u/%u %s: began in an earlier run and did not finish, left out", n, total, name);
            fprintf(res, "%s\tCRASHED\n", name);
            fflush(res);
            ++bad;
            continue;
        }
        fprintf(start, "%s\n", name);
        fflush(start);
        snprintf(path, sizeof path, "%s/ogg/%s", dir, name);
        FILE *f = fopen(path, "rb");
        uint8_t *file = NULL;
        long size = -1;
        if (f) {
            fseek(f, 0, SEEK_END);
            size = ftell(f);
            fseek(f, 0, SEEK_SET);
            file = malloc(size > 0 ? (size_t)size : 1u);
            if (!file || fread(file, 1, (size_t)size, f) != (size_t)size)
                size = -1;
            fclose(f);
        }
        if (size <= 0) {
            fprintf(res, "%s\tUNREADABLE\n", name);
            fflush(res);
            free(file);
            ++bad;
            continue;
        }
        const uint32_t data = heap_alloc((uint32_t)size, 0, 16);
        memcpy(g_mem + data, file, (size_t)size);
        Made a, b;
        make(c, sys, data, (uint32_t)size, GAME_MODE | OPENMEMORY, 0, 0, scr, &a);
        heap_free(data);

        uint8_t *pcm = NULL;
        uint32_t bytes = 0;
        int channels = 0, rate = 0;
        const double t0 = now_ms();
        const int ok = g3n_fmodogg_decode(file, (size_t)size, &pcm, &bytes, &channels, &rate);
        const double t1 = now_ms();
        free(file);
        memset(&b, 0, sizeof b);
        if (ok) {
            const uint32_t g = heap_alloc(bytes ? bytes : 1u, 0, 16);
            memcpy(g_mem + g, pcm, bytes);
            make(c, sys, g, bytes, GAME_MODE | OPENMEMORY | OPENRAW, channels, rate, scr, &b);
            b.ms += t1 - t0;
            heap_free(g);
        } else
            b.result = 0xffffffffu; /* fmodogg.c leaves this file to FMOD */
        free(pcm);

        uint8_t *pa = pcm_of(c, &a, scr + 0x20u), *pb = ok ? pcm_of(c, &b, scr + 0x20u) : NULL;
        long differ = -1, maxd = -1;
        if (pa && pb && a.length == b.length) {
            differ = maxd = 0;
            for (uint32_t i = 0; i + 1 < a.length; i += 2) {
                const int d = abs((int)(int16_t)(pa[i] | pa[i + 1] << 8) - (int)(int16_t)(pb[i] | pb[i + 1] << 8));
                differ += d != 0;
                maxd = d > maxd ? d : maxd;
            }
        }
        free(pa);
        free(pb);
        if (a.sound)
            guest_call(c, F_RELEASE, (const uint32_t[]){a.sound}, 1);
        if (b.sound)
            guest_call(c, F_RELEASE, (const uint32_t[]){b.sound}, 1);
        float fa, fb;
        memcpy(&fa, &a.freq_bits, 4);
        memcpy(&fb, &b.freq_bits, 4);
        fprintf(res, "%s\t%ld\t%u\t%d\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%.0f\t%.0f\t%ld\t%ld\t%.2f\t%.2f\n", name,
                size, a.result, (int)b.result, a.length, b.length, a.type, b.type, a.format, b.format, a.channels,
                b.channels, a.bits, b.bits, fa, fb, differ, maxd, a.ms, b.ms);
        fflush(res);
        const int match = ok && !a.result && !b.result && a.length == b.length && a.format == b.format &&
                          a.channels == b.channels && a.bits == b.bits && fa == fb && differ >= 0;
        same += match;
        bad += ok && !match;
        fmod_ms += a.ms;
        native_ms += ok ? b.ms : a.ms;
        if (n % 50 == 0 || !match)
            SAY("%u/%u %s: %s (FMOD %.1f ms, native %.1f ms, %ld samples differ, at most by %ld)", n, total, name,
                !ok ? "left to FMOD" : match ? "same sample" : "DIFFERENT", a.ms, b.ms, differ, maxd);
    }
    SAY("end: %u files, %u the same sample, %u different or failed, %u done before; FMOD %.0f ms, native %.0f ms",
        total, same, bad, skipped, fmod_ms, native_ms);
    fclose(list);
    fclose(res);
    fclose(start);
    fclose(log);
    return 1;
}
