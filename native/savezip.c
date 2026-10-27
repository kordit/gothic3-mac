/* native/savezip.c - a save's compression with libdeflate (group zlib).
 *
 * Game's save writer (FUN_10aa8210) compresses the world's serialized bytes with its own zlib: deflateInit
 * (10ae2ea0), then deflate (10ae30f0) in a loop into a 32 KB buffer on the stack, each round's output written
 * with fwrite, until a round leaves output space unused; then deflateEnd (10ae3800). That deflate has no other
 * caller and is compiled for Z_FINISH only (it stores last_flush = 4, 10ae313e). Its deflate_slow and longest_match
 * are 12% of a quick save (work/sample-f5-1.txt, session 16).
 *
 * g3n_10ae30f0 (EDI = the z_stream: +0 next_in, +4 avail_in, +8 total_in, +0xc next_out, +0x10 avail_out,
 * +0x14 total_out, +0x30 adler; EAX = the result; plain RET) compresses the whole input in the first call of a
 * stream (total_out 0) with libdeflate's zlib format into a host buffer, then hands it out avail_out bytes a call,
 * returning Z_OK (0) while some is left and Z_STREAM_END (1) once all of it is out. The file is a valid zlib stream
 * with other bytes than the original's (a different level and matcher); the game reads it with inflate as before.
 * The deflate state stays as deflateInit left it, and deflateEnd frees it. */
#include "chunk.h"
#include "g3_opt.h"
#include "third_party/libdeflate.h"

#include <stdlib.h>
#include <string.h>

void fn_10ae30f0(X86 *c); /* Game zlib deflate(strm = EDI, Z_FINISH) */

#define SZ_LEVEL 4

static struct {
    uint32_t strm;
    uint8_t *buf;
    size_t len, pos;
} sz;

static void sz_return(X86 *c, uint32_t eax) {
    const uint32_t esp = c->r[R_ESP];
    c->r[R_EAX] = eax;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4u;
    recomp_return(c);
}

void g3n_10ae30f0(X86 *c) {
    const uint32_t strm = c->r[R_EDI];
    if (!G3OPT(ZLIB) || strm == 0u || rd32(strm + 0x1cu) == 0u || rd32(strm + 0xcu) == 0u ||
        rd32(strm + 0x10u) == 0u) {
        fn_10ae30f0(c);
        return;
    }
    if (rd32(strm + 0x14u) == 0u) { /* a new stream: compress all of it now */
        free(sz.buf);
        sz.buf = 0;
        sz.strm = 0u;
        const uint32_t in = rd32(strm), n = rd32(strm + 4u);
        struct libdeflate_compressor *z = libdeflate_alloc_compressor(SZ_LEVEL);
        const size_t cap = z ? libdeflate_zlib_compress_bound(z, n) : 0;
        uint8_t *buf = cap ? (uint8_t *)malloc(cap) : 0;
        const size_t len = buf ? libdeflate_zlib_compress(z, (const uint8_t *)g_mem + in, n, buf, cap) : 0;
        if (z)
            libdeflate_free_compressor(z);
        if (len == 0) { /* the original's way */
            free(buf);
            fn_10ae30f0(c);
            return;
        }
        sz.strm = strm;
        sz.buf = buf;
        sz.len = len;
        sz.pos = 0;
        wr32(strm, in + n);
        wr32(strm + 4u, 0u);
        wr32(strm + 8u, rd32(strm + 8u) + n);
        wr32(strm + 0x30u, (uint32_t)libdeflate_adler32(1, (const uint8_t *)g_mem + in, n));
    } else if (sz.strm != strm) { /* a stream this did not start */
        fn_10ae30f0(c);
        return;
    }
    const uint32_t out = rd32(strm + 0xcu), room = rd32(strm + 0x10u);
    size_t k = sz.len - sz.pos;
    if (k > room)
        k = room;
    memcpy((uint8_t *)g_mem + out, sz.buf + sz.pos, k);
    sz.pos += k;
    wr32(strm + 0xcu, out + (uint32_t)k);
    wr32(strm + 0x10u, room - (uint32_t)k);
    wr32(strm + 0x14u, rd32(strm + 0x14u) + (uint32_t)k);
    if (sz.pos < sz.len) {
        sz_return(c, 0u);
        return;
    }
    /* All out. The caller calls again only when this round filled its buffer exactly; that call finds total_out set
     * and this stream still recorded, copies nothing and ends the stream again. */
    free(sz.buf);
    sz.buf = 0;
    sz.len = sz.pos = 0;
    sz_return(c, 1u);
}
