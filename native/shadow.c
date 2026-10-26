/* native/shadow.c - the shadow pass's sort: Engine's CRT qsort, natively.
 *
 * docs/perf/shadow-maps.md has the reconnaissance. The sun's detail shadow map (LsPSM) is drawn
 * every frame; its draw list is sorted by eCFrustumDatabase::ExecuteShadowMap 10402220 with
 * Engine's static CRT qsort 10564ec0 and the comparator 103fd240 (the float at +0x18 of each
 * render context). The same qsort sorts the main frustum database's lists (comparator 103fd1c0),
 * the vertex buffer pool (1043abd0, 1043ab50) and the particles (103af9c0). The translation swaps
 * bytes one at a time through guest memory and goes through an indirect call for every
 * comparison; together that is about 3.3% of a frame in the NoMenu world (work/hostprof-h3-1.txt).
 *
 * g3n_10564ec0 is the same algorithm as the original (VC8 qsort: shortsort 10564e30 below nine
 * elements, median of three, the two-sided partition, its 30-entry stack), instruction for
 * instruction minus EFLAGS: every comparison is made with the same arguments in the same order,
 * every swap is the same, and EAX/ECX/EDX end as the original leaves them. The guest stack frame
 * is built as the original builds it (saved registers, lo/hi, the stack pointer and arrays, the
 * shortsort frame), so a comparator called through the guest sees the same ESP, arguments,
 * return address and registers. Three comparators are pure leaves and run inline: 103fd240 and
 * 103fd1c0 ending in the x87 state the translation leaves (the two slots below TOP empty and
 * holding the keys, IE on a NaN, the last FCOMI's EFLAGS), 1043abd0 in integers. Any other comparator is called through the guest, through a
 * per-site inline cache as the translation does, except the particles' depth comparator 103af9c0
 * under G3OPT(PDO) or G3OPT(EXEC), which native/pdo.c runs inline with one key per element (it
 * replaced exec.c's per-comparison version). Mod hooks are not consulted (as in vector.c).
 *
 * The error paths (base 0 with elements, width 0, comparator 0) go to the original. */
#include "chunk.h"
#include "g3_opt.h"

void fn_10564ec0(X86 *c); /* Engine _qsort (cdecl: base, num, width, comp) */

#define CMP_ASC18 0x103fd240u  /* ascending by the float at (*elem)+0x18 (ExecuteShadowMap) */
#define CMP_DESC18 0x103fd1c0u /* descending by the float at (*elem)+0x18 (SortInverseZ, Execute) */
#define CMP_INT64 0x1043abd0u  /* (*a)+0x64 - (*b)+0x64 (vertex buffer pool) */
#define CMP_DEPTH 0x103af9c0u  /* the particles far to near (DepthSortParticles), native/pdo.c */

/* native/pdo.c: the depth comparator inline, with one key per element for the sort in progress;
 * begin returns 0 when its groups are off or an import it computes in place is hooked */
uint32_t g3n_pdo_cmp_depth(X86 *c, uint32_t first, uint32_t second, uint32_t *ecx);
int g3n_pdo_sort_begin(uint32_t base, uint32_t num, uint32_t width);
void g3n_pdo_sort_swap(uint32_t a, uint32_t b);
void g3n_pdo_sort_end(X86 *c);
static int qs_depth_keys; /* the sort in progress is the depth sort with pdo's keys */

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t comp;
} QS;

static inline void qs_out(X86 *c, const QS *q) {
    c->r[0] = q->eax;
    c->r[1] = q->ecx;
    c->r[2] = q->edx;
    c->r[3] = q->ebx;
    c->r[4] = q->esp;
    c->r[5] = q->ebp;
    c->r[6] = q->esi;
    c->r[7] = q->edi;
}

static inline void qs_in(X86 *c, QS *q) {
    q->eax = c->r[0];
    q->ecx = c->r[1];
    q->edx = c->r[2];
    q->ebx = c->r[3];
    q->esp = c->r[4];
    q->ebp = c->r[5];
    q->esi = c->r[6];
    q->edi = c->r[7];
}

/* 103fd240 / 103fd1c0: FLD [a+0x18] / FSTP [ESP+4] / FLD [b+0x18] / FSTP [ESP+8] / FLD [ESP+8] /
 * FLD [ESP+4] / FCOMI, then either two FSTP ST0, or FXCH / FCOMIP / FSTP ST0. The argument slots
 * are below the caller's ESP once it pops them, so the round trip through them is the float
 * itself. EAX is the result, ECX the second element's word, EDX untouched. */
static inline void cmp_key18(X86 *c, QS *q, uint32_t first, uint32_t second, int desc) {
    const uint32_t a = rd32(first);
    const uint32_t b = rd32(second);
    q->ecx = b;
#ifdef SHADOW_X87_HELPERS /* the translation's own sequence, for reference */
    fpush(c, (double)rdf32(a + 0x18u));
    const float fa = fto_float(c, ST(c, 0));
    fdrop(c);
    fpush(c, (double)rdf32(b + 0x18u));
    const float fb = fto_float(c, ST(c, 0));
    fdrop(c);
    fpush(c, (double)fb);
    fpush(c, (double)fa);
    fcomi(c, ST(c, 0), ST(c, 1));
    if (!(c->eflags_cf || c->eflags_zf)) {
        fdrop(c);
        fdrop(c);
        q->eax = desc ? 0xffffffffu : 1u;
        return;
    }
    fxch(c, 1);
    fcomi(c, ST(c, 0), ST(c, 1));
    fdrop(c);
    fdrop(c);
    if (!(c->eflags_cf || c->eflags_zf))
        q->eax = desc ? 1u : 0xffffffffu;
    else
        q->eax = 0u;
#else
    /* The same end state written directly: the two slots below TOP were pushed into and popped,
     * so they are empty and hold the two keys (b over a, or a over b after the FXCH), TOP is as
     * on entry, IE is raised by FCOMI on a NaN, EFLAGS are those of the last FCOMI. A float
     * loaded and stored back as a float is itself (the FSTP/FLD round trip through the argument
     * slots; no signalling NaN reaches here, FLD has quieted it, which (double) does too). */
    const double va = (double)rdf32(a + 0x18u), vb = (double)rdf32(b + 0x18u);
    const unsigned p1 = (c->fpu_top - 1u) & 7u, p2 = (c->fpu_top - 2u) & 7u;
    c->st_exact[p1] = 0;
    c->st_exact[p2] = 0;
    c->fpu_tag = (uint16_t)(c->fpu_tag | (3u << (2u * p1)) | (3u << (2u * p2)));
    c->eflags_of = c->eflags_sf = c->eflags_af = 0;
    if (va > vb) {
        c->st[p1] = vb;
        c->st[p2] = va;
        c->eflags_pf = c->eflags_zf = c->eflags_cf = 0;
        q->eax = desc ? 0xffffffffu : 1u;
        return;
    }
    c->st[p1] = va;
    c->st[p2] = vb;
    const uint64_t t = c->st_bits[p1];
    c->st_bits[p1] = c->st_bits[p2];
    c->st_bits[p2] = t;
    if (vb > va) {
        c->eflags_pf = c->eflags_zf = c->eflags_cf = 0;
        q->eax = desc ? 1u : 0xffffffffu;
        return;
    }
    if (va != va || vb != vb) {
        c->eflags_pf = c->eflags_zf = c->eflags_cf = 1;
        c->fpu_sw |= 0x0001u;
    } else {
        c->eflags_pf = 0;
        c->eflags_zf = 1;
        c->eflags_cf = 0;
    }
    q->eax = 0u;
#endif
}

/* PUSH second / PUSH first / CALL comp (continuation `ret`) / ADD ESP,8. */
static inline void qs_cmp(X86 *c, QS *q, uint32_t first, uint32_t second, uint32_t ret, uint64_t *ic) {
    switch (q->comp) {
    case CMP_ASC18:
        cmp_key18(c, q, first, second, 0);
        return;
    case CMP_DESC18:
        cmp_key18(c, q, first, second, 1);
        return;
    case CMP_INT64: {
        const uint32_t a = rd32(first);
        q->edx = second;
        const uint32_t b = rd32(second);
        q->ecx = rd32(a + 0x64u) - rd32(b + 0x64u);
        q->eax = q->ecx;
        return;
    }
    case CMP_DEPTH: /* pdo.c, under G3OPT(PDO) or G3OPT(EXEC); else the guest call */
        if (qs_depth_keys) {
            q->eax = g3n_pdo_cmp_depth(c, first, second, &q->ecx);
            return;
        }
        break;
    default:
        break;
    }
    q->esp -= 4u;
    wr32(q->esp, second);
    q->esp -= 4u;
    wr32(q->esp, first);
    q->esp -= 4u;
    wr32(q->esp, ret);
    qs_out(c, q);
    recomp_call_cached(c, q->comp, ic);
    qs_in(c, q);
    q->esp += 8u;
}

/* Swaps `n` bytes at a and b (distinct elements, never overlapping); returns the last byte that
 * was at `last_from` (one of a, b) before the swap, as the byte loops leave it in DL. */
static inline uint32_t qs_swap(uint32_t a, uint32_t b, uint32_t n, uint32_t last_from) {
    const uint32_t last = rd8(last_from + n - 1u);
    if (qs_depth_keys)
        g3n_pdo_sort_swap(a, b);
    if (n == 4u) {
        const uint32_t va = rd32(a), vb = rd32(b);
        wr32(a, vb);
        wr32(b, va);
    } else if (n <= 512u && !g_store_hooks && a >= 0x10000u && b >= 0x10000u) {
        /* No watch armed and no dirty range open, so the stores report nothing and the byte
         * order of the swap cannot be seen: the elements never overlap, swap them whole (the
         * particles' 0xb4-byte elements, about two thirds of their sort in the byte loop). */
        uint8_t t[512];
        memcpy(t, g_mem + a, n);
        memcpy(g_mem + a, g_mem + b, n);
        memcpy(g_mem + b, t, n);
    } else if ((n & 3u) == 0u) {
        /* word by word: the same bytes end in the same places (the elements never overlap) */
        for (uint32_t i = 0; i < n; i += 4u) {
            const uint32_t va = rd32(a + i), vb = rd32(b + i);
            wr32(a + i, vb);
            wr32(b + i, va);
        }
    } else {
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t x = rd8(b + i), y = rd8(a + i);
            wr8(a + i, x);
            wr8(b + i, y);
        }
    }
    return last;
}

/* swap 10564e00 (EAX = a, ECX = b, EDX = width; PUSH ESI / PUSH EBX ... POP). Its two pushes and
 * the return address lie below ESP and are overwritten by the next call's pushes before anything
 * reads them, so they are not written. */
static inline void qs_swap_call(QS *q) {
    const uint32_t a = q->eax, b = q->ecx, n = q->edx;
    if (a == b || n == 0u)
        return;
    const uint32_t dl = qs_swap(a, b, n, a);
    q->eax = a + n;
    q->ecx = b + n;
    q->edx = (n & 0xffffff00u) | dl;
}

/* shortsort 10564e30 (lo, hi, width, comp), called with its arguments pushed and the return
 * address at ESP. */
static void qs_shortsort(X86 *c, QS *q) {
    static uint64_t ic_e62;
    const uint32_t s = q->esp;
    q->ecx = rd32(s + 4u);
    q->esp -= 4u;
    wr32(q->esp, q->ebp);
    q->ebp = rd32(q->esp + 0xcu);
    if (q->ebp <= q->ecx)
        goto out1;
    q->esp -= 4u;
    wr32(q->esp, q->ebx);
    q->ebx = rd32(q->esp + 0x14u);
    q->esp -= 4u;
    wr32(q->esp, q->esi);
    q->eax = q->ecx + q->ebx;
    q->esp -= 4u;
    wr32(q->esp, q->edi);
    wr32(q->esp + 0x18u, q->eax);
    for (;;) {
        q->esi = rd32(q->esp + 0x18u);
        q->edi = q->ecx;
        if (q->esi <= q->ebp) {
            do {
                q->comp = rd32(q->esp + 0x20u);
                qs_cmp(c, q, q->esi, q->edi, 0x10564e66u, &ic_e62);
                if ((int32_t)q->eax > 0)
                    q->edi = q->esi;
                q->esi += q->ebx;
            } while (q->esi <= q->ebp);
            q->ecx = rd32(q->esp + 0x14u);
        }
        q->esi = q->ebx;
        q->eax = q->ebp;
        if (q->edi != q->ebp && q->ebx != 0u) {
            const uint32_t n = q->ebx;
            /* MOV BL,[EAX] / MOV DL,[ECX+EAX] / MOV [ECX+EAX],BL / MOV [EAX],DL: DL ends as the
             * last byte of the maximum, BL as the last of the top (EBX is reloaded below). */
            const uint32_t dl = qs_swap(q->ebp, q->edi, n, q->edi);
            q->edx = (q->edx & 0xffffff00u) | dl;
            q->ecx = q->edi - q->ebp;
            q->eax = q->ebp + n;
            q->esi = 0u;
            q->ebx = rd32(q->esp + 0x1cu);
            q->ecx = rd32(q->esp + 0x14u);
        }
        q->ebp -= q->ebx;
        if (!(q->ebp > q->ecx))
            break;
    }
    q->edi = rd32(q->esp);
    q->esp += 4u;
    q->esi = rd32(q->esp);
    q->esp += 4u;
    q->ebx = rd32(q->esp);
    q->esp += 4u;
out1:
    q->ebp = rd32(q->esp);
    q->esp += 4u;
    q->esp += 4u; /* RET */
}

/* void qsort(void *base, size_t num, size_t width, int (*comp)(const void *, const void *)), cdecl. */
void g3n_10564ec0(X86 *c) {
    if (!G3OPT(SHADOW)) {
        fn_10564ec0(c);
        return;
    }
    const uint32_t e = c->r[4];
    {
        const uint32_t base = rd32(e + 4u), num = rd32(e + 8u);
        if ((base == 0u && num != 0u) || rd32(e + 0xcu) == 0u || rd32(e + 0x10u) == 0u) {
            fn_10564ec0(c); /* _invalid_parameter */
            return;
        }
    }
    static uint64_t ic_fbb, ic_fd6, ic_ff1, ic_01c, ic_03a, ic_050, ic_0b8, ic_0dc;
    qs_depth_keys = rd32(e + 0x10u) == CMP_DEPTH && g3n_pdo_sort_begin(rd32(e + 4u), rd32(e + 8u), rd32(e + 0xcu));
    QS q;
    qs_in(c, &q);
    q.esp -= 0x100u;
    q.esp -= 4u;
    wr32(q.esp, q.ebx);
    q.ebx = rd32(q.esp + 0x108u);
    q.esp -= 4u;
    wr32(q.esp, q.esi);
    q.esi = rd32(q.esp + 0x110u);
    q.esp -= 4u;
    wr32(q.esp, q.ebp);
    q.ebp = rd32(q.esp + 0x118u);
    if (q.esi < 2u)
        goto out3;
    q.esi = (q.esi - 1u) * q.ebp + q.ebx;
    wr32(q.esp + 0x18u, 0u);
    wr32(q.esp + 0xcu, q.ebx);
    wr32(q.esp + 0x10u, q.esi);
    q.esp -= 4u;
    wr32(q.esp, q.edi);

recurse:
    q.eax = q.esi - q.ebx;
    q.edx = q.eax % q.ebp; /* DIV EBP, EDX = 0 */
    q.eax = q.eax / q.ebp + 1u;
    if (q.eax <= 8u) {
        q.eax = rd32(q.esp + 0x120u);
        q.esp -= 4u;
        wr32(q.esp, q.eax);
        q.esp -= 4u;
        wr32(q.esp, q.ebp);
        q.esp -= 4u;
        wr32(q.esp, q.esi);
        q.esp -= 4u;
        wr32(q.esp, q.ebx);
        q.esp -= 4u;
        wr32(q.esp, 0x10564f83u);
        qs_shortsort(c, &q);
        q.esp += 0x10u;
        goto pop;
    }
    q.eax = (q.eax >> 1) * q.ebp + q.ebx;
    q.edi = q.eax;
    q.comp = rd32(q.esp + 0x120u);
    qs_cmp(c, &q, q.ebx, q.edi, 0x10564fc2u, &ic_fbb);
    if ((int32_t)q.eax > 0) {
        q.edx = q.ebp;
        q.ecx = q.edi;
        q.eax = q.ebx;
        qs_swap_call(&q);
    }
    q.comp = rd32(q.esp + 0x120u);
    qs_cmp(c, &q, q.ebx, q.esi, 0x10564fddu, &ic_fd6);
    if ((int32_t)q.eax > 0) {
        q.edx = q.ebp;
        q.ecx = q.esi;
        q.eax = q.ebx;
        qs_swap_call(&q);
    }
    q.comp = rd32(q.esp + 0x120u);
    qs_cmp(c, &q, q.edi, q.esi, 0x10564ff8u, &ic_ff1);
    if ((int32_t)q.eax > 0) {
        q.edx = q.ebp;
        q.ecx = q.esi;
        q.eax = q.edi;
        qs_swap_call(&q);
    }

part: /* 10565010 */
    if (q.edi > q.ebx) {
        for (;;) {
            q.ebx += q.ebp;
            if (q.ebx >= q.edi)
                goto up; /* 10565030 */
            q.comp = rd32(q.esp + 0x120u);
            qs_cmp(c, &q, q.ebx, q.edi, 0x10565023u, &ic_01c);
            if ((int32_t)q.eax > 0)
                break;
        }
        if (q.edi > q.ebx)
            goto down;
    }
up: /* 10565030 */
    q.ebx += q.ebp;
    if (q.ebx > rd32(q.esp + 0x14u))
        goto down;
    q.comp = rd32(q.esp + 0x120u);
    qs_cmp(c, &q, q.ebx, q.edi, 0x10565041u, &ic_03a);
    if ((int32_t)q.eax <= 0)
        goto up;
down: /* 10565048 */
    q.esi -= q.ebp;
    if (q.esi > q.edi) {
        q.comp = rd32(q.esp + 0x120u);
        qs_cmp(c, &q, q.esi, q.edi, 0x10565057u, &ic_050);
        if ((int32_t)q.eax > 0)
            goto down;
    }
    if (q.ebx <= q.esi) { /* 1056505e */
        q.ecx = q.ebp;
        q.eax = q.esi;
        if (q.ebx != q.esi) {
            /* the inline swap of [ESI] and [EBX]: EDX and the byte at [ESP+0x1b] end as the last
             * byte of the element at EBX, zero-extended */
            const uint32_t n = q.ecx;
            const uint32_t last = qs_swap(q.esi, q.ebx, n, q.ebx);
            wr8(q.esp + 0x1bu, (uint8_t)last);
            q.edx = last;
            q.eax = q.esi + n;
            q.ecx = 0u;
            q.ebp = rd32(q.esp + 0x11cu);
        }
        if (q.edi == q.esi)
            q.edi = q.ebx;
        goto part;
    }
    /* 105650a5 */
    q.esi += q.ebp;
    if (q.edi < q.esi) {
        for (;;) {
            q.esi -= q.ebp;
            if (q.esi <= q.edi)
                goto left; /* 105650d0 */
            q.comp = rd32(q.esp + 0x120u);
            qs_cmp(c, &q, q.esi, q.edi, 0x105650bfu, &ic_0b8);
            if (q.eax != 0u)
                break;
        }
        if (q.edi < q.esi)
            goto lo_eax; /* 105650ea */
    }
left: /* 105650d0 */
    for (;;) {
        q.eax = rd32(q.esp + 0x10u);
        q.esi -= q.ebp;
        if (q.esi <= q.eax)
            goto sizes; /* 105650ee */
        q.comp = rd32(q.esp + 0x120u);
        qs_cmp(c, &q, q.esi, q.edi, 0x105650e3u, &ic_0dc);
        if (q.eax != 0u)
            break;
    }
lo_eax: /* 105650ea */
    q.eax = rd32(q.esp + 0x10u);
sizes: /* 105650ee */
    q.edx = rd32(q.esp + 0x14u);
    q.ecx = q.edx - q.ebx;
    q.edi = q.esi - q.eax;
    if (!((int32_t)q.edi < (int32_t)q.ecx)) {
        if (q.eax < q.esi) {
            q.ecx = rd32(q.esp + 0x1cu);
            wr32(q.esp + q.ecx * 4u + 0x20u, q.eax);
            wr32(q.esp + q.ecx * 4u + 0x98u, q.esi);
            q.ecx += 1u;
            wr32(q.esp + 0x1cu, q.ecx);
        }
        if (q.ebx >= q.edx)
            goto pop;
        q.esi = rd32(q.esp + 0x14u);
        wr32(q.esp + 0x10u, q.ebx);
        goto recurse;
    }
    if (q.ebx < q.edx) { /* 1056512d */
        q.ecx = rd32(q.esp + 0x1cu);
        wr32(q.esp + q.ecx * 4u + 0x20u, q.ebx);
        wr32(q.esp + q.ecx * 4u + 0x98u, q.edx);
        q.ecx += 1u;
        wr32(q.esp + 0x1cu, q.ecx);
    }
    if (q.eax >= q.esi)
        goto pop;
    q.ebx = rd32(q.esp + 0x10u);
    wr32(q.esp + 0x14u, q.esi);
    goto recurse;

pop: /* 10564f86 */
    q.eax = rd32(q.esp + 0x1cu) - 1u;
    wr32(q.esp + 0x1cu, q.eax);
    if ((int32_t)q.eax >= 0) {
        q.edx = rd32(q.esp + q.eax * 4u + 0x20u);
        q.eax = rd32(q.esp + q.eax * 4u + 0x98u);
        wr32(q.esp + 0x10u, q.edx);
        wr32(q.esp + 0x14u, q.eax);
        q.esi = q.eax;
        q.ebx = q.edx;
        goto recurse;
    }
    q.edi = rd32(q.esp);
    q.esp += 4u;
out3: /* 1056515d */
    q.ebp = rd32(q.esp);
    q.esp += 4u;
    q.esi = rd32(q.esp);
    q.esp += 4u;
    q.ebx = rd32(q.esp);
    q.esp += 4u;
    q.esp += 0x100u;
    qs_out(c, &q);
    if (qs_depth_keys) {
        g3n_pdo_sort_end(c);
        qs_depth_keys = 0;
    }
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u;
    recomp_return(c);
}
