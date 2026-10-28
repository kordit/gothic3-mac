/* native/ai.c - SharedBase's hot functions under the AI's fight code: _strrchr and the vector geometry.
 *
 * The group began with the speech file lookup in a fight (gCScriptRoutine_PS::OnProcess -> StartSaySVM ->
 * ... -> eCVirtualFileSystem::GetFileSize -> FileSystem FindFirstFileA), whose name match and directory entry test
 * lived here until FileSystem.dll went native whole (D-043, native/filesystem.cpp). What stays, natively and
 * exactly as the translation runs it (the same registers, the same guest stack, the same writes):
 *   g3n_1006b740  SharedBase _strrchr (REPNE SCASB forwards, then backwards);
 *   and below, bCVector2::Normalize, the 2D magnitudes, bCVector::GetMagnitude and bCInfCylinder's
 *   Cull and Intersects, which CanEvade runs.
 * Each runs its native version only under G3OPT(AI); otherwise it calls the original. */
#include "chunk.h"
#include "g3_opt.h"

#include <math.h>
#include <string.h>

void fn_1006b740(X86 *c); /* SharedBase _strrchr, cdecl */

/* char *_strrchr(const char *s, int ch), cdecl: PUSH EBP / MOV EBP,ESP / PUSH EDI, REPNE SCASB to
 * the NUL with ECX = -1, then STD and REPNE SCASB backwards from the NUL with ECX = length + 1
 * for the low byte of ch, CLD. Leaves EAX = the last match (the NUL counts) or 0, ECX = its offset
 * (0 when there is none), EDX untouched, EDI and EBP restored, DF clear. A call with DF already
 * set (never, in MSVC code) takes the original. */
void g3n_1006b740(X86 *c) {
    if (!G3OPT(AI) || c->eflags_df) {
        fn_1006b740(c);
        return;
    }
    uint32_t *const r = c->r;
    const uint32_t esp = r[4];
    const uint32_t s = rd32(esp + 4u);
    const uint8_t ch = rd8(esp + 8u);
    wr32(esp - 4u, r[5]);
    wr32(esp - 8u, r[7]);
    (void)rd8(s); /* the first SCASB's read (a null page faults there) */
    const uint32_t len = (uint32_t)strlen((const char *)(g_mem + s));
    uint32_t p = 0u, off = 0u;
    if (ch == 0) {
        p = s + len;
        off = len;
    } else {
        const uint8_t *const b = g_mem + s;
        for (uint32_t i = len; i-- > 0;)
            if (b[i] == ch) {
                off = i;
                p = s + i;
                break;
            }
    }
    r[0] = p;
    r[1] = off;
    c->eflags_df = 0;
    /* the flags of the last CMP byte [EDI],AL: nothing reads them after the RET */
    c->eip = rd32(esp);
    r[4] = esp + 4u;
    recomp_return(c);
}

/* ---- CanEvade's geometry: bCVector2::Normalize --------------------------------------------- */

void fn_1000f810(X86 *c); /* SharedBase bCVector2::Normalize, thiscall, RET */

#define SB_SQRT_FASTFLAG 0x100d4118u /* the CRT sqrt goes to its other exit when this is set */
#define SB_SQRT_NAME 0x100d3c50u     /* "sqrt", ECX as the CRT's __math_exit leaves it */
#define SB_SQRT_RET2 0x10085590u     /* after CALL 0x1008559d */
#define SB_SQRT_RET3 0x100855b5u     /* after CALL 0x10086855 (control word other than 0x27f) */
#define SB_NORMALIZE2_SQRT_RET 0x1000f846u

static inline void ai_ffree_below(X86 *c, unsigned n) {
    for (unsigned i = 1; i <= n; ++i) {
        const unsigned p = (c->fpu_top - i) & 7u;
        ftag_put(c, p, FTAG_EMPTY);
        c->st_exact[p] = 0;
    }
}

static inline uint32_t ai_f2u(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* bCVector2 const &bCVector2::Normalize(), thiscall, RET; EAX = this. Frame: PUSH EBP, EBP =
 * ESP0-4, ESP aligned to 8, 0xc bytes, PUSH ESI; sp is the ESP after that. y and x go through
 * float slots (+8, +4), x*x + y*y through the slot +0xc into the CRT's sqrt (CALL 0x10085580),
 * the length through +0xc twice; FUCOMIP 0, length: a length that is not 0 (a NaN too) scales x
 * and y by the float 1/length (in +0xc).
 *
 * The sqrt is done here as in leaf.c's bCVector::Normalize, for its common path: a non-negative
 * argument that is not a NaN, the CRT's fast flag clear, and no pending precision exception its
 * exit would report; FSQRT under (cw & 0x300) | 0x7f with cw restored, ECX = the name "sqrt",
 * EDX the control word slot. The other cases run the whole original, decided before anything
 * is written. */
void g3n_1000f810(X86 *c) {
    if (!G3OPT(AI)) {
        fn_1000f810(c);
        return;
    }
    const uint32_t self = c->r[1];
    const uint32_t esp0 = c->r[4];
    const uint32_t ebp = esp0 - 4u;
    const uint32_t sp = ((ebp & 0xfffffff8u) - 0xcu) - 4u;
    /* the frame down to the sqrt's deepest slot: [sp-0x1c, esp0) */
    if ((uint64_t)(sp - 0x1cu) < (uint64_t)self + 8u && (uint64_t)self < (uint64_t)esp0) {
        fn_1000f810(c);
        return;
    }
    const uint16_t sw0 = c->fpu_sw;
    const float fy = fto_float(c, (double)rdf32(self + 4u));
    const float fx = fto_float(c, (double)rdf32(self));
    const double xx = fx87(c, (double)fx * (double)fx);
    const double yy = fx87(c, (double)fy * (double)fy);
    const float s = fto_float(c, fx87(c, yy + xx));
    const uint32_t sb = ai_f2u(s);
    const uint16_t cw = c->fpu_cw;
    if ((sb & 0x80000000u) || ((sb & 0x7f800000u) == 0x7f800000u && (sb & 0x007fffffu)) ||
        rd32(SB_SQRT_FASTFLAG) != 0 || (cw != 0x027fu && (cw & 0x20u) && (c->fpu_sw & 0x20u))) {
        c->fpu_sw = sw0;
        fn_1000f810(c);
        return;
    }
    const uint32_t edx0 = c->r[2];
    const double r = sb == 0x7f800000u ? (double)s : fx87(c, sqrt((double)s));
    const float len = fto_float(c, r);
    wr32(sp - 4u, SB_NORMALIZE2_SQRT_RET);
    wrf64(sp - 0x10u, (double)s);
    wr32(sp - 0x14u, SB_SQRT_RET2);
    uint32_t edx;
    if (cw != 0x027fu && sb != 0x7f800000u) {
        edx = ((uint32_t)((cw & 0x300u) | 0x7fu) << 16) | cw;
        wr32(sp - 0x1cu, SB_SQRT_RET3);
    } else {
        edx = (edx0 & 0xffff0000u) | cw;
    }
    wr32(sp - 0x18u, edx);
    c->r[1] = SB_SQRT_NAME;
    c->r[2] = edx;
    c->r[0] = self;
    /* FLD ST0 / FLDZ / FUCOMIP ST0,ST1 / FSTP ST0 / LAHF / TEST AH,0x44 / JP */
    fucomi(c, 0.0, (double)len);
    float last = len;
    if (c->eflags_pf || !c->eflags_zf) {
        const float inv = fto_float(c, fx87(c, fdivz(c, 1.0, (double)len)));
        wrf32(self, fto_float(c, fx87(c, (double)fx * (double)inv)));
        wrf32(self + 4u, fto_float(c, fx87(c, (double)inv * (double)fy)));
        last = inv;
    }
    /* TEST AH,0x44 leaves PF = parity of ZF|PF; nothing reads the flags after the RET */
    wr32(ebp, c->r[5]);
    wr32(sp, c->r[6]);
    wrf32(sp + 4u, fx);
    wrf32(sp + 8u, fy);
    wrf32(sp + 0xcu, last);
    ai_ffree_below(c, 3); /* the deepest point is three (y, x*x, y), the sqrt adds none */
    c->eip = rd32(esp0);
    c->r[4] = esp0 + 4u;
    recomp_return(c);
}

/* ---- bCVector::GetSquareMagnitude2D / GetMagnitude2D --------------------------------------- */

void fn_10010e50(X86 *c); /* SharedBase bCVector::GetSquareMagnitude2D(bECoordinate), RET 4, float in ST0 */
void fn_10010c90(X86 *c); /* SharedBase bCVector::GetMagnitude2D(bECoordinate), RET 4, float in ST0 */

#define SB_MAG2D_SQ_RET 0x10010ca2u   /* GetMagnitude2D's continuation after CALL 10010e50 */
#define SB_MAG2D_SQRT_RET 0x10010ca7u /* and after CALL sqrt */

/* The square of the 2D length for coordinate `coord` (0: y,z; 1: x,z; 2: x,y), the float the
 * original stores to [ESP+4] and loads back. Products in the original's order. 0 for another
 * coordinate (the original's assertion path). */
static inline int ai_sq2d(X86 *c, uint32_t self, uint32_t coord, float *out) {
    const double x = (double)rdf32(self), y = (double)rdf32(self + 4u), z = (double)rdf32(self + 8u);
    double s;
    if (coord == 1u) {        /* FLD z / FLD x / FMUL ST0 / FLD ST1 / FMULP ST2 / FADDP */
        const double xx = fx87(c, x * x);
        const double zz = fx87(c, z * z);
        s = fx87(c, zz + xx);
    } else if (coord == 0u) { /* the same with y for x */
        const double yy = fx87(c, y * y);
        const double zz = fx87(c, z * z);
        s = fx87(c, zz + yy);
    } else if (coord == 2u) { /* FLD y / FLD x / FLD ST1 / FMULP ST2 / FMUL ST0 / FADDP */
        const double yy = fx87(c, y * y);
        const double xx = fx87(c, x * x);
        s = fx87(c, yy + xx);
    } else {
        return 0;
    }
    *out = fto_float(c, s);
    return 1;
}

/* float bCVector::GetSquareMagnitude2D(bECoordinate) const, thiscall, RET 4: EAX = the
 * coordinate, the float through the argument's slot, left in ST0 (three slots used). */
void g3n_10010e50(X86 *c) {
    const uint32_t esp = c->r[4];
    const uint32_t coord = rd32(esp + 4u);
    if (!G3OPT(AI) || coord > 2u) {
        fn_10010e50(c);
        return;
    }
    float s;
    ai_sq2d(c, c->r[1], coord, &s);
    c->r[0] = coord;
    wrf32(esp + 4u, s);
    ai_ffree_below(c, 3);
    fpush(c, (double)rdf32(esp + 4u));
    c->eip = rd32(esp);
    c->r[4] = esp + 8u;
    recomp_return(c);
}

/* float bCVector::GetMagnitude2D(bECoordinate) const, thiscall, RET 4. Frame: PUSH EBP, EBP =
 * ESP0-4, ESP aligned to 8 less 8 (sp); PUSH coord, CALL GetSquareMagnitude2D (its float into
 * that pushed slot), CALL the CRT sqrt at ESP = sp, FSTP float [sp+4] / FLD it. The sqrt's
 * common path as in g3n_1000f810; EAX after it: MOV AX,[cw slot] over the double's high dword
 * (zero for +inf, whose path ANDs it with the sign), then AND AX,0x20 when cw is not 0x27f,
 * where FSTSW's PE (never modelled, so clear) leaves AX = 0. Other paths: the original. */
void g3n_10010c90(X86 *c) {
    const uint32_t esp0 = c->r[4];
    const uint32_t coord = rd32(esp0 + 4u);
    if (!G3OPT(AI) || coord > 2u) {
        fn_10010c90(c);
        return;
    }
    const uint32_t self = c->r[1];
    const uint32_t ebp = esp0 - 4u;
    const uint32_t sp = (ebp & 0xfffffff8u) - 8u;
    /* the vector inside the frame the pushes and the sqrt write: the original */
    if ((uint64_t)(sp - 0x1cu) < (uint64_t)self + 12u && (uint64_t)self < (uint64_t)esp0) {
        fn_10010c90(c);
        return;
    }
    const uint16_t sw0 = c->fpu_sw;
    float s;
    ai_sq2d(c, self, coord, &s);
    const uint32_t sb = ai_f2u(s);
    const uint16_t cw = c->fpu_cw;
    if ((sb & 0x80000000u) || ((sb & 0x7f800000u) == 0x7f800000u && (sb & 0x007fffffu)) ||
        rd32(SB_SQRT_FASTFLAG) != 0 || (cw != 0x027fu && (cw & 0x20u) && (c->fpu_sw & 0x20u))) {
        c->fpu_sw = sw0;
        fn_10010c90(c);
        return;
    }
    const uint32_t edx0 = c->r[2];
    const double r = sb == 0x7f800000u ? (double)s : fx87(c, sqrt((double)s));
    const float len = fto_float(c, r);
    wr32(ebp, c->r[5]);
    wr32(sp - 8u, SB_MAG2D_SQ_RET);
    wr32(sp - 4u, SB_MAG2D_SQRT_RET); /* over the square's slot */
    wrf64(sp - 0x10u, (double)s);
    wr32(sp - 0x14u, SB_SQRT_RET2);
    uint32_t edx;
    if (cw != 0x027fu && sb != 0x7f800000u) {
        edx = ((uint32_t)((cw & 0x300u) | 0x7fu) << 16) | cw;
        wr32(sp - 0x1cu, SB_SQRT_RET3);
    } else {
        edx = (edx0 & 0xffff0000u) | cw;
    }
    wr32(sp - 0x18u, edx);
    uint64_t db;
    const double sd = (double)s;
    memcpy(&db, &sd, 8);
    const uint32_t high = sb == 0x7f800000u ? 0u : (uint32_t)(db >> 32) & 0xffff0000u;
    c->r[0] = high | (cw == 0x027fu ? 0x027fu : 0u);
    c->r[1] = SB_SQRT_NAME;
    c->r[2] = edx;
    wrf32(sp + 4u, len);
    ai_ffree_below(c, 3);
    fpush(c, (double)rdf32(sp + 4u));
    c->eip = rd32(esp0);
    c->r[4] = esp0 + 8u;
    recomp_return(c);
}

/* ---- the CRT sqrt as a CALL from a frame whose ESP is sp (shared by the functions below) ---- */

/* The sqrt's common path applies to the float argument with bits sb (see g3n_1000f810). */
static inline int ai_sqrt_common(const X86 *c, uint32_t sb) {
    const uint16_t cw = c->fpu_cw;
    return !((sb & 0x80000000u) || ((sb & 0x7f800000u) == 0x7f800000u && (sb & 0x007fffffu)) ||
             rd32(SB_SQRT_FASTFLAG) != 0 || (cw != 0x027fu && (cw & 0x20u) && (c->fpu_sw & 0x20u)));
}

/* CALL 0x10085580 with s in ST0 at ESP = sp, continuation `ret`: the result (the float of it is
 * what the callers store), the sqrt's frame below sp, EAX, ECX and EDX as it leaves them. */
static inline double ai_sqrt_call(X86 *c, uint32_t sp, float s, uint32_t ret) {
    const uint32_t sb = ai_f2u(s);
    const uint16_t cw = c->fpu_cw;
    const uint32_t edx0 = c->r[2];
    const double r = sb == 0x7f800000u ? (double)s : fx87(c, sqrt((double)s));
    wr32(sp - 4u, ret);
    wrf64(sp - 0x10u, (double)s);
    wr32(sp - 0x14u, SB_SQRT_RET2);
    uint32_t edx;
    if (cw != 0x027fu && sb != 0x7f800000u) {
        edx = ((uint32_t)((cw & 0x300u) | 0x7fu) << 16) | cw;
        wr32(sp - 0x1cu, SB_SQRT_RET3);
    } else {
        edx = (edx0 & 0xffff0000u) | cw;
    }
    wr32(sp - 0x18u, edx);
    uint64_t db;
    const double sd = (double)s;
    memcpy(&db, &sd, 8);
    const uint32_t high = sb == 0x7f800000u ? 0u : (uint32_t)(db >> 32) & 0xffff0000u;
    c->r[0] = high | (cw == 0x027fu ? 0x027fu : 0u);
    c->r[1] = SB_SQRT_NAME;
    c->r[2] = edx;
    return r;
}

/* ---- bCVector::GetMagnitude ----------------------------------------------------------------- */

void fn_10010c50(X86 *c); /* SharedBase bCVector::GetMagnitude, thiscall, RET, float in ST0 */
#define SB_MAG_SQRT_RET 0x10010c7eu

/* float bCVector::GetMagnitude() const, thiscall, RET. Frame: PUSH EBP, EBP = ESP0-4, ESP aligned
 * to 8 less 8 (sp). FLD y, x, z: (y*y + x*x) + z*z in that order (four slots deep), the float
 * through [sp+4] into the CRT sqrt, its float through [sp+4] again into ST0. */
void g3n_10010c50(X86 *c) {
    if (!G3OPT(AI)) {
        fn_10010c50(c);
        return;
    }
    const uint32_t self = c->r[1];
    const uint32_t esp0 = c->r[4];
    const uint32_t ebp = esp0 - 4u;
    const uint32_t sp = (ebp & 0xfffffff8u) - 8u;
    if ((uint64_t)(sp - 0x1cu) < (uint64_t)self + 12u && (uint64_t)self < (uint64_t)esp0) {
        fn_10010c50(c);
        return;
    }
    const uint16_t sw0 = c->fpu_sw;
    const double y = (double)rdf32(self + 4u), x = (double)rdf32(self), z = (double)rdf32(self + 8u);
    const double xx = fx87(c, x * x);
    const double yy = fx87(c, y * y);
    const double xy = fx87(c, yy + xx);
    const double zz = fx87(c, z * z);
    const float s = fto_float(c, fx87(c, xy + zz));
    if (!ai_sqrt_common(c, ai_f2u(s))) {
        c->fpu_sw = sw0;
        fn_10010c50(c);
        return;
    }
    wr32(ebp, c->r[5]);
    const float len = fto_float(c, ai_sqrt_call(c, sp, s, SB_MAG_SQRT_RET));
    wrf32(sp + 4u, len);
    ai_ffree_below(c, 4);
    fpush(c, (double)rdf32(sp + 4u));
    c->eip = rd32(esp0);
    c->r[4] = esp0 + 4u;
    recomp_return(c);
}

/* ---- bCInfCylinder::Cull -------------------------------------------------------------------- */

void fn_10039900(X86 *c); /* SharedBase bCInfCylinder::Cull(bCVector const &, bool), RET 8 */
#define SB_CULL_FACTOR 0x10090ba0u /* double, the radius' factor with the bool set */

/* bool bCInfCylinder::Cull(bCVector const &p, bool) const, thiscall, RET 8; the cylinder is
 * {radius, centre x, y, z}. SUB ESP,0xc (E): px, pz through float slots [E], [E+8], dx = px - cx
 * and dz = pz - cz back through them; (dz*dz + (dx*dx + 0*0)) and r*r (times the double factor,
 * twice, (r*r*k)*k, when the bool byte is 1) each through the bool's argument slot as a float; FCOMIP:
 * AL = r*r > d*d (a NaN: 0). Four x87 slots deep; EAX's upper bytes are the point's address. */
void g3n_10039900(X86 *c) {
    if (!G3OPT(AI)) {
        fn_10039900(c);
        return;
    }
    const uint32_t self = c->r[1];
    const uint32_t esp0 = c->r[4];
    const uint32_t e = esp0 - 0xcu;
    const uint32_t p = rd32(esp0 + 4u);
    const int one = rd8(esp0 + 8u) == 1u;
    /* the point or the cylinder inside the slots written before they are read: the original */
    if ((uint64_t)e < (uint64_t)p + 12u && (uint64_t)p < (uint64_t)esp0 + 12u) {
        fn_10039900(c);
        return;
    }
    if ((uint64_t)e < (uint64_t)self + 16u && (uint64_t)self < (uint64_t)esp0 + 12u) {
        fn_10039900(c);
        return;
    }
    const float px = fto_float(c, (double)rdf32(p));
    const float pz = fto_float(c, (double)rdf32(p + 8u));
    const float dx = fto_float(c, fx87(c, (double)px - (double)rdf32(self + 4u)));
    const float dz = fto_float(c, fx87(c, (double)pz - (double)rdf32(self + 0xcu)));
    const double r = (double)rdf32(self);
    const double dxx = fx87(c, (double)dx * (double)dx);
    const double zero = fx87(c, 0.0 * 0.0);
    const double dxx0 = fx87(c, dxx + zero);
    const double dzz = fx87(c, (double)dz * (double)dz);
    const double d2 = fx87(c, dzz + dxx0);
    const float d2f = fto_float(c, d2);
    const double rr = fx87(c, r * r);
    float rrf;
    if (one) {
        const double k = rdf64(SB_CULL_FACTOR);
        const double rrk = fx87(c, rr * k); /* FMUL ST2: ST2 = ST2 * ST0 (the factor on top) */
        rrf = fto_float(c, fx87(c, rrk * k)); /* FMULP ST2 */
    } else {
        rrf = fto_float(c, rr);
    }
    fcomi(c, (double)rrf, (double)d2f);
    const int above = !c->eflags_cf && !c->eflags_zf;
    wrf32(e, dx);
    wrf32(e + 8u, dz);
    wrf32(esp0 + 8u, rrf);
    c->r[0] = (p & 0xffffff00u) | (above ? 1u : 0u);
    ai_ffree_below(c, 4);
    c->eip = rd32(esp0);
    c->r[4] = esp0 + 12u;
    recomp_return(c);
}

/* ---- bCInfCylinder::Intersects(bCStretch const &, float &, bCVector2 &, bool, bool): its two
 * early "no" exits --------------------------------------------------------------------------- */

void fn_10039d80(X86 *c); /* SharedBase bCInfCylinder::Intersects (stretch), thiscall, RET 0x14 */
#define SB_EPSILON 0x1008b4b0u /* g_fEpsilon */
#define SB_ISECT_NORM_RET 0x10039e08u

/* The host side of g3n_1000f810's common path on {x, y}: whether it applies (else 0) and the
 * vector it leaves. */
static int ai_norm2_value(X86 *c, float x, float y, float *nx, float *ny) {
    const double xx = fx87(c, (double)x * (double)x);
    const double yy = fx87(c, (double)y * (double)y);
    const float s = fto_float(c, fx87(c, yy + xx));
    const uint32_t sb = ai_f2u(s);
    if (!ai_sqrt_common(c, sb))
        return 0;
    const double r = sb == 0x7f800000u ? (double)s : fx87(c, sqrt((double)s));
    const float len = fto_float(c, r);
    *nx = x;
    *ny = y;
    fucomi(c, 0.0, (double)len);
    if (c->eflags_pf || !c->eflags_zf) {
        const float inv = fto_float(c, fx87(c, fdivz(c, 1.0, (double)len)));
        *nx = fto_float(c, fx87(c, (double)x * (double)inv));
        *ny = fto_float(c, fx87(c, (double)inv * (double)y));
    }
    return 1;
}

static inline void ai_movss_load(X86 *c, int reg, uint32_t bits) {
    c->xmm[reg][0] = bits;
    c->xmm[reg][1] = c->xmm[reg][2] = c->xmm[reg][3] = 0u;
}

/* The stretch is {p0 x, y, z, p1 x, y, z}, the cylinder {radius, centre x, y, z}. Frame: PUSH EBP,
 * EBP = ESP0-4, ESP aligned to 8 less 0x50, PUSH ESI (s1), PUSH EDI (s2 = s1-4).
 *   a = p1.z - p0.z, b = p0.x - p1.x through float slots; b*b + a*a <= g_fEpsilon: false.
 *   bCVector2::Normalize(a, b) (CALL 1000f810 at ESP = s2); l = (p0.x - cx)*a' + (p0.z - cz)*b'
 *   in the original's order; |l| >= radius * 0.5 (double 10090ba0): false, after six FSTP STn
 *   that leave the eight-deep x87 stack empty.
 * Both exits natively, decided on the host before anything is written; every other case (the
 * intersection itself, a NaN on the way, the sqrt's other paths, a cylinder in the frame) runs
 * the whole original. */
void g3n_10039d80(X86 *c) {
    if (!G3OPT(AI)) {
        fn_10039d80(c);
        return;
    }
    uint32_t *const r = c->r;
    const uint32_t esp0 = r[4];
    const uint32_t ebp = esp0 - 4u;
    const uint32_t s1 = (ebp & 0xfffffff8u) - 0x50u - 4u;
    const uint32_t s2 = s1 - 4u;
    const uint32_t st = rd32(esp0 + 4u);
    const uint32_t self = r[1];
    /* the frame Normalize writes reaches s2 - 0x30; the stretch and the cylinder outside it */
    const uint64_t flo = (uint64_t)s2 - 0x30u, fhi = (uint64_t)esp0;
    if ((flo < (uint64_t)st + 24u && (uint64_t)st < fhi) || (flo < (uint64_t)self + 16u && (uint64_t)self < fhi)) {
        fn_10039d80(c);
        return;
    }
    const uint16_t sw0 = c->fpu_sw;
    const uint32_t p0x_b = rd32(st), p0z_b = rd32(st + 8u);
    const float p1x = fto_float(c, (double)rdf32(st + 0xcu));
    const float p1z = fto_float(c, (double)rdf32(st + 0x14u));
    float p0x, p0z;
    memcpy(&p0x, &p0x_b, 4);
    memcpy(&p0z, &p0z_b, 4);
    const float a = fto_float(c, fx87(c, (double)p1z - (double)p0z));
    const float b = fto_float(c, fx87(c, (double)p0x - (double)p1x));
    const double aa = fx87(c, (double)a * (double)a);
    const double bb = fx87(c, (double)b * (double)b);
    const float l40 = fto_float(c, fx87(c, bb + aa));
    fcomi(c, (double)rdf32(SB_EPSILON), (double)l40);
    const int exit1 = !c->eflags_cf;
    float na = 0, nb = 0, cz = 0, cx = 0, l38 = 0, l34 = 0, l48 = 0, ab = 0;
    double rk = 0;
    if (!exit1) {
        if (!ai_norm2_value(c, a, b, &na, &nb)) {
            c->fpu_sw = sw0;
            fn_10039d80(c);
            return;
        }
        cz = fto_float(c, (double)rdf32(self + 0xcu));
        cx = fto_float(c, (double)rdf32(self + 4u));
        l38 = fto_float(c, fx87(c, (double)p0x - (double)cx));
        l34 = fto_float(c, fx87(c, (double)p0z - (double)cz));
        const double t1 = fx87(c, (double)l34 * (double)nb);
        const double t2 = fx87(c, (double)l38 * (double)na);
        l48 = fto_float(c, fx87(c, t1 + t2));
        ab = fto_float(c, fabs((double)l48));
        rk = fx87(c, (double)rdf32(self) * rdf64(SB_CULL_FACTOR));
        fcomi(c, (double)ab, rk);
        if (c->eflags_cf) { /* JC: the intersection itself (or a NaN) */
            c->fpu_sw = sw0;
            fn_10039d80(c);
            return;
        }
    }
    /* commit, in the original's order */
    c->fpu_sw = sw0;
    wr32(ebp, r[5]);
    wr32(s1, r[6]);
    wrf32(s1 + 0x1cu, p1x);
    ai_movss_load(c, 0, p0z_b);
    ai_movss_load(c, 1, p0x_b);
    wr32(s1 + 0xcu, p0z_b);
    wrf32(s1 + 0x14u, p1z);
    wr32(s1 + 8u, p0x_b);
    wr32(s2, r[7]);
    wr32(s2 + 0x1cu, p0z_b);
    wr32(s2 + 0x38u, p0x_b);
    wrf32(s2 + 0x10u, a);
    wrf32(s2 + 0x14u, b);
    wrf32(s2 + 0x20u, l40);
    /* FCOMIP eps,l40 and the arithmetic before it change no x87 state on these paths (no NaN) */
    const uint32_t esi0 = r[6], edi0 = r[7], ebp0 = r[5];
    if (exit1) {
        ai_ffree_below(c, 3);
        r[0] &= 0xffffff00u;
    } else {
        /* CALL 1000f810 with ECX = the vector at s2+0x10, EDI = this, ESI = the stretch */
        r[1] = s2 + 0x10u;
        r[5] = ebp;
        r[6] = st;
        r[7] = self;
        r[4] = s2 - 4u;
        wr32(r[4], SB_ISECT_NORM_RET);
        ai_ffree_below(c, 3);
        g3n_1000f810(c); /* the common path: checked above */
        wrf32(s2 + 0x20u, cz);
        wrf32(s2 + 0x30u, cx);
        wrf32(s2 + 0x28u, l38);
        wrf32(s2 + 0x2cu, l34);
        fcomi(c, (double)ab, rk); /* sets no x87 state here, as on the host */
        wrf32(s2 + 0x18u, l48);
        wrf32(s2 + 0x20u, ab);
        wrf64(s2 + 0x40u, rk);
        ai_ffree_below(c, 8);
        r[0] &= 0xffffff00u;
    }
    r[5] = ebp0;
    r[6] = esi0;
    r[7] = edi0;
    c->eip = rd32(esp0);
    r[4] = esp0 + 4u + 0x14u;
    recomp_return(c);
}
