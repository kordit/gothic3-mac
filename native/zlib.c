/* native/zlib.c - FileSystem.dll's static zlib 1.2.5 inflate, natively (group zlib).
 *
 * Every archive read goes through it: eCImageResource::DoLoad -> eCVirtualFile::Open -> FileSystem
 * CreateFileA 10da1c30 -> ... -> 10da33c0, which inflates a packed file in 128 KB input chunks into
 * one output buffer as big as the file (one inflate() call per chunk). With World Texture Mix 20% of
 * the main thread in a fight is that, almost all of it in inflate_fast; the same path loads the world
 * and streams it while running.
 *
 * What the listings show (analysis/FileSystem.dll, "inflate 1.2.5 Copyright 1995-2010 Mark Adler"):
 *   10daa430 inflate(strm [ESP+4]), RET 4: flush specialised away (no Z_FINISH, Z_BLOCK or Z_TREES
 *            test is left: TYPE and TYPEDO share one jump-table case, the end only maps "no progress"
 *            to Z_BUF_ERROR). Mode table at 10daba00, 31 cases, the 1.2.5 enum (HEAD 0 .. MEM 30).
 *   10dad830 inflate_fast(strm, start), RET 8 (1.2.5, PUP pre-increment)
 *   10dadff0 inflate_table(lens ECX, bits EDX, type, codes, table, work), RET 0x10 (1.2.5: the
 *            "used >= ENOUGH" checks, the incomplete-code fill loop; lbase/lext 10dbe698/10dbe6d8,
 *            dbase/dext 10dbe718/10dbe758)
 *   10daa340 updatewindow(strm [ESP+4], out EAX), RET 4: the only guest call (zalloc of the window)
 *   10dad5b0 adler32(adler ECX, buf EDX, len EAX); 10dadd20 crc32, 10dadd00 its NULL wrapper
 *   10daa2c0 inflateInit_(strm EAX), windowBits 15 inlined; 10daa270 inflateReset2(15), 10daa200
 *            inflateReset; 10daba90 inflateEnd(strm ESI); zalloc 10dadfc0 = malloc, zfree 10dadfe0
 *   lenfix 10dc0890 / distfix 10dc1090 (fixedtables), order[] 10dc1110, messages 10dc1cbc..10dc1e8c
 *   z_stream (0x38): next_in 0, avail_in 4, total_in 8, next_out c, avail_out 10, total_out 14,
 *            msg 18, state 1c, zalloc 20, zfree 24, opaque 28, data_type 2c, adler 30
 *   inflate_state (0x1bcc, inflateInit_'s zalloc): the 1.2.5 layout, below.
 *
 * g3n_10daa430 is 1.2.5's inflate, inflate_fast, inflate_table and adler32 working in place on the
 * guest's z_stream, state, window and code tables, so guest memory and EAX ECX EDX EBX ESI EDI EBP
 * ESP EIP end as the translation's (ECX and EDX are what the original's data_type arithmetic leaves
 * there). The order of input reads and the fast loop's exit points are 1.2.5's to the byte: which codes
 * the slow path decodes decides length/offset/extra/back/was at the end. The one guest call stays a
 * guest call: when the window is still unallocated the translated updatewindow runs from the
 * original's registers and stack, so zalloc sees the same call. Everything else falls back to the
 * translated inflate before touching anything: the group off, a store hook armed (DirectDraw Lock
 * dirty ranges, RECOMP_WATCH), the Z_STREAM_ERROR checks, gzip (wrap & 2; the game never sets it,
 * inflateInit_ fixes wrap = 1), a null-page or wrapping buffer, or buffers that overlap each other
 * or the stream or state (the original's order of loads and stores would then matter). */
#include "chunk.h"
#include "g3_opt.h"

#include <stddef.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

void fn_10daa430(X86 *c); /* inflate */
void fn_10daa340(X86 *c); /* updatewindow */

#ifndef ZLIB_MUT
#define ZLIB_MUT 0 /* native/tests/zlib_test.c's mutation runs: 0 = none */
#endif

#define ZI_RET_UPDATEWINDOW 0x10dab912u /* the continuation of inflate's CALL updatewindow */
#define ZI_LENFIX 0x10dc0890u
#define ZI_DISTFIX 0x10dc1090u
#define ZI_LBASE 0x10dbe698u
#define ZI_LEXT 0x10dbe6d8u
#define ZI_DBASE 0x10dbe718u
#define ZI_DEXT 0x10dbe758u

/* strm->msg */
#define M_HEADER_CHECK 0x10dc1cbcu  /* incorrect header check */
#define M_METHOD 0x10dc1cd4u        /* unknown compression method */
#define M_WINDOW_SIZE 0x10dc1cf0u   /* invalid window size */
#define M_BLOCK_TYPE 0x10dc1d34u    /* invalid block type */
#define M_STORED_LENS 0x10dc1d48u   /* invalid stored block lengths */
#define M_TOO_MANY 0x10dc1d68u      /* too many length or distance symbols */
#define M_CODE_LENS 0x10dc1d8cu     /* invalid code lengths set */
#define M_REPEAT 0x10dc1da8u        /* invalid bit length repeat */
#define M_NO_EOB 0x10dc1dc4u        /* invalid code -- missing end-of-block */
#define M_LITLEN_SET 0x10dc1decu    /* invalid literal/lengths set */
#define M_DIST_SET 0x10dc1e08u      /* invalid distances set */
#define M_LITLEN_CODE 0x10dc1e20u   /* invalid literal/length code */
#define M_DIST_CODE 0x10dc1e3cu     /* invalid distance code */
#define M_TOO_FAR 0x10dc1e54u       /* invalid distance too far back */
#define M_DATA_CHECK 0x10dc1e74u    /* incorrect data check */

enum {
    HEAD, FLAGS, TIME, OS, EXLEN, EXTRA, NAME, COMMENT, HCRC, DICTID, DICT,
    TYPE, TYPEDO, STORED, COPY_, COPY, TABLE, LENLENS, CODELENS,
    LEN_, LEN, LENEXT, DIST, DISTEXT, MATCH, LIT,
    CHECK, LENGTH, DONE, BAD, MEM, SYNC
};
enum { Z_OK = 0, Z_STREAM_END = 1, Z_NEED_DICT = 2, Z_DATA_ERROR = -3, Z_MEM_ERROR = -4, Z_BUF_ERROR = -5 };
enum { CODES, LENS, DISTS };
#define ENOUGH_LENS 852
#define ENOUGH_DISTS 592
#define MAXBITS 15

typedef struct {
    uint8_t op, bits;
    uint16_t val;
} zcode;

typedef struct {
    uint32_t next_in, avail_in, total_in, next_out, avail_out, total_out, msg, state;
    uint32_t zalloc, zfree, opaque, data_type, adler, reserved;
} zstream;

typedef struct {
    uint32_t mode, last, wrap, havedict, flags, dmax, check, total, head;
    uint32_t wbits, wsize, whave, wnext, window, hold, bits, length, offset, extra;
    uint32_t lencode, distcode, lenbits, distbits, ncode, nlen, ndist, have, next;
    uint16_t lens[320], work[288];
    zcode codes[852 + 592];
    int32_t sane, back;
    uint32_t was;
} zstate;

_Static_assert(sizeof(zstream) == 0x38, "z_stream");
_Static_assert(offsetof(zstate, window) == 0x34 && offsetof(zstate, lencode) == 0x4c, "inflate_state");
_Static_assert(offsetof(zstate, next) == 0x6c && offsetof(zstate, lens) == 0x70, "inflate_state");
_Static_assert(offsetof(zstate, work) == 0x2f0 && offsetof(zstate, codes) == 0x530, "inflate_state");
_Static_assert(offsetof(zstate, sane) == 0x1bc0 && sizeof(zstate) == 0x1bcc, "inflate_state");

static inline uint8_t *H(uint32_t a) {
    return g_mem + a;
}
static inline uint32_t G(const uint8_t *p) {
    return (uint32_t)(p - g_mem);
}

/* ---- adler32 (10dad5b0): (a, b) mod 65521; every form of 1.2.5's gives the same value ---- */

#define BASE 65521u
#define NMAX 5552u

static uint32_t adler32_n(uint32_t adler, const uint8_t *buf, uint32_t len) {
    uint32_t a = adler & 0xffffu, b = (adler >> 16) & 0xffffu;
    if (len == 1u) { /* 1.2.5's single-byte path */
        a += buf[0];
        if (a >= BASE)
            a -= BASE;
        b += a;
        if (b >= BASE)
            b -= BASE;
        return a | (b << 16);
    }
    if (len < 16u) {
        while (len--) {
            a += *buf++;
            b += a;
        }
        if (a >= BASE)
            a -= BASE;
        b %= BASE;
        return a | (b << 16);
    }
#if defined(__aarch64__)
    /* NEON: 32 bytes a step. For a block of n bytes x_i: b += n*a + sum (n - i) x_i, a += sum x_i. */
    static const uint16_t taps[32] = {32 - (ZLIB_MUT == 5), 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
                                      16, 15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1};
    const uint16x8_t t0 = vld1q_u16(taps), t1 = vld1q_u16(taps + 8), t2 = vld1q_u16(taps + 16),
                     t3 = vld1q_u16(taps + 24);
    while (len >= 32u) {
        /* at most NMAX bytes between reductions (5552 = 173 * 32 + 16) */
        uint32_t n = len < NMAX ? len : NMAX;
        n &= ~31u;
        len -= n;
        uint32_t blocks = n / 32u;
        uint32x4_t vs1 = vdupq_n_u32(0), vs2 = vdupq_n_u32(0), vps = vdupq_n_u32(0);
        uint16x8_t c0 = vdupq_n_u16(0), c1 = c0, c2 = c0, c3 = c0;
        const uint32_t a0 = a;
        do {
            const uint8x16_t d0 = vld1q_u8(buf), d1 = vld1q_u8(buf + 16);
            buf += 32;
            vps = vaddq_u32(vps, vs1); /* a before this block, summed per block */
            const uint16x8_t s = vpaddlq_u8(d0);
            const uint16x8_t s2 = vpadalq_u8(s, d1);
            vs1 = vpadalq_u16(vs1, s2);
            c0 = vaddw_u8(c0, vget_low_u8(d0));
            c1 = vaddw_u8(c1, vget_high_u8(d0));
            c2 = vaddw_u8(c2, vget_low_u8(d1));
            c3 = vaddw_u8(c3, vget_high_u8(d1));
        } while (--blocks);
        /* b gains 32 * (sum of the bytes before each block) + the tapped column sums + n * a0 */
        vs2 = vshlq_n_u32(vps, 5);
        vs2 = vmlal_u16(vs2, vget_low_u16(c0), vget_low_u16(t0));
        vs2 = vmlal_high_u16(vs2, c0, t0);
        vs2 = vmlal_u16(vs2, vget_low_u16(c1), vget_low_u16(t1));
        vs2 = vmlal_high_u16(vs2, c1, t1);
        vs2 = vmlal_u16(vs2, vget_low_u16(c2), vget_low_u16(t2));
        vs2 = vmlal_high_u16(vs2, c2, t2);
        vs2 = vmlal_u16(vs2, vget_low_u16(c3), vget_low_u16(t3));
        vs2 = vmlal_high_u16(vs2, c3, t3);
        const uint64_t sb = (uint64_t)vaddvq_u32(vs2) + (uint64_t)n * a0 + b;
        a = (a0 + vaddvq_u32(vs1)) % BASE;
        b = (uint32_t)(sb % BASE);
    }
#endif
    while (len >= NMAX) {
        len -= NMAX;
        for (uint32_t n = NMAX; n; --n) {
            a += *buf++;
            b += a;
        }
        a %= BASE;
        b %= BASE;
    }
    while (len--) {
        a += *buf++;
        b += a;
    }
    a %= BASE;
    b %= BASE;
    return a | (b << 16);
}

/* ---- inflate_table (10dadff0), 1.2.5 ---- */

static int inflate_table(int type, const uint16_t *lens, unsigned codes, uint32_t *table_g, uint32_t *bits,
                         uint16_t *work) {
    unsigned len, sym, min, max, root, curr, drop, used, huff, incr, fill, low, mask;
    int left;
    zcode here;
    zcode *next;
    uint32_t base_g, extra_g; /* guest addresses of base[0], extra[0] (biased as 1.2.5's) */
    int end;
    uint16_t count[MAXBITS + 1], offs[MAXBITS + 1];

    for (len = 0; len <= MAXBITS; len++)
        count[len] = 0;
    for (sym = 0; sym < codes; sym++)
        count[lens[sym]]++;

    root = *bits;
    for (max = MAXBITS; max >= 1; max--)
        if (count[max] != 0)
            break;
    if (root > max)
        root = max;
    if (max == 0) {
        here.op = 64;
        here.bits = 1;
        here.val = 0;
        zcode *t = (zcode *)H(*table_g);
        t[0] = here;
        t[1] = here;
        *table_g += 8u;
        *bits = 1;
        return 0;
    }
    for (min = 1; min < max; min++)
        if (count[min] != 0)
            break;
    if (root < min)
        root = min;

    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= count[len];
        if (left < 0)
            return -1;
    }
    if (left > 0 && (type == CODES || max != 1))
        return -1;

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = (uint16_t)(offs[len] + count[len]);
    for (sym = 0; sym < codes; sym++)
        if (lens[sym] != 0)
            work[offs[lens[sym]]++] = (uint16_t)sym;

    switch (type) {
    case CODES:
        base_g = extra_g = 0; /* not used: every symbol is below end */
        end = 19;
        break;
    case LENS:
        base_g = ZI_LBASE - 2u * 257u;
        extra_g = ZI_LEXT - 2u * 257u;
        end = 256;
        break;
    default:
        base_g = ZI_DBASE;
        extra_g = ZI_DEXT;
        end = -1;
    }

    huff = 0;
    sym = 0;
    len = min;
    zcode *const table = (zcode *)H(*table_g);
    next = table;
    curr = root;
    drop = 0;
    low = (unsigned)-1;
    used = 1u << root;
    mask = used - 1;

    if ((type == LENS && used >= ENOUGH_LENS) || (type == DISTS && used >= ENOUGH_DISTS))
        return 1;

    for (;;) {
        here.bits = (uint8_t)(len - drop);
        const int w = (int)work[sym];
        if (w < end) {
            here.op = 0;
            here.val = work[sym];
        } else if (w > end) {
            here.op = (uint8_t)rd16(extra_g + 2u * (uint32_t)w);
            here.val = rd16(base_g + 2u * (uint32_t)w);
        } else {
            here.op = 32 + 64;
            here.val = 0;
        }

        incr = 1u << (len - drop);
        fill = 1u << curr;
        min = fill;
        do {
            fill -= incr;
            next[(huff >> drop) + fill] = here;
        } while (fill != 0);

        incr = 1u << (len - 1);
        while (huff & incr)
            incr >>= 1;
        if (incr != 0) {
            huff &= incr - 1;
            huff += incr;
        } else
            huff = 0;

        sym++;
        if (--(count[len]) == 0) {
            if (len == max)
                break;
            len = lens[work[sym]];
        }

        if (len > root && (huff & mask) != low) {
            if (drop == 0)
                drop = root;
            next += min;
            curr = len - drop;
            left = (int)(1 << curr);
            while (curr + drop < max) {
                left -= count[curr + drop];
                if (left <= 0)
                    break;
                curr++;
                left <<= 1;
            }
            used += 1u << curr;
            if ((type == LENS && used >= ENOUGH_LENS) || (type == DISTS && used >= ENOUGH_DISTS))
                return 1;
            low = huff & mask;
#if ZLIB_MUT == 9
            curr += curr == 7;
#endif
            table[low].op = (uint8_t)curr;
            table[low].bits = (uint8_t)root;
            table[low].val = (uint16_t)(next - table);
        }
    }

    /* 1.2.5: fill in the rest of the table for an incomplete code */
    here.op = 64;
    here.bits = (uint8_t)(len - drop);
    here.val = 0;
    while (huff != 0) {
        if (drop != 0 && (huff & mask) != low) {
            drop = 0;
            len = root;
            next = table;
            here.bits = (uint8_t)len;
        }
        next[huff >> drop] = here;
        incr = 1u << (len - 1);
        while (huff & incr)
            incr >>= 1;
        if (incr != 0) {
            huff &= incr - 1;
            huff += incr;
        } else
            huff = 0;
    }

    *table_g += used * 4u;
    *bits = root;
    return 0;
}

/* ---- inflate_fast (10dad830), 1.2.5 ---- */

/* `n` bytes from `from` to `out`, forward one byte at a time as the original: a source that runs
 * into the destination repeats the pattern. */
static inline void copy16(uint8_t *out, const uint8_t *from) {
    uint8_t t[16];
    memcpy(t, from, 16);
    memcpy(out, t, 16);
}
static inline void copy8(uint8_t *out, const uint8_t *from) {
    uint64_t t;
    memcpy(&t, from, 8);
    memcpy(out, &t, 8);
}
static inline void lz_copy(uint8_t *out, const uint8_t *from, unsigned n) {
    /* out - from: a source in another region (the window) wraps to a huge distance */
    const uintptr_t d = (uintptr_t)out - (uintptr_t)from;
    if (d >= 16u) { /* every 16-byte chunk reads only bytes already final */
        while (n >= 16u) {
            copy16(out, from);
            out += 16;
            from += 16;
            n -= 16u;
        }
        if (n >= 8u) {
            copy8(out, from);
            out += 8;
            from += 8;
            n -= 8u;
        }
    } else if (d == 1u) {
        memset(out, *from, n);
        return;
    } else if (d >= 8u) {
        while (n >= 8u) {
            copy8(out, from);
            out += 8;
            from += 8;
            n -= 8u;
        }
    }
    while (n--)
        *out++ = *from++;
}

typedef struct {
    const uint8_t *next;
    uint8_t *put;
    unsigned have, left;
    uint32_t hold;
    unsigned bits;
} zio;

/* `start` is inflate's avail_out on entry; beg the output pointer then. */
static inline uint32_t ent(const uint8_t *table, uint32_t i) {
    uint32_t e;
    memcpy(&e, table + 4u * i, 4);
    return e;
}

static void inflate_fast(zstream *strm, zstate *st, zio *io, unsigned start) {
    const uint8_t *in = io->next;
    const uint8_t *const last = in + (io->have - 5u);
    uint8_t *out = io->put;
    const uint8_t *const beg = out - (start - io->left);
    uint8_t *const end = out + (io->left - 257u);
    const unsigned wsize = st->wsize, whave = st->whave, wnext = st->wnext;
    const uint8_t *const window = H(st->window);
    uint32_t hold = io->hold;
    unsigned bits = io->bits;
    const uint8_t *const lcode = H(st->lencode);
    const uint8_t *const dcode = H(st->distcode);
    const unsigned lmask = (1u << st->lenbits) - 1u, dmask = (1u << st->distbits) - 1u;
    uint32_t here; /* a code entry as one word: op, bits, val from the low byte up */
    unsigned op, len, dist;
    const uint8_t *from;

    do {
        if (bits < 15) {
            hold += (uint32_t)(*in++) << bits;
            bits += 8;
            hold += (uint32_t)(*in++) << bits;
            bits += 8;
        }
        here = ent(lcode, hold & lmask);
    dolen:
        op = (here >> 8) & 0xffu;
        hold >>= op;
        bits -= op;
        op = here & 0xffu;
        if (op == 0) {
            *out++ = (uint8_t)(here >> 16);
        } else if (op & 16) {
            len = here >> 16;
            op &= 15;
            if (op) {
                if (bits < op) {
                    hold += (uint32_t)(*in++) << bits;
                    bits += 8;
                }
                len += (unsigned)hold & ((1u << op) - 1u);
                hold >>= op;
                bits -= op;
            }
            if (bits < 15) {
                hold += (uint32_t)(*in++) << bits;
                bits += 8;
                hold += (uint32_t)(*in++) << bits;
                bits += 8;
            }
            here = ent(dcode, hold & dmask);
        dodist:
            op = (here >> 8) & 0xffu;
            hold >>= op;
            bits -= op;
            op = here & 0xffu;
            if (op & 16) {
                dist = here >> 16;
                op &= 15;
                if (bits < op) {
                    hold += (uint32_t)(*in++) << bits;
                    bits += 8;
                    if (bits < op) {
                        hold += (uint32_t)(*in++) << bits;
                        bits += 8;
                    }
                }
                dist += (unsigned)hold & ((1u << op) - 1u);
                hold >>= op;
                bits -= op;
                op = (unsigned)(out - beg);
                if (dist > op) {
                    op = dist - op;
                    if (op > whave) {
                        if (st->sane) {
                            strm->msg = M_TOO_FAR;
                            st->mode = BAD;
                            break;
                        }
                    }
                    from = window;
                    if (wnext == 0) {
                        from += wsize - op;
                        if (op < len) {
                            len -= op;
                            lz_copy(out, from, op);
                            out += op;
                            from = out - dist;
                        }
                    } else if (wnext < op) {
                        from += wsize + wnext - op;
                        op -= wnext;
                        if (op < len) {
                            len -= op;
                            lz_copy(out, from, op);
                            out += op;
                            from = window;
                            if (wnext < len) {
                                op = wnext;
                                len -= op;
                                lz_copy(out, from, op);
                                out += op;
                                from = out - dist;
                            }
                        }
                    } else {
                        from += wnext - op;
                        if (op < len) {
                            len -= op;
                            lz_copy(out, from, op);
                            out += op;
                            from = out - dist;
                        }
                    }
                    /* the rest: from the window (no overlap) or from the output */
                    lz_copy(out, from, len);
                    out += len;
                } else {
                    from = out - dist;
#if ZLIB_MUT == 1
                    if (len > 3)
                        --len;
#endif
                    lz_copy(out, from, len);
                    out += len;
                }
            } else if ((op & 64) == 0) {
                here = ent(dcode, (here >> 16) + (hold & ((1u << op) - 1u)));
                goto dodist;
            } else {
                strm->msg = M_DIST_CODE;
                st->mode = BAD;
                break;
            }
        } else if ((op & 64) == 0) {
            here = ent(lcode, (here >> 16) + (hold & ((1u << op) - 1u)));
            goto dolen;
        } else if (op & 32) {
            st->mode = TYPE;
            break;
        } else {
            strm->msg = M_LITLEN_CODE;
            st->mode = BAD;
            break;
        }
#if ZLIB_MUT == 6
    } while (in <= last && out < end);
#else
    } while (in < last && out < end);
#endif

    /* return unused bytes (on entry bits < 8, so in won't go too far back) */
    len = bits >> 3;
    in -= len;
    bits -= len << 3;
    hold &= (1u << bits) - 1u;

    io->next = in;
    io->put = out;
    io->have = (unsigned)(in < last ? 5u + (last - in) : 5u - (in - last));
    io->left = (unsigned)(out < end ? 257u + (end - out) : 257u - (out - end));
    io->hold = hold;
    io->bits = bits;
}

/* ---- updatewindow (10daa340) with the window already there ---- */

static void updatewindow_n(zstream *strm, zstate *st, unsigned out) {
    uint8_t *const window = H(st->window);
    if (st->wsize == 0) {
        st->wsize = 1u << (st->wbits & 31u);
        st->wnext = 0;
        st->whave = 0;
    }
    unsigned copy = out - strm->avail_out;
    const uint8_t *const next_out = H(strm->next_out);
    if (copy >= st->wsize) {
        memcpy(window, next_out - st->wsize, st->wsize);
        st->wnext = 0;
        st->whave = st->wsize;
    } else {
        unsigned dist = st->wsize - st->wnext;
        if (dist > copy)
            dist = copy;
        memcpy(window + st->wnext, next_out - copy, dist);
        copy -= dist;
        if (copy) {
            memcpy(window, next_out - copy, copy);
            st->wnext = copy;
            st->whave = st->wsize;
        } else {
            st->wnext += dist;
            if (st->wnext == st->wsize)
                st->wnext = 0;
            if (st->whave < st->wsize && ZLIB_MUT != 4)
                st->whave += dist;
        }
    }
}

/* ---- inflate (10daa430) ---- */

/* [lo, lo + n) inside the guest's 4 GB, off the null page when the build checks it */
static inline int span_ok(uint32_t lo, uint32_t n) {
    return n == 0u || ((uint64_t)lo + n <= 0x100000000ull && RECOMP_STRING_FAST_ADDR(lo));
}
static inline int apart(uint32_t a, uint32_t na, uint32_t b, uint32_t nb) {
    return na == 0u || nb == 0u || (uint64_t)a + na <= b || (uint64_t)b + nb <= a;
}

/* inflate's own end: RET 4 with EAX = ret and the original's ECX/EDX. */
static inline void zi_return(X86 *c, uint32_t esp, int32_t ret, uint32_t ecx, uint32_t edx) {
    c->r[R_EAX] = (uint32_t)ret;
    c->r[R_ECX] = ecx;
    c->r[R_EDX] = edx;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 8u;
    recomp_return(c);
}

void g3n_10daa430(X86 *c) {
    const uint32_t esp = c->r[R_ESP];
    const uint32_t sg = rd32(esp + 4u);
    if (!G3OPT(ZLIB) || !RECOMP_STRING_FAST_STORES() || sg == 0u || (sg & 3u) || !span_ok(sg, 0x38u)) {
        fn_10daa430(c);
        return;
    }
    zstream *const strm = (zstream *)H(sg);
    const uint32_t stg = strm->state;
    if (stg == 0u || (stg & 3u) || !span_ok(stg, 0x1bccu) || strm->next_out == 0u ||
        (strm->next_in == 0u && strm->avail_in != 0u)) {
        fn_10daa430(c);
        return;
    }
    zstate *const st = (zstate *)H(stg);
    const uint32_t mode0 = st->mode;
    if (mode0 > MEM || (mode0 >= FLAGS && mode0 <= HCRC) || (st->wrap & 2u) || st->sane == 0 ||
        (st->wrap != 0u && mode0 != HEAD && st->flags != 0u)) {
        fn_10daa430(c);
        return;
    }
    const uint32_t wn = st->window ? 1u << (st->wbits & 31u) : 0u;
    const uint32_t ig = strm->next_in, in_n = strm->avail_in, og = strm->next_out, out_n = strm->avail_out;
    if (!span_ok(ig, in_n) || !span_ok(og, out_n) || !span_ok(st->window, wn) || !apart(og, out_n, ig, in_n) ||
        !apart(og, out_n, sg, 0x38u) || !apart(og, out_n, stg, 0x1bccu) || !apart(og, out_n, st->window, wn) ||
        !apart(ig, in_n, stg, 0x1bccu) || !apart(ig, in_n, sg, 0x38u) || !apart(sg, 0x38u, stg, 0x1bccu) ||
        !apart(st->window, wn, stg, 0x1bccu) || !apart(st->window, wn, sg, 0x38u)) {
        fn_10daa430(c);
        return;
    }

    if (mode0 == MEM) { /* case MEM: return Z_MEM_ERROR, ECX = avail_out, EDX = avail_in */
        zi_return(c, esp, Z_MEM_ERROR, out_n, in_n);
        return;
    }

    const uint8_t *next;
    uint8_t *put;
    unsigned have, left, bits;
    uint32_t hold;
    unsigned in, out, copy, len;
    const uint8_t *from;
    zcode here, last;
    int32_t ret;

    if (st->mode == TYPE)
        st->mode = TYPEDO;
    put = H(og);
    left = out_n;
    next = H(ig);
    have = in_n;
    hold = st->hold;
    bits = st->bits;
    in = have;
    out = left;
    ret = Z_OK;

#define PULLBYTE()                                                                                 \
    do {                                                                                           \
        if (have == 0)                                                                             \
            goto inf_leave;                                                                        \
        have--;                                                                                    \
        hold += (uint32_t)(*next++) << bits;                                                       \
        bits += 8;                                                                                 \
    } while (0)
#define NEEDBITS(n)                                                                                \
    do {                                                                                           \
        while (bits < (unsigned)(n))                                                               \
            PULLBYTE();                                                                            \
    } while (0)
#define BITS(n) ((unsigned)hold & ((1u << (n)) - 1u))
#define DROPBITS(n)                                                                                \
    do {                                                                                           \
        hold >>= (n);                                                                              \
        bits -= (unsigned)(n);                                                                     \
    } while (0)
#define INITBITS() (hold = 0, bits = 0)
#define BYTEBITS()                                                                                 \
    do {                                                                                           \
        hold >>= bits & 7u;                                                                        \
        bits -= bits & 7u;                                                                         \
    } while (0)
#define REVERSE(q) ((((q) >> 24) & 0xff) + (((q) >> 8) & 0xff00) + (((q) & 0xff00) << 8) + (((q) & 0xff) << 24))

    for (;;)
        switch (st->mode) {
        case HEAD:
            if (st->wrap == 0) {
                st->mode = TYPEDO;
                break;
            }
            NEEDBITS(16);
            /* wrap & 2 (gzip) went to the original */
            st->flags = 0;
            if (st->head != 0u)
                wr32(st->head + 0x30u, 0xffffffffu);
            if (!(st->wrap & 1u) || ((BITS(8) << 8) + (hold >> 8)) % 31u) {
                strm->msg = M_HEADER_CHECK;
                st->mode = BAD;
                break;
            }
            if (BITS(4) != 8u) {
                strm->msg = M_METHOD;
                st->mode = BAD;
                break;
            }
            DROPBITS(4);
            len = BITS(4) + 8u;
            if (st->wbits == 0)
                st->wbits = len;
            else if (len > st->wbits) {
                strm->msg = M_WINDOW_SIZE;
                st->mode = BAD;
                break;
            }
            st->dmax = 1u << len;
            strm->adler = st->check = 1u; /* adler32(0, NULL, 0) */
            st->mode = hold & 0x200u ? DICTID : TYPE;
            INITBITS();
            break;
        case DICTID:
            NEEDBITS(32);
            strm->adler = st->check = REVERSE(hold);
            INITBITS();
            st->mode = DICT;
            /* fallthrough */
        case DICT:
            if (st->havedict == 0) {
                strm->next_out = G(put);
                strm->avail_out = left;
                strm->next_in = G(next);
                strm->avail_in = have;
                st->hold = hold;
                st->bits = bits;
                zi_return(c, esp, Z_NEED_DICT, left, have);
                return;
            }
            strm->adler = st->check = 1u;
            st->mode = TYPE;
            /* fallthrough */
        case TYPE:
        case TYPEDO:
            if (st->last) {
                BYTEBITS();
                st->mode = CHECK;
                break;
            }
            NEEDBITS(3);
            st->last = BITS(1);
            DROPBITS(1);
            switch (BITS(2)) {
            case 0:
                st->mode = STORED;
                break;
            case 1: /* fixedtables */
                st->lencode = ZI_LENFIX;
                st->lenbits = 9;
                st->distcode = ZI_DISTFIX;
                st->distbits = 5;
                st->mode = LEN_;
                break;
            case 2:
                st->mode = TABLE;
                break;
            case 3:
                strm->msg = M_BLOCK_TYPE;
                st->mode = BAD;
            }
            DROPBITS(2);
            break;
        case STORED:
            BYTEBITS();
            NEEDBITS(32);
            if ((hold & 0xffffu) != ((hold >> 16) ^ 0xffffu)) {
                strm->msg = M_STORED_LENS;
                st->mode = BAD;
                break;
            }
            st->length = hold & 0xffffu;
            INITBITS();
            st->mode = COPY_;
            /* fallthrough */
        case COPY_:
            st->mode = COPY;
            /* fallthrough */
        case COPY:
            copy = st->length;
            if (copy) {
                if (copy > have)
                    copy = have;
                if (copy > left)
                    copy = left;
                if (copy == 0)
                    goto inf_leave;
                memcpy(put, next, copy);
                have -= copy;
                next += copy;
                left -= copy;
                put += copy;
                st->length -= copy;
                break;
            }
            st->mode = TYPE;
            break;
        case TABLE:
            NEEDBITS(14);
            st->nlen = BITS(5) + 257u;
            DROPBITS(5);
            st->ndist = BITS(5) + 1u;
            DROPBITS(5);
            st->ncode = BITS(4) + 4u;
            DROPBITS(4);
            if (st->nlen > 286u || st->ndist > 30u) {
                strm->msg = M_TOO_MANY;
                st->mode = BAD;
                break;
            }
            st->have = 0;
            st->mode = LENLENS;
            /* fallthrough */
        case LENLENS: {
            static const uint16_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            while (st->have < st->ncode) {
                NEEDBITS(3);
                st->lens[order[st->have++]] = (uint16_t)BITS(3);
                DROPBITS(3);
            }
            while (st->have < 19u)
                st->lens[order[st->have++]] = 0;
            st->next = stg + (uint32_t)offsetof(zstate, codes);
            st->lencode = st->next;
            st->lenbits = 7;
            ret = inflate_table(CODES, st->lens, 19, &st->next, &st->lenbits, st->work);
            if (ret) {
                strm->msg = M_CODE_LENS;
                st->mode = BAD;
                break;
            }
            st->have = 0;
            st->mode = CODELENS;
        }
            /* fallthrough */
        case CODELENS:
            while (st->have < st->nlen + st->ndist) {
                const zcode *const lc = (const zcode *)H(st->lencode);
                for (;;) {
                    here = lc[BITS(st->lenbits)];
                    if ((unsigned)here.bits <= bits)
                        break;
                    PULLBYTE();
                }
                if (here.val < 16) {
                    NEEDBITS(here.bits);
                    DROPBITS(here.bits);
                    st->lens[st->have++] = here.val;
                } else {
                    if (here.val == 16) {
                        NEEDBITS(here.bits + 2u);
                        DROPBITS(here.bits);
                        if (st->have == 0) {
                            strm->msg = M_REPEAT;
                            st->mode = BAD;
                            break;
                        }
                        len = st->lens[st->have - 1];
                        copy = 3u + BITS(2);
                        DROPBITS(2);
                    } else if (here.val == 17) {
                        NEEDBITS(here.bits + 3u);
                        DROPBITS(here.bits);
                        len = 0;
                        copy = 3u + BITS(3);
                        DROPBITS(3);
                    } else {
                        NEEDBITS(here.bits + 7u);
                        DROPBITS(here.bits);
                        len = 0;
                        copy = 11u + BITS(7);
                        DROPBITS(7);
                    }
                    if (st->have + copy > st->nlen + st->ndist) {
                        strm->msg = M_REPEAT;
                        st->mode = BAD;
                        break;
                    }
                    while (copy--)
                        st->lens[st->have++] = (uint16_t)len;
                }
            }
            if (st->mode == BAD)
                break;
            if (st->lens[256] == 0) {
                strm->msg = M_NO_EOB;
                st->mode = BAD;
                break;
            }
            st->next = stg + (uint32_t)offsetof(zstate, codes);
            st->lencode = st->next;
            st->lenbits = 9;
            ret = inflate_table(LENS, st->lens, st->nlen, &st->next, &st->lenbits, st->work);
            if (ret) {
                strm->msg = M_LITLEN_SET;
                st->mode = BAD;
                break;
            }
            st->distcode = st->next;
            st->distbits = 6;
            ret = inflate_table(DISTS, st->lens + st->nlen, st->ndist, &st->next, &st->distbits, st->work);
            if (ret) {
                strm->msg = M_DIST_SET;
                st->mode = BAD;
                break;
            }
            st->mode = LEN_;
            /* fallthrough */
        case LEN_:
            st->mode = LEN;
            /* fallthrough */
        case LEN:
            if (have >= 6 && left >= 258) {
                zio io = {next, put, have, left, hold, bits};
                inflate_fast(strm, st, &io, out);
                next = io.next;
                put = io.put;
                have = io.have;
                left = io.left;
                hold = io.hold;
                bits = io.bits;
                if (st->mode == TYPE)
                    st->back = -1;
                break;
            }
            st->back = 0;
            {
                const zcode *const lc = (const zcode *)H(st->lencode);
                for (;;) {
                    here = lc[BITS(st->lenbits)];
                    if ((unsigned)here.bits <= bits)
                        break;
                    PULLBYTE();
                }
                if (here.op && (here.op & 0xf0) == 0) {
                    last = here;
                    for (;;) {
                        here = lc[last.val + (BITS(last.bits + last.op) >> last.bits)];
                        if ((unsigned)(last.bits + here.bits) <= bits)
                            break;
                        PULLBYTE();
                    }
                    DROPBITS(last.bits);
                    st->back += last.bits;
                }
            }
            DROPBITS(here.bits);
#if ZLIB_MUT != 2
            st->back += here.bits;
#endif
            st->length = here.val;
            if (here.op == 0) {
                st->mode = LIT;
                break;
            }
            if (here.op & 32) {
                st->back = -1;
                st->mode = TYPE;
                break;
            }
            if (here.op & 64) {
                strm->msg = M_LITLEN_CODE;
                st->mode = BAD;
                break;
            }
            st->extra = here.op & 15u;
            st->mode = LENEXT;
            /* fallthrough */
        case LENEXT:
            if (st->extra) {
                NEEDBITS(st->extra);
                st->length += BITS(st->extra);
                DROPBITS(st->extra);
                st->back += st->extra;
            }
            st->was = st->length;
            st->mode = DIST;
            /* fallthrough */
        case DIST: {
            const zcode *const dc = (const zcode *)H(st->distcode);
            for (;;) {
                here = dc[BITS(st->distbits)];
                if ((unsigned)here.bits <= bits)
                    break;
                PULLBYTE();
            }
            if ((here.op & 0xf0) == 0) {
                last = here;
                for (;;) {
                    here = dc[last.val + (BITS(last.bits + last.op) >> last.bits)];
                    if ((unsigned)(last.bits + here.bits) <= bits)
                        break;
                    PULLBYTE();
                }
                DROPBITS(last.bits);
                st->back += last.bits;
            }
            DROPBITS(here.bits);
            st->back += here.bits;
            if (here.op & 64) {
                strm->msg = M_DIST_CODE;
                st->mode = BAD;
                break;
            }
            st->offset = here.val;
            st->extra = here.op & 15u;
            st->mode = DISTEXT;
        }
            /* fallthrough */
        case DISTEXT:
            if (st->extra) {
                NEEDBITS(st->extra);
                st->offset += BITS(st->extra);
                DROPBITS(st->extra);
                st->back += st->extra;
            }
            st->mode = MATCH;
            /* fallthrough */
        case MATCH:
            if (left == 0)
                goto inf_leave;
            copy = out - left;
            if (st->offset > copy) {
                copy = st->offset - copy;
                if (copy > st->whave) {
                    if (st->sane) {
                        strm->msg = M_TOO_FAR;
                        st->mode = BAD;
                        break;
                    }
                }
                if (copy > st->wnext) {
                    copy -= st->wnext;
                    from = H(st->window) + (st->wsize - copy);
                } else
                    from = H(st->window) + (st->wnext - copy);
                if (copy > st->length && ZLIB_MUT != 8)
                    copy = st->length;
            } else {
                from = put - st->offset;
                copy = st->length;
            }
            if (copy > left)
                copy = left;
            left -= copy;
            st->length -= copy;
            lz_copy(put, from, copy);
            put += copy;
            if (st->length == 0)
                st->mode = LEN;
            break;
        case LIT:
            if (left == 0)
                goto inf_leave;
            *put++ = (uint8_t)st->length;
            left--;
            st->mode = LEN;
            break;
        case CHECK:
            if (st->wrap) {
                NEEDBITS(32);
                out -= left;
                strm->total_out += out;
                st->total += out;
                if (out)
                    strm->adler = st->check = adler32_n(st->check, put - out, out);
                out = left;
                if (REVERSE(hold) != st->check) { /* flags == 0: gzip went to the original */
                    strm->msg = M_DATA_CHECK;
                    st->mode = BAD;
                    break;
                }
                INITBITS();
            }
            st->mode = LENGTH;
            /* fallthrough */
        case LENGTH:
            /* wrap && flags is gzip only */
            st->mode = DONE;
            /* fallthrough */
        case DONE:
            ret = Z_STREAM_END;
            goto inf_leave;
        case BAD:
            ret = Z_DATA_ERROR;
            goto inf_leave;
        default: /* MEM (only reached at entry) */
            ret = Z_MEM_ERROR;
            goto inf_leave;
        }

inf_leave:
    strm->next_out = G(put);
    strm->avail_out = left;
    strm->next_in = G(next);
    strm->avail_in = have;
    st->hold = hold;
    st->bits = bits;
    if (st->wsize || (st->mode < CHECK && out != strm->avail_out)) {
        if (st->window == 0u) {
            /* the translated updatewindow allocates it: the original's registers at 10dab90d and its
             * frame (ESI EBX EDI EBP saved at E-0x3c..E-0x30, strm pushed at E-0x40) */
            const uint32_t sv_ebx = c->r[R_EBX], sv_esi = c->r[R_ESI], sv_edi = c->r[R_EDI], sv_ebp = c->r[R_EBP];
            wr32(esp - 0x30u, sv_ebp);
            wr32(esp - 0x34u, sv_edi);
            wr32(esp - 0x38u, sv_ebx);
            wr32(esp - 0x3cu, sv_esi);
            c->r[R_EAX] = out;
            c->r[R_ECX] = left;
            c->r[R_EDX] = st->wsize ? have : left;
            c->r[R_EBX] = hold;
            c->r[R_ESI] = bits;
            c->r[R_EDI] = stg;
            c->r[R_EBP] = G(next);
            c->r[R_ESP] = esp - 0x44u;
            wr32(esp - 0x40u, sg);
            wr32(esp - 0x44u, ZI_RET_UPDATEWINDOW);
            if (recomp_profile_enabled)
                recomp_call(c, 0x10daa340u);
            else
                fn_10daa340(c);
            const uint32_t r = c->r[R_EAX];
            c->r[R_EBX] = sv_ebx;
            c->r[R_ESI] = sv_esi;
            c->r[R_EDI] = sv_edi;
            c->r[R_EBP] = sv_ebp;
            if (r != 0u) {
                st->mode = MEM;
                c->r[R_EAX] = (uint32_t)Z_MEM_ERROR;
                c->eip = rd32(esp);
                c->r[R_ESP] = esp + 8u;
                recomp_return(c);
                return;
            }
        } else
            updatewindow_n(strm, st, out);
    }
    in -= strm->avail_in;
    out -= strm->avail_out;
    strm->total_in += in;
    strm->total_out += out;
    st->total += out;
    if (st->wrap && out)
        strm->adler = st->check = adler32_n(st->check, H(strm->next_out) - out, out);
    const uint32_t ecx = (st->last ? 64u : 0u) + (st->mode == LEN_ || (st->mode == COPY_ && ZLIB_MUT != 7) ? 256u : 0u);
    strm->data_type = st->bits + ecx + (st->mode == TYPE && ZLIB_MUT != 3 ? 128u : 0u);
    if (in == 0 && out == 0 && ret == Z_OK)
        ret = Z_BUF_ERROR;
    zi_return(c, esp, ret, ecx, strm->data_type);

}
