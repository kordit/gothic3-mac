/* native/fmodogg.c - an Ogg Vorbis sample decoded natively before FMOD Ex makes it (group fmodogg; D-053,
 * docs/perf/fmod-ogg.md).
 *
 * eCResourceAudioSample_PS::CreateSample (Engine 102ba4b0) has eCAudioSound::Create (102b5f60) make every sound with
 * 3D | SOFTWARE | CREATESAMPLE | ACCURATETIME | IGNORETAGS (0x4150 | 0x2000000): FMOD decodes the whole file into
 * 16-bit PCM inside System::createSound, on the game thread, with the libvorbis linked into fmodex.dll translated like
 * the rest (session 28: 356 ms of one 382 ms freeze on the way to Reddock, the same in two games).
 *
 * The boundary is fmodex's export System::createSound (113bf8d0, stdcall: this, name, mode, exinfo, sound; RET 0x14),
 * the one place Engine makes a sound. For a file FMOD would open by name, with nothing set but what
 * eCAudioSound::Create sets, this reads the file through the same callbacks FMOD would use (the ones
 * eCAudioSystem::Create 102b71d0 hands to System::setFileSystem: open 102b6f60, read 102b6fc0, close 102b70a0), and if
 * it is an Ogg Vorbis stream of one or two channels decodes it here with libvorbisfile 1.3.7 the way FMOD's codec
 * does (ov_read with little endian, 16 bits, signed: fmodex 11373ae0), in pieces of at least two seconds on the
 * performance cores at once (one decoder took 88 ms for the 82 s of sfx_env_fire_01.wav), then gives the PCM to the original
 * createSound as memory: OPENMEMORY | OPENRAW with the length, channels, rate and PCM16 in the CREATESOUNDEXINFO
 * (0x88 bytes, fmodex 11410f90 copies that many). FMOD copies the PCM into its sample and the buffers are freed.
 * The sample has the same length, channels, rate and format as FMOD's own decoding; the samples themselves come from
 * another build of libvorbis and may differ in the last bit (tools/g3fmodcheck.sh compares them for the game's files).
 *
 * Anything else, a file that is not Ogg, more channels (FMOD reorders 6 and 8), a chained stream whose format
 * changes, a read or decode error, and the original runs as before. */
#include "chunk.h"
#include "g3_opt.h"
#include "third_party/vorbis/vorbisfile.h"

#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

void fn_113bf8d0(X86 *c);
uint32_t guest_call(X86 *c, uint32_t fn, const uint32_t *args, int nargs) __asm__("__Z10guest_callP3X86jPKji");
uint32_t heap_alloc(uint32_t size, _Bool zero, uint32_t align) __asm__("__Z10heap_allocjbj");
_Bool heap_free(uint32_t addr) __asm__("__Z9heap_freej");

#define FMOD_3D 0x10u
#define FMOD_CREATESTREAM 0x80u
#define FMOD_CREATESAMPLE 0x100u
#define FMOD_CREATECOMPRESSEDSAMPLE 0x200u
#define FMOD_OPENUSER 0x400u
#define FMOD_OPENMEMORY 0x800u
#define FMOD_OPENRAW 0x1000u
#define FMOD_OPENONLY 0x2000u
#define FMOD_NONBLOCKING 0x10000u
#define FMOD_UNICODE 0x1000000u
#define FMOD_OPENMEMORY_POINT 0x10000000u
#define FMOD_SOUND_FORMAT_PCM16 2u
#define EXINFO_SIZE 0x88u

#define ENG_FILE_OPEN 0x102b6f60u  /* stdcall (name, unicode, &size, &handle, &userdata) */
#define ENG_FILE_READ 0x102b6fc0u  /* stdcall (handle, buffer, bytes, &read, userdata) */
#define ENG_FILE_CLOSE 0x102b70a0u /* stdcall (handle, userdata) */
#define MAX_FILE (64u << 20)

unsigned g3n_fmodogg_native, g3n_fmodogg_original; /* the log and tools/g3fmodcheck.sh */

typedef struct {
    const uint8_t *p;
    size_t n, at;
} Mem;
static size_t m_read(void *dst, size_t size, size_t count, void *src) {
    Mem *m = src;
    size_t want = size * count, left = m->n - m->at;
    if (want > left)
        want = left - left % (size ? size : 1);
    memcpy(dst, m->p + m->at, want);
    m->at += want;
    return size ? want / size : 0;
}
static int m_seek(void *src, ogg_int64_t off, int whence) {
    Mem *m = src;
    const ogg_int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (ogg_int64_t)m->at : (ogg_int64_t)m->n;
    if (base + off < 0 || base + off > (ogg_int64_t)m->n)
        return -1;
    m->at = (size_t)(base + off);
    return 0;
}
static long m_tell(void *src) {
    return (long)((Mem *)src)->at;
}

/* Samples [from, to) of the stream, each part with its own decoder: ov_pcm_seek decodes the packet before `from` for
 * the window's overlap, so a part's samples are the ones a decoder going from the start gives (tools/g3fmodcheck.sh
 * compares the two for every file). */
typedef struct {
    const uint8_t *file;
    size_t size;
    ogg_int64_t from, to;
    int channels, ok;
    uint8_t *out;
} Part;
static void decode_part(void *parts, size_t i) {
    Part *p = (Part *)parts + i;
    Mem m = {p->file, p->size, 0};
    const ov_callbacks cb = {m_read, m_seek, NULL, m_tell};
    OggVorbis_File vf;
    p->ok = 0;
    if (ov_open_callbacks(&m, &vf, NULL, 0, cb) < 0)
        return;
    const size_t want = (size_t)(p->to - p->from) * (size_t)p->channels * 2u;
    size_t have = 0;
    int ok = p->from == 0 || ov_pcm_seek(&vf, p->from) == 0;
    while (ok && have < want) {
        int bs;
        const long n =
            ov_read(&vf, (char *)p->out + have, (int)(want - have < 0x10000 ? want - have : 0x10000), 0, 2, 1, &bs);
        if (n == 0)
            break;
        if (n < 0) {
            if (n == OV_HOLE) /* FMOD's codec reads past a hole the same way: the next call goes on */
                continue;
            ok = 0;
            break;
        }
        have += (size_t)n;
    }
    p->ok = ok && have == want;
    ov_clear(&vf);
}

/* The whole stream as 16-bit little endian signed PCM, as FMOD's codec reads it, in `parts` pieces decoded at once
 * (0: one per performance core, at least two seconds each). 0 when this file is not one this does (not Vorbis, not
 * one or two channels, a format change between chained streams, an error). */
int g3n_fmodogg_decode_parts(const uint8_t *file, size_t size, int parts, uint8_t **pcm, uint32_t *bytes,
                             int *channels, int *rate) {
    if (size < 4 || memcmp(file, "OggS", 4))
        return 0;
    Mem m = {file, size, 0};
    const ov_callbacks cb = {m_read, m_seek, NULL, m_tell};
    OggVorbis_File vf;
    if (ov_open_callbacks(&m, &vf, NULL, 0, cb) < 0)
        return 0;
    const vorbis_info *vi = ov_info(&vf, -1);
    const ogg_int64_t total = ov_pcm_total(&vf, -1);
    int ok = vi && (vi->channels == 1 || vi->channels == 2) && total >= 0 &&
             (uint64_t)total * (uint64_t)vi->channels * 2u <= 0x7fffffffu;
    for (int i = 0; ok && i < ov_streams(&vf); ++i) {
        const vorbis_info *v = ov_info(&vf, i);
        ok = v && v->channels == vi->channels && v->rate == vi->rate;
    }
    const int ch = ok ? vi->channels : 0;
    const long hz = ok ? vi->rate : 0;
    ov_clear(&vf);
    if (!ok)
        return 0;
    if (parts <= 0) {
        static int cores;
        if (!cores) {
            size_t n = sizeof cores;
            if (sysctlbyname("hw.perflevel0.physicalcpu", &cores, &n, NULL, 0) || cores < 1)
                cores = 1;
        }
        const ogg_int64_t most = total / (2 * (ogg_int64_t)(hz > 0 ? hz : 44100));
        parts = most < 1 ? 1 : most < cores ? (int)most : cores;
    }
    const size_t want = (size_t)total * (size_t)ch * 2u;
    uint8_t *out = malloc(want ? want : 1);
    Part part[64];
    if (parts > 64)
        parts = 64;
    if (!out)
        return 0;
    for (int i = 0; i < parts; ++i) {
        const ogg_int64_t from = total * i / parts, to = total * (i + 1) / parts;
        part[i] = (Part){file, size, from, to, ch, 0, out + (size_t)from * (size_t)ch * 2u};
    }
    if (parts == 1)
        decode_part(part, 0);
    else
        dispatch_apply_f((size_t)parts, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), part, decode_part);
    for (int i = 0; i < parts; ++i)
        ok = ok && part[i].ok;
    if (ok) {
        *pcm = out;
        *bytes = (uint32_t)want;
        *channels = ch;
        *rate = (int)hz;
    } else
        free(out);
    return ok;
}

int g3n_fmodogg_decode(const uint8_t *file, size_t size, uint8_t **pcm, uint32_t *bytes, int *channels, int *rate) {
    return g3n_fmodogg_decode_parts(file, size, 0, pcm, bytes, channels, rate);
}

/* The file FMOD would open for `name`, through the game's callbacks, into host memory. */
static uint8_t *read_file(X86 *c, uint32_t name, size_t *size) {
    const uint32_t scr = heap_alloc(16, 1, 16); /* size, handle, userdata, bytes read */
    if (!scr)
        return NULL;
    uint8_t *data = NULL;
    if (guest_call(c, ENG_FILE_OPEN, (const uint32_t[]){name, 0, scr, scr + 4u, scr + 8u}, 5) == 0) {
        const uint32_t n = rd32(scr), handle = rd32(scr + 4u), user = rd32(scr + 8u);
        const uint32_t buf = n >= 4u && n <= MAX_FILE ? heap_alloc(n, 0, 16) : 0;
        if (buf && guest_call(c, ENG_FILE_READ, (const uint32_t[]){handle, buf, n, scr + 12u, user}, 5) == 0 &&
            rd32(scr + 12u) == n && (data = malloc(n)) != NULL) {
            memcpy(data, g_mem + buf, n);
            *size = n;
        }
        if (buf)
            heap_free(buf);
        guest_call(c, ENG_FILE_CLOSE, (const uint32_t[]){handle, user}, 2);
    }
    heap_free(scr);
    return data;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* FMOD::System::createSound(char const *, unsigned int, FMOD_CREATESOUNDEXINFO *, FMOD::Sound **) */
void g3n_113bf8d0(X86 *c) {
    const uint32_t sp = c->r[4];
    const uint32_t name = rd32(sp + 8u), mode = rd32(sp + 12u), exinfo = rd32(sp + 16u);
    const uint32_t other = FMOD_CREATESTREAM | FMOD_CREATECOMPRESSEDSAMPLE | FMOD_OPENUSER | FMOD_OPENMEMORY |
                           FMOD_OPENRAW | FMOD_OPENONLY | FMOD_NONBLOCKING | FMOD_UNICODE | FMOD_OPENMEMORY_POINT;
    if (!G3OPT(FMODOGG) || !name || exinfo || !(mode & FMOD_CREATESAMPLE) || (mode & other)) {
        fn_113bf8d0(c);
        return;
    }
    const double t0 = now_ms();
    size_t size = 0;
    uint8_t *file = read_file(c, name, &size), *pcm = NULL;
    uint32_t bytes = 0;
    int channels = 0, rate = 0;
    const int ok = file && g3n_fmodogg_decode(file, size, &pcm, &bytes, &channels, &rate);
    free(file);
    const uint32_t gpcm = ok ? heap_alloc(bytes ? bytes : 1u, 0, 16) : 0;
    const uint32_t ex = gpcm ? heap_alloc(EXINFO_SIZE, 1, 16) : 0;
    if (!ex) {
        if (gpcm)
            heap_free(gpcm);
        free(pcm);
        ++g3n_fmodogg_original;
        fn_113bf8d0(c);
        return;
    }
    memcpy(g_mem + gpcm, pcm, bytes);
    free(pcm);
    wr32(ex + 0x00u, EXINFO_SIZE);             /* cbsize */
    wr32(ex + 0x04u, bytes);                   /* length */
    wr32(ex + 0x0cu, (uint32_t)channels);      /* numchannels */
    wr32(ex + 0x10u, (uint32_t)rate);          /* defaultfrequency */
    wr32(ex + 0x14u, FMOD_SOUND_FORMAT_PCM16); /* format */
    wr32(sp + 8u, gpcm);
    wr32(sp + 12u, mode | FMOD_OPENMEMORY | FMOD_OPENRAW);
    wr32(sp + 16u, ex);
    const double t1 = now_ms();
    fn_113bf8d0(c);
    heap_free(gpcm);
    heap_free(ex);
    ++g3n_fmodogg_native;
    const double t2 = now_ms();
    if (t2 - t0 >= 5.0) {
        char what[160];
        size_t i = 0;
        for (; i + 1 < sizeof what && g_mem[name + i]; ++i)
            what[i] = (char)g_mem[name + i];
        what[i] = 0;
        fprintf(stderr, "[g3] fmodogg: %s: %u bytes of PCM (%d ch, %d Hz) in %.1f ms, FMOD's copy %.1f ms\n", what,
                bytes, channels, rate, t1 - t0, t2 - t1);
    }
}
