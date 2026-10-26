/* native/tests/d3d_test.c - native/d3d.c against the translation.
 *
 * Build and run (from the checkout root):
 *   sh native/tests/build.sh d3d aux-engine/chunk_10310000.c aux-engine/chunk_10300000.c \
 *       aux-engine/chunk_10304000.c
 * (the chunks hold Execute 10311240, CreateVertexDeclaration 10303bd0 and
 * DestroyVertexDeclaration 10303eb0, SetVertexDeclaration 10304020). G3N_BENCH=1 adds timings.
 *
 * Everything the three call is a fake defined here: the IDirect3DDevice9, vertex declaration
 * and vertex buffer methods (indirect calls, answered in recomp_call_ic by target), the imports
 * of the error blocks (DXGetErrorString, bCString, the log; IAT slots pointed at fake targets),
 * the direct calls (_memmove, _memset, _memcpy, the cache's push_back). Each fake appends a
 * record to a log in guest memory: target, return address, ESP, EAX..EDI, DF, seven
 * stack arguments, and a hash of the guest stack from its ESP to the top of the frame. Then it
 * answers from a hash of the case seed, its position in the call sequence and those inputs:
 * EAX (an HRESULT that fails now and then, so every error block runs), clobbered ECX/EDX, the
 * six flags, and the out-parameters the caller reads back (GetDeclaration's elements, the
 * stream source, the buffer description, the locked pointer). The log is compared memory, so a
 * call made in another order, with another argument, register, flag, ESP or stack slot, or one
 * too many or too few, fails the case.
 *
 * Each case runs the original and the native from the same state (random registers, x87, XMM;
 * difftest_rp.h) and compares registers, x87 state, memory and here also DF and the whole
 * stack region, the dead bytes below the final ESP included. Execute runs both with fake
 * wrapper methods (its own paths) and with the real eCDX9Device vtable, so its calls of
 * CreateVertexDeclaration, SetVertexDeclaration and DestroyVertexDeclaration go through the
 * translated or the native ones. */
#include "difftest_rp.h"

#include <time.h>

void fn_10311240(X86 *c);
void g3n_10311240(X86 *c);
void fn_10303bd0(X86 *c);
void g3n_10303bd0(X86 *c);
void fn_10304020(X86 *c);
void g3n_10304020(X86 *c);
void fn_10303eb0(X86 *c);

#define ESP0 0x0e800000u
#define STACK_LO (ESP0 - 0x1000u)
#define STACK_LEN 0x1040u

#define PRIM 0x03000000u    /* eCDX9Primitive, 0x100 */
#define WRAP 0x03001000u    /* eCDX9Device: +4.. +8 device, +0x54 cache, +0x58 its count */
#define WRAPVT 0x03001100u  /* fake wrapper vtable */
#define DEV 0x03002000u     /* IDirect3DDevice9: [DEV] = DEVVT */
#define DEVVT 0x03002100u
#define DECL1 0x03003000u   /* the primitive's declaration */
#define DECL2 0x03003010u   /* the cut one, from the cache */
#define DECLVT 0x03003100u
#define VB0 0x03004000u     /* four vertex buffers, 0x10 apart, then the white one, the new one */
#define WVB 0x03004040u
#define NEWVB 0x03004050u
#define VBVT 0x03004100u
#define IB 0x03004200u
#define CACHE 0x03005000u   /* the declaration cache: 12-byte entries */
#define POOL 0x03006000u    /* their elements, 0x100 per entry */
#define ARGELEMS 0x03008000u /* CreateVertexDeclaration's argument */
#define LOCKBUF 0x03010000u
#define LOCKLEN 0x8000u
#define DATAIMP 0x03020000u /* what [0x105f7290] points to */
#define LOG 0x03030000u
#define LOG_SIZE 0x10000u
#define NEWBLOCK 0x03040000u /* operator new's answer */

#define FAKE_DEV 0x0fd10000u  /* + vtable offset */
#define FAKE_DECL 0x0fd20000u
#define FAKE_VB 0x0fd30000u
#define FAKE_WRAP 0x0fd40000u
#define FAKE_IMP 0x0fd50000u  /* + IAT slot & 0xffff */

#define CVD_REAL 0x10303bd0u
#define DVD_REAL 0x10303eb0u
#define SVD_REAL 0x10304020u
#define WRAPVT_REAL 0x1068aafcu

#define RET_EXEC 0x102f7e6bu /* a return site in eCDX9RenderContext::Execute */
#define RET_CVD 0x10311a68u
#define RET_SVD 0x10311a8bu

static uint32_t case_seed;
static int fail_rate = 40;   /* one call in fail_rate fails (0: none) */
static int bench_fast;
static uint32_t frame_top;   /* the hash runs from a fake's ESP to here */

static uint32_t mix(uint32_t h, uint32_t v) {
    h ^= v + 0x7f4a7c15u + (h << 6) + (h >> 2);
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

static uint32_t flags_of(const X86 *c) {
    return c->eflags_cf | c->eflags_pf << 1 | c->eflags_af << 2 | c->eflags_zf << 3 | c->eflags_sf << 4 |
           c->eflags_of << 5;
}
/* The arithmetic flags at a call and at the return are dead in the game's translation (game.toml
 * [translate] flags_abi, D-030): the translated original no longer computes them there, so neither
 * the call log nor the comparison at the end looks at them. DF is still compared. */
static uint32_t live_flags_of(const X86 *c) {
    return c->eflags_df;
}

#define REC 24u /* words per record */

/* Logs the call, returns its hash. */
static uint32_t fake_enter(X86 *c, uint32_t addr) {
    const uint32_t n = rd32(LOG);
    wr32(LOG, n + 1u);
    if (bench_fast)
        return mix(case_seed ^ addr, n);
    const uint32_t e = LOG + 4u + 4u * REC * n;
    if (e + 4u * REC > LOG + LOG_SIZE) {
        fprintf(stderr, "call log full\n");
        abort();
    }
    const uint32_t esp = c->r[4];
    uint32_t sh = 0x12345u;
    for (uint32_t a = esp; a < frame_top; a += 4u)
        sh = mix(sh, rd32(a));
    uint32_t w[REC] = {addr, rd32(esp), esp, c->r[0], c->r[1], c->r[2], c->r[3], c->r[5], c->r[6], c->r[7],
                       live_flags_of(c), sh};
    for (unsigned i = 0; i < 7; ++i)
        w[12 + i] = rd32(esp + 4u + 4u * i);
    for (unsigned i = 0; i < REC; ++i)
        wr32(e + 4u * i, w[i]);
    uint32_t h = mix(case_seed, addr);
    h = mix(h, n);
    for (unsigned i = 1; i < 19; ++i)
        h = mix(h, w[i]);
    return h;
}

/* A word into the last record's spare slots (19..23). */
static void fake_note(unsigned k, uint32_t v) {
    if (bench_fast)
        return;
    const uint32_t n = rd32(LOG);
    wr32(LOG + 4u + 4u * REC * (n - 1u) + 4u * (19u + k), v);
}

static void fake_leave(X86 *c, uint32_t h, uint32_t eax, uint32_t retn) {
    c->r[0] = eax;
    c->r[1] = mix(h, 2);
    c->r[2] = mix(h, 3);
    const uint32_t f = mix(h, 4);
    c->eflags_cf = f & 1u;
    c->eflags_pf = (f >> 1) & 1u;
    c->eflags_af = (f >> 2) & 1u;
    c->eflags_zf = (f >> 3) & 1u;
    c->eflags_sf = (f >> 4) & 1u;
    c->eflags_of = (f >> 5) & 1u;
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + retn;
}

/* An HRESULT: S_OK mostly, a failure one call in fail_rate, now and then another success. */
static uint32_t pick_hr(uint32_t h) {
    if (fail_rate && (h >> 8) % (uint32_t)fail_rate == 0u)
        return 0x8876086cu - (h & 3u);
    return (h >> 20) % 16u == 0u ? 1u + (h & 0xffu) : 0u;
}

/* ---- the elements GetDeclaration hands out (set per case) ------------------------------- */

static uint8_t case_elems[0x200];
static uint32_t case_n;

static void make_elems(void) {
    const uint32_t real = dt_rand32() % 10u == 0 ? 0u : dt_rand32() % 13u;
    int want_c1 = dt_rand32() % 3u == 0;
    for (uint32_t i = 0; i < real; ++i) {
        uint8_t *e = case_elems + 8u * i;
        uint16_t stream = dt_rand32() % 8u == 0 ? (uint16_t)(4u + dt_rand32() % 4u) : (uint16_t)(dt_rand32() % 4u);
        if (dt_rand32() % 40u == 0)
            stream = (uint16_t)dt_rand32();
        const uint16_t off = (uint16_t)(dt_rand32() % 6u == 0 ? dt_rand32() % 80u : 4u * (dt_rand32() % 10u));
        memcpy(e, &stream, 2);
        memcpy(e + 2, &off, 2);
        e[4] = (uint8_t)(dt_rand32() % 18u);
        e[5] = 0;
        static const uint8_t usages[] = {0, 3, 5, 10, 10, 10, 6, 7, 1, 2};
        e[6] = usages[dt_rand32() % sizeof usages];
        e[7] = (uint8_t)(dt_rand32() % 4u == 0 ? dt_rand32() % 4u : (dt_rand32() & 1u));
        if (want_c1 && e[6] == 10)
            e[7] = 1;
    }
    uint8_t *end = case_elems + 8u * real;
    const uint8_t end_[8] = {0xff, 0, 0, 0, 17, 0, 0, 0};
    memcpy(end, end_, 8);
    case_n = real + 1u;
    if (dt_rand32() % 50u == 0)
        case_n = 0; /* a count of none */
}

/* ---- the fakes ------------------------------------------------------------------------- */

static void dev_method(X86 *c, uint32_t off) {
    const uint32_t h = fake_enter(c, FAKE_DEV + off);
    const uint32_t esp = c->r[4];
    switch (off) {
    case 8: /* Release (GetDevice's reference) */
        fake_leave(c, h, h & 7u, 4);
        return;
    case 0x190: /* SetStreamSource(dev, s, vb, offset, stride) */
        fake_leave(c, h, pick_hr(h), 0x14);
        return;
    case 0x198: /* SetStreamSourceFreq(dev, s, freq) */
        fake_leave(c, h, pick_hr(h), 0xc);
        return;
    case 0x1a0: /* SetIndices */
    case 0x164: /* SetFVF */
    case 0x15c: /* SetVertexDeclaration */
        fake_leave(c, h, pick_hr(h), 8);
        return;
    case 0x148: /* DrawIndexedPrimitive */
        fake_leave(c, h, pick_hr(h), 0x1c);
        return;
    case 0x144: /* DrawPrimitive */
        fake_leave(c, h, pick_hr(h), 0x10);
        return;
    case 0x194: { /* GetStreamSource(dev, s, &vb, &offset, &stride) */
        const uint32_t hr = pick_hr(h);
        if ((int32_t)hr >= 0) {
            wr32(rd32(esp + 12u), VB0 + 0x10u * (h & 3u));
            wr32(rd32(esp + 16u), (h >> 4) & 0x3cu);
            wr32(rd32(esp + 20u), (h >> 10) % 23u == 0 ? 0u : 4u + ((h >> 12) % 16u) * 4u);
        }
        fake_leave(c, h, hr, 0x14);
        return;
    }
    case 0x68: { /* CreateVertexBuffer(dev, len, usage, fvf, pool, &vb, 0) */
        fake_note(0, rd32(esp + 8u));
        const uint32_t hr = pick_hr(h);
        if ((int32_t)hr >= 0)
            wr32(rd32(esp + 24u), NEWVB);
        fake_leave(c, h, hr, 0x1c);
        return;
    }
    case 0x158: { /* CreateVertexDeclaration(dev, elems, &decl) */
        const uint32_t hr = pick_hr(h);
        if ((int32_t)hr >= 0)
            wr32(rd32(esp + 12u), (h >> 9) % 7u == 0 ? 0u : DECL2 + 0x20u);
        fake_leave(c, h, hr, 0xc);
        return;
    }
    }
    fprintf(stderr, "unexpected device method %x\n", off);
    abort();
}

static void decl_method(X86 *c, uint32_t off) {
    const uint32_t h = fake_enter(c, FAKE_DECL + off);
    const uint32_t esp = c->r[4];
    switch (off) {
    case 4:
    case 8: /* AddRef, Release */
        /* the reference count, in compared memory */
        wr32(LOG + LOG_SIZE - 4u, rd32(LOG + LOG_SIZE - 4u) + (off == 4 ? 1u : (uint32_t)-1));
        fake_leave(c, h, 1u + (h & 7u), 4);
        return;
    case 0x10: { /* GetDeclaration(decl, elems, &n) */
        const uint32_t hr = pick_hr(h);
        if ((int32_t)hr >= 0) {
            memcpy(g_mem + rd32(esp + 8u), case_elems, 8u * case_n);
            wr32(rd32(esp + 12u), case_n);
        }
        fake_leave(c, h, hr, 0xc);
        return;
    }
    }
    fprintf(stderr, "unexpected declaration method %x\n", off);
    abort();
}

static void vb_method(X86 *c, uint32_t off) {
    const uint32_t h = fake_enter(c, FAKE_VB + off);
    const uint32_t esp = c->r[4];
    switch (off) {
    case 8: /* Release */
        fake_leave(c, h, h & 3u, 4);
        return;
    case 0xc: /* GetDevice(res, &dev) */
        wr32(rd32(esp + 8u), (h >> 5) % 3u == 0 ? 0x03002800u : DEV);
        fake_leave(c, h, pick_hr(h), 8);
        return;
    case 0x2c: { /* Lock(vb, off, size, &p, flags) */
        const uint32_t hr = pick_hr(h);
        if ((int32_t)hr >= 0)
            wr32(rd32(esp + 16u), LOCKBUF);
        fake_leave(c, h, hr, 0x14);
        return;
    }
    case 0x30: /* Unlock */
        fake_leave(c, h, pick_hr(h), 4);
        return;
    case 0x34: { /* GetDesc(vb, &desc) */
        const uint32_t hr = pick_hr(h);
        if ((int32_t)hr >= 0) {
            const uint32_t d = rd32(esp + 8u);
            for (uint32_t k = 0; k < 0x18u; k += 4u)
                wr32(d + k, mix(h, k));
            wr32(d + 0x10u, (h >> 3) % 0x2000u);
        }
        fake_leave(c, h, hr, 8);
        return;
    }
    }
    fprintf(stderr, "unexpected buffer method %x\n", off);
    abort();
}

/* The wrapper's methods when its vtable is the fake one. */
static void wrap_method(X86 *c, uint32_t off) {
    const uint32_t h = fake_enter(c, FAKE_WRAP + off);
    const uint32_t esp = c->r[4];
    switch (off) {
    case 0x170: { /* CreateVertexDeclaration(elems, &decl): the cut elements into the log */
        const uint32_t el = rd32(esp + 4u);
        uint32_t eh = 0;
        for (uint32_t k = 0; k < 0x200u; k += 4u)
            eh = mix(eh, rd32(el + k));
        fake_note(0, eh);
        const uint32_t ok = (h >> 7) % 12u == 0 ? (h >> 11) % 3u : 1u;
        wr32(rd32(esp + 8u), (h >> 13) % 10u == 0 ? 0u : DECL2);
        fake_leave(c, h, ok, 8);
        return;
    }
    case 0x17c: /* SetVertexDeclaration(decl) */
    case 0x174: /* DestroyVertexDeclaration(decl) */
        fake_leave(c, h, (h >> 7) % 12u == 0 ? (h >> 11) % 3u : 1u, 4);
        return;
    }
    fprintf(stderr, "unexpected wrapper method %x\n", off);
    abort();
}

/* Imports: RET n by slot. */
static void imp_call(X86 *c, uint32_t slot) {
    const uint32_t h = fake_enter(c, FAKE_IMP + slot);
    uint32_t retn;
    switch (slot) {
    case 0x72c0: retn = 4; break;    /* bCString(char const *) */
    case 0x6670: retn = 0x24; break; /* the formatter */
    case 0x71f0: retn = 0; break;
    case 0x7210: retn = 0; break;
    case 0x71f4: retn = 8; break;
    case 0x71f8: retn = 0; break;
    case 0x722c: retn = 0; break;    /* ~bCString */
    case 0x7230: retn = 4; break;    /* operator new(size) */
    case 0x6624: retn = 0; break;
    default:
        fprintf(stderr, "unexpected import %x\n", slot);
        abort();
    }
    fake_leave(c, h, slot == 0x6624u || slot == 0x7230u ? NEWBLOCK : mix(h, 9), retn);
}

/* Execute with the real wrapper: whether the case's cut declaration is made to be in the cache
 * (at entry plant_at), so that the lookup hits as it does in the game. */
static int plant_hit;
static uint32_t plant_at;
static void plant(uint32_t elems) {
    uint32_t n = 0;
    while (n < 64u && rd16(elems + 8u * n) != 0xffu)
        ++n;
    if (rd32(WRAP + 0x58u) <= plant_at || rd32(WRAP + 0x54u) != CACHE)
        return;
    memcpy(g_mem + POOL + 0x100u * plant_at, g_mem + elems, 8u * n);
    wr32(CACHE + 12u * plant_at + 4u, n);
    wr32(CACHE + 12u * plant_at + 8u, POOL + 0x100u * plant_at);
}

void recomp_call_ic(X86 *c, uint32_t target, uint64_t *ic) {
    (void)ic;
    if (target - FAKE_DEV < 0x400u) {
        dev_method(c, target - FAKE_DEV);
        return;
    }
    if (target - FAKE_DECL < 0x100u) {
        decl_method(c, target - FAKE_DECL);
        return;
    }
    if (target - FAKE_VB < 0x100u) {
        vb_method(c, target - FAKE_VB);
        return;
    }
    if (target - FAKE_WRAP < 0x400u) {
        wrap_method(c, target - FAKE_WRAP);
        return;
    }
    if (target - FAKE_IMP < 0x10000u) {
        imp_call(c, target - FAKE_IMP);
        return;
    }
    if (target == CVD_REAL) {
        if (plant_hit) /* the cut declaration into a cache entry, the same in both runs */
            plant(rd32(c->r[4] + 4u));
        g3n_10303bd0(c);
        return;
    }
    if (target == SVD_REAL) {
        g3n_10304020(c);
        return;
    }
    if (target == DVD_REAL) {
        fn_10303eb0(c);
        return;
    }
    fprintf(stderr, "unexpected indirect call to %08x from %08x (g3_opt %x)\n", target, rd32(c->r[4]), g3_opt);
    abort();
}

void recomp_call(X86 *c, uint32_t target) {
    recomp_call_ic(c, target, NULL);
}

/* Direct calls. */
static void cdecl_leave(X86 *c, uint32_t h, uint32_t eax) {
    fake_leave(c, h, eax, 0);
}
void g3n_10564220(X86 *c) { /* _memmove(dst, src, n) */
    const uint32_t h = fake_enter(c, 0x10564220u);
    const uint32_t esp = c->r[4], d = rd32(esp + 4u), s = rd32(esp + 8u), n = rd32(esp + 12u);
    if (n > 0x1000u) {
        fprintf(stderr, "memmove of %u\n", n);
        abort();
    }
    memmove(g_mem + d, g_mem + s, n);
    cdecl_leave(c, h, d);
}
void fn_1056a710(X86 *c) { /* _memset(p, v, n) */
    const uint32_t h = fake_enter(c, 0x1056a710u);
    const uint32_t esp = c->r[4], d = rd32(esp + 4u), n = rd32(esp + 12u);
    if (d != LOCKBUF || n > LOCKLEN) {
        fprintf(stderr, "memset %08x %u\n", d, n);
        abort();
    }
    memset(g_mem + d, (int)rd32(esp + 8u), n);
    cdecl_leave(c, h, d);
}
void g3n_1056ad40(X86 *c) { /* _memcpy(d, s, n) */
    const uint32_t h = fake_enter(c, 0x1056ad40u);
    const uint32_t esp = c->r[4], d = rd32(esp + 4u), s = rd32(esp + 8u), n = rd32(esp + 12u);
    if (n > 0x400u) {
        fprintf(stderr, "memcpy of %u\n", n);
        abort();
    }
    memcpy(g_mem + d, g_mem + s, n);
    cdecl_leave(c, h, d);
}
void fn_1059225a(X86 *c) {
    const uint32_t h = fake_enter(c, 0x1059225au);
    fake_leave(c, h, mix(h, 8), 4);
}
void fn_105a40c6(X86 *c) {
    const uint32_t h = fake_enter(c, 0x105a40c6u);
    fake_leave(c, h, mix(h, 8), 4);
}
void fn_1030c2d0(X86 *c) { /* the cache's push_back(EAX = array, &entry), RET 4 */
    const uint32_t h = fake_enter(c, 0x1030c2d0u);
    const uint32_t p = rd32(c->r[4] + 4u);
    fake_note(0, rd32(p));
    fake_note(1, rd32(p + 4u));
    fake_note(2, rd32(p + 8u));
    fake_leave(c, h, mix(h, 8), 4);
}

/* ---- the world ------------------------------------------------------------------------- */

static const uint32_t iat_slots[] = {0x72c0, 0x6670, 0x71f0, 0x7210, 0x71f4, 0x71f8, 0x722c, 0x7230, 0x6624};

static void fill_random(uint32_t a, uint32_t n) {
    for (uint32_t k = 0; k < n; k += 4)
        wr32(a + k, dt_rand32());
}

static void world_common(void) {
    case_seed = dt_rand32();
    memset(g_mem + LOG, 0, LOG_SIZE);
    for (unsigned i = 0; i < sizeof iat_slots / sizeof iat_slots[0]; ++i)
        wr32(0x105f0000u + iat_slots[i], FAKE_IMP + iat_slots[i]);
    wr32(0x105f7290u, DATAIMP);
    wr32(DATAIMP, dt_rand32());
    wr32(DEV, DEVVT);
    wr32(0x03002800u, DEVVT); /* another device, as GetDevice may answer */
    for (uint32_t k = 0; k < 0x400u; k += 4u)
        wr32(DEVVT + k, FAKE_DEV + k);
    for (uint32_t k = 0; k < 0x20u; k += 0x10u)
        wr32(DECL1 + k, DECLVT);
    wr32(DECL2 + 0x20u, DECLVT);
    for (uint32_t k = 0; k < 0x100u; k += 4u)
        wr32(DECLVT + k, FAKE_DECL + k);
    for (uint32_t k = 0; k <= 0x50u; k += 0x10u)
        wr32(VB0 + k, VBVT);
    for (uint32_t k = 0; k < 0x100u; k += 4u)
        wr32(VBVT + k, FAKE_VB + k);
    for (uint32_t k = 0; k < 0x400u; k += 4u)
        wr32(WRAPVT + k, FAKE_WRAP + k);
}

/* The wrapper's declaration cache: up to 12 entries, element counts 0..6, some duplicates. When
 * `hit_from` is set the argument copies an entry; otherwise it is random-ish (mostly a miss). */
static void make_cache(uint32_t arg) {
    const uint32_t count = dt_rand32() % 8u == 0 ? 0u : 1u + dt_rand32() % 12u;
    wr32(WRAP + 0x54u, CACHE);
    wr32(WRAP + 0x58u, count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = CACHE + 12u * i, el = POOL + 0x100u * i;
        uint32_t n = dt_rand32() % 7u;
        if (i && dt_rand32() % 4u == 0) { /* a copy of an earlier one, or of its count */
            const uint32_t j = dt_rand32() % i;
            n = rd32(CACHE + 12u * j + 4u);
            memcpy(g_mem + el, g_mem + POOL + 0x100u * j, 8u * n);
            if (dt_rand32() & 1u)
                g_mem[el + dt_rand32() % (8u * n + 1u)] ^= (uint8_t)(1u << (dt_rand32() % 8u));
        } else {
            for (uint32_t k = 0; k < 8u * n; ++k)
                g_mem[el + k] = (uint8_t)(dt_rand32() % 3u == 0 ? dt_rand32() : (k & 7u) == 0 ? dt_rand32() % 4u : 0u);
            for (uint32_t k = 0; k < n; ++k)
                if (rd16(el + 8u * k) == 0xffu)
                    wr16(el + 8u * k, 1u);
        }
        wr32(e, DECL2 + 0x20u);
        wr32(e + 4u, n);
        wr32(e + 8u, el);
    }
    /* the argument: an entry's elements and the end, or something else */
    uint32_t n = dt_rand32() % 7u;
    if (count && dt_rand32() % 4u) {
        const uint32_t j = dt_rand32() % count;
        n = rd32(CACHE + 12u * j + 4u);
        memcpy(g_mem + arg, g_mem + POOL + 0x100u * j, 8u * n);
        if (dt_rand32() % 6u == 0 && n)
            g_mem[arg + dt_rand32() % (8u * n)] ^= 0x40u;
    } else {
        for (uint32_t k = 0; k < 8u * n; ++k)
            g_mem[arg + k] = (uint8_t)(dt_rand32() % 4u);
        for (uint32_t k = 0; k < n; ++k)
            if (rd16(arg + 8u * k) == 0xffu)
                wr16(arg + 8u * k, 2u);
    }
    wr16(arg + 8u * n, 0xffu);
    wr16(arg + 8u * n + 2u, 0u);
    wr32(arg + 8u * n + 4u, 17u);
}

static int real_wrapper;

static void setup_exec(X86 *c) {
    world_common();
    fill_random(PRIM, 0x100u);
    const uint8_t mode = dt_rand32() % 5u == 0 ? (uint8_t)(1u + dt_rand32() % 3u) : 0u;
    wr8(PRIM + 0x60u, mode);
    wr32(PRIM + 0x64u, mode ? dt_rand32() & 0x1ffu : DECL1);
    wr32(PRIM + 4u, WRAP);
    wr32(PRIM + 8u, DEV);
    for (uint32_t s = 0; s < 4u; ++s) {
        wr32(PRIM + 0xcu + 4u * s, dt_rand32() % 2u ? VB0 + 0x10u * s : 0u);
        wr8(PRIM + 0x1cu + 16u * s, dt_rand32() % 5u == 0 ? 0u : (dt_rand32() % 6u == 0 ? (uint8_t)dt_rand32() : 1u));
        wr32(PRIM + 0x20u + 16u * s, dt_rand32() % 3u ? 0u : dt_rand32() % 256u);
        static const uint32_t strides[] = {0, 12, 16, 20, 24, 28, 32, 36, 40, 44, 64};
        wr32(PRIM + 0x24u + 16u * s, strides[dt_rand32() % (sizeof strides / sizeof strides[0])]);
        wr32(PRIM + 0x28u + 16u * s, dt_rand32() % 4u ? 1u : dt_rand32());
    }
    wr32(PRIM + 0x5cu, IB);
    wr8(PRIM + 0x68u, dt_rand32() % 4u ? 1u : (uint8_t)(dt_rand32() % 3u));
    make_elems();
    /* the wrapper: fake methods, or the real vtable over a cache */
    fill_random(WRAP, 0x60u);
    wr32(WRAP + 8u, dt_rand32() % 30u == 0 ? 0u : DEV);
    real_wrapper = dt_rand32() & 1u;
    wr32(WRAP, real_wrapper ? WRAPVT_REAL : WRAPVT);
    if (real_wrapper) {
        /* a cache that holds the cut declaration now and then: its elements from the case's */
        make_cache(ARGELEMS);
        const uint32_t count = rd32(WRAP + 0x58u);
        plant_hit = count && dt_rand32() % 4u != 0;
        plant_at = count ? dt_rand32() % count : 0u;
    } else {
        plant_hit = 0;
    }
    /* the white buffer */
    wr32(0x1086db98u, dt_rand32() % 3u == 0 ? 0u : WVB);
    static const uint32_t lens[] = {0, 0x800, 0x1000, 0x1800};
    wr32(0x1086db94u, lens[dt_rand32() % 4u]);
    dt_context(c, ESP0, dt_rand32() % 6u);
    c->eflags_cf = dt_rand32() & 1u;
    c->eflags_pf = dt_rand32() & 1u;
    c->eflags_af = dt_rand32() & 1u;
    c->eflags_zf = dt_rand32() & 1u;
    c->eflags_sf = dt_rand32() & 1u;
    c->eflags_of = dt_rand32() & 1u;
    c->r[1] = PRIM;
    c->r[4] = ESP0 - 4u * (dt_rand32() % 4u); /* the frame aligns down from here */
    c->r[4] -= 4u;
    wr32(c->r[4], RET_EXEC);
    frame_top = ESP0 + 0x10u;
}

static void setup_cvd(X86 *c) {
    world_common();
    fill_random(WRAP, 0x60u);
    wr32(WRAP, WRAPVT_REAL);
    wr32(WRAP + 8u, dt_rand32() % 20u == 0 ? 0u : DEV);
    make_cache(ARGELEMS);
    dt_context(c, ESP0, dt_rand32() % 6u);
    c->eflags_af = dt_rand32() & 1u;
    c->r[1] = WRAP;
    c->r[4] -= 4u;
    wr32(c->r[4], ESP0 - 0x40u); /* &decl */
    c->r[4] -= 4u;
    wr32(c->r[4], ARGELEMS);
    c->r[4] -= 4u;
    wr32(c->r[4], RET_CVD);
    frame_top = ESP0 + 0x10u;
}

static void setup_svd(X86 *c) {
    world_common();
    fill_random(WRAP, 0x60u);
    wr32(WRAP, WRAPVT_REAL);
    wr32(WRAP + 8u, dt_rand32() % 10u == 0 ? 0u : DEV);
    dt_context(c, ESP0, dt_rand32() % 6u);
    c->eflags_af = dt_rand32() & 1u;
    c->r[1] = WRAP;
    c->r[4] -= 4u;
    wr32(c->r[4], DECL2);
    c->r[4] -= 4u;
    wr32(c->r[4], RET_SVD);
    frame_top = ESP0 + 0x10u;
}

/* Beyond dt_compare: DF (live_flags_of). */
static int compare_flags(const char *what, const X86 *o, const X86 *n) {
    if (live_flags_of(o) == live_flags_of(n))
        return 1;
    if (dt_fail < 20)
        fprintf(stderr, "FAIL %s: flags %02x/%02x\n", what, flags_of(o), flags_of(n));
    ++dt_fail;
    return 0;
}

static int run(const char *what, X86 *c, void (*orig)(X86 *), void (*native)(X86 *)) {
    X86 start = *c, o;
    dt_save(0);
    g3_opt = 0;
    orig(c);
    o = *c;
    dt_save(1);
    dt_restore();
    *c = start;
    g3_opt = 0xffffffffu;
    native(c);
    const int ok = dt_compare(what, &o, c);
    return compare_flags(what, &o, c) && ok;
}

static unsigned calls_hist[64], exits[2], cut_draws, color_paths, err_paths;

static void count_exec(const X86 *c) {
    const uint32_t n = rd32(LOG);
    calls_hist[n < 63u ? n : 63u]++;
    exits[(c->r[0] & 0xffu) != 0]++;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t t = rd32(LOG + 4u + 4u * REC * i);
        if (t == FAKE_WRAP + 0x170u || t == 0x1030c2d0u || (t == FAKE_DECL + 4u && real_wrapper))
            ++cut_draws;
        if (t == FAKE_DEV + 0x194u)
            ++color_paths;
        if (t == 0x1059225au)
            ++err_paths;
    }
}

static void test_exec(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_exec(&c);
        if (run("Execute", &c, fn_10311240, g3n_10311240))
            count_exec(&c);
    }
}

static unsigned cvd_hits, cvd_miss, cvd_null;

static void test_cvd(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_cvd(&c);
        const int null_dev = rd32(WRAP + 8u) == 0u;
        if (run("CreateVertexDeclaration", &c, fn_10303bd0, g3n_10303bd0)) {
            if (null_dev)
                ++cvd_null;
            else if (rd32(LOG) && rd32(LOG + 4u) == FAKE_DECL + 4u)
                ++cvd_hits;
            else
                ++cvd_miss;
        }
    }
}

static unsigned svd_calls[4];

static void test_svd(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_svd(&c);
        if (run("SetVertexDeclaration", &c, fn_10304020, g3n_10304020))
            svd_calls[rd32(LOG) > 2u ? 3u : rd32(LOG)]++;
    }
}

/* With the group off each g3n_ is its translation. */
static void test_off(unsigned n) {
    X86 c;
    for (unsigned i = 0; i < n; ++i) {
        setup_exec(&c);
        X86 start = c, o;
        dt_save(0);
        g3_opt = 0;
        fn_10311240(&c);
        o = c;
        dt_save(1);
        dt_restore();
        c = start;
        g3_opt = ~G3OPT_D3D;
        g3n_10311240(&c);
        dt_compare("Execute (d3d off)", &o, &c);
        compare_flags("Execute (d3d off)", &o, &c);
    }
    g3_opt = 0xffffffffu;
}

/* ---- G3N_BENCH ------------------------------------------------------------------------- */

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static void null_call(X86 *c) {
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u;
}

/* The floor under a case: as many fake calls as the native made in it (SetIndices' fake, through
 * an indirect call site and a RET), and nothing else. */
static uint32_t floor_calls;
static void calls_only(X86 *c) {
    static uint64_t ic;
    for (uint32_t k = 0; k < floor_calls; ++k) {
        c->r[4] -= 4u;
        wr32(c->r[4], IB);
        c->r[4] -= 4u;
        wr32(c->r[4], DEV);
        c->r[4] -= 4u;
        wr32(c->r[4], 0x10311504u);
        recomp_call_cached(c, FAKE_DEV + 0x1a0u, &ic);
        recomp_return(c);
    }
    null_call(c);
}

/* The same cases through each, ns per call less the restore. The fakes cost the same in both
 * (bench_fast: no log, no stack hash), so the difference is what the native saves; less the
 * floor (the same number of bare fake calls), what is left is each one's own work. */
static void bench_one(const char *name, void (*setup)(X86 *), void (*orig)(X86 *), void (*native)(X86 *),
                      int (*want)(void)) {
    enum { NC = 128, REP = 2000 };
    static X86 start[NC];
    static uint32_t seed[NC], ncalls[NC];
    static uint8_t prim[NC][0x100], wrap[NC][0x60], cache[NC][0x100], white[NC][8], pool[NC][0x1000],
        arg[NC][0x100];
    fail_rate = 0;
    double calls = 0;
    for (unsigned i = 0; i < NC; ++i) {
        do
            setup(&start[i]);
        while (want && !want());
        seed[i] = case_seed;
        memcpy(prim[i], g_mem + PRIM, 0x100);
        memcpy(wrap[i], g_mem + WRAP, 0x60);
        memcpy(cache[i], g_mem + CACHE, 0x100);
        memcpy(white[i], g_mem + 0x1086db94u, 8);
        memcpy(pool[i], g_mem + POOL, 0x1000);
        memcpy(arg[i], g_mem + ARGELEMS, 0x100);
        X86 c = start[i];
        wr32(LOG, 0u);
        native(&c);
        ncalls[i] = rd32(LOG);
        calls += ncalls[i];
    }
    void (*const f[4])(X86 *) = {orig, native, null_call, calls_only};
    double t[4];
    bench_fast = 1;
    for (int k = 0; k < 4; ++k) {
        g3_opt = k == 1 ? 0xffffffffu : 0u;
        const double t0 = now_ns();
        for (unsigned rep = 0; rep < REP; ++rep)
            for (unsigned i = 0; i < NC; ++i) {
                memcpy(g_mem + PRIM, prim[i], 0x100);
                memcpy(g_mem + WRAP, wrap[i], 0x60);
                memcpy(g_mem + CACHE, cache[i], 0x100);
                memcpy(g_mem + 0x1086db94u, white[i], 8);
                memcpy(g_mem + POOL, pool[i], 0x1000);
                memcpy(g_mem + ARGELEMS, arg[i], 0x100);
                wr32(LOG, 0u);
                case_seed = seed[i];
                floor_calls = ncalls[i];
                X86 c = start[i];
                f[k](&c);
            }
        t[k] = (now_ns() - t0) / (double)(REP * NC);
    }
    g3_opt = 0xffffffffu;
    bench_fast = 0;
    fail_rate = 40;
    const double fl = t[3] - t[2];
    printf("bench %s: %.1f calls out; ns per call less the restore (%.1f ns): translation %.1f, native %.1f, "
           "ratio %.2f; less the floor of bare fake calls (%.1f ns): %.1f / %.1f, ratio %.2f "
           "(warm cache)\n",
           name, calls / NC, t[2], t[0] - t[2], t[1] - t[2], (t[0] - t[2]) / (t[1] - t[2]), fl, t[0] - t[2] - fl,
           t[1] - t[2] - fl, (t[0] - t[2] - fl) / (t[1] - t[2] - fl));
}

/* Execute with the real wrapper and a cache hit: the per-draw path of a cut declaration. */
static int want_exec_typical(void) {
    return rd8(PRIM + 0x60u) == 0u && real_wrapper && rd32(WRAP + 8u) == DEV;
}
static int want_cvd_hit(void) {
    const uint32_t count = rd32(WRAP + 0x58u);
    if (count < 6u || rd32(WRAP + 8u) != DEV)
        return 0;
    uint32_t n = 0;
    while (rd16(ARGELEMS + 8u * n) != 0xffu)
        ++n;
    for (uint32_t i = 0; i < count; ++i) /* a hit, past the first half of the cache */
        if (rd32(CACHE + 12u * i + 4u) == n && !memcmp(g_mem + ARGELEMS, g_mem + rd32(CACHE + 12u * i + 8u), 8u * n))
            return n && i >= count / 2u;
    return 0;
}

static void bench(void) {
    bench_one("Execute (real wrapper)", setup_exec, fn_10311240, g3n_10311240, want_exec_typical);
    bench_one("CreateVertexDeclaration (cache of 6+)", setup_cvd, fn_10303bd0, g3n_10303bd0, want_cvd_hit);
    bench_one("SetVertexDeclaration", setup_svd, fn_10304020, g3n_10304020, NULL);
}

int main(void) {
    harness_mem();
    dt_load_pe("Engine.dll");
    dt_stack_floor = 0; /* the whole stack region is compared, below the final ESP too */
    dt_region(STACK_LO, STACK_LEN);
    dt_region(PRIM, 0x100u);
    dt_region(WRAP, 0x200u);
    dt_region(DECL1, 0x40u);
    dt_region(CACHE, 0x100u);
    dt_region(POOL, 0x1000u);
    dt_region(ARGELEMS, 0x100u);
    dt_region(LOCKBUF, LOCKLEN);
    dt_region(LOG, LOG_SIZE);
    dt_region(0x10862d20u, 0x10u);
    dt_region(0x1086db90u, 0x10u);

    test_exec(300000);
    test_off(2000);
    test_cvd(200000);
    test_svd(50000);
    printf("Execute: true/false %u/%u, cut-declaration lookups %u, COLOR0 fix-ups %u, error blocks %u\n",
           exits[1], exits[0], cut_draws, color_paths, err_paths);
    printf("Execute calls made per case:");
    for (unsigned k = 0; k < 64; ++k)
        if (calls_hist[k])
            printf(" %u:%u", k, calls_hist[k]);
    printf("\nCreateVertexDeclaration: hits %u, misses %u, null device %u\n", cvd_hits, cvd_miss, cvd_null);
    printf("SetVertexDeclaration calls made: 0:%u 1:%u 2:%u more:%u\n", svd_calls[0], svd_calls[1], svd_calls[2],
           svd_calls[3]);
    if (getenv("G3N_BENCH"))
        bench();
    return dt_report("d3d_test");
}
