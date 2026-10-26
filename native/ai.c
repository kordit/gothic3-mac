/* native/ai.c - the AI scripts' hot path in a fight: NPC speech looks its file up by name.
 *
 * In a fight at the start of a new game (work/sample-ng2-a.txt) gCScriptRoutine_PS::OnProcess
 * 10a74b60 holds 904 of 6071 main-thread samples, and 504 of those are one chain: the Script_Game
 * routines (10fd2cd0, 10fd2710, 10fd3530, 10fc4480, ...) -> StartSaySVM -> StartSayEx ->
 * StartOutput -> eCAudioModule::PlayStream3D -> fmodex createSound -> eCAudioStreamer::OpenFile ->
 * eCVirtualFileSystem::GetFileSize -> FileSystem FindFirstFileA 10da1cc0 (384). FindFirstFileA's
 * directory walk FUN_10da4800 asks every entry of the directory, through its vtable +0x20
 * (FUN_10da29f0), whether its name fits the wanted one: the entry's path is cut after its last '/'
 * (bCString::ReverseFind -> SharedBase _strrchr 1006b740, then Right, a new string), and the name
 * is matched by FUN_10da9e10, which splits both names at their first '.' into four 256-byte stack
 * buffers and matches base and extension with FUN_10da9d30 (wildcards, _tolower).
 *
 * Here, natively and exactly as the translation runs them (the same registers, the same guest
 * stack, the same writes, the same guest calls with the same arguments in the same order):
 *   g3n_10da9e10  the name match with its two FUN_10da9d30 calls inlined; FileSystem's _tolower
 *                 10db08d6 computed in place while the CRT's locale-changed flag is clear (its
 *                 first five instructions), a guest call otherwise;
 *   g3n_1006b740  SharedBase _strrchr (REPNE SCASB forwards, then backwards).
 * Each runs its native version only under G3OPT(AI); otherwise it calls the original. */
#include "chunk.h"
#include "g3_opt.h"

#include <math.h>
#include <string.h>

void fn_10da9e10(X86 *c); /* FileSystem FUN_10da9e10, name match, EAX = wanted, EDX = entry name */
void fn_1006b740(X86 *c); /* SharedBase _strrchr, cdecl */
void fn_10db08d6(X86 *c); /* FileSystem _tolower, cdecl */
#ifndef FN_10db08d6
#define FN_10db08d6 fn_10db08d6
#endif

#define FS_LOCALE_CHANGED 0x10dc6b60u /* FileSystem CRT __locale_changed */

/* The registers a transcribed function works on; copied to and from X86 around guest calls. */
typedef struct {
    uint32_t r[8];
} ai_regs;

static inline void ai_push(ai_regs *g, uint32_t v) {
    g->r[4] -= 4u;
    wr32(g->r[4], v);
}
static inline uint32_t ai_pop(ai_regs *g) {
    const uint32_t v = rd32(g->r[4]);
    g->r[4] += 4u;
    return v;
}
static inline void ai_setlo8(uint32_t *r, uint8_t v) {
    *r = (*r & 0xffffff00u) | v;
}

/* CALL 10db08d6 (_tolower) with the argument already pushed: PUSH ret, then either its fast path
 * in place (CMP [locale changed],0 / JNZ / MOV EAX,[ESP+4] / LEA ECX,[EAX-0x41] / CMP ECX,0x19 /
 * JA / ADD EAX,0x20 / RET) or the guest function. */
static inline void ai_tolower(X86 *c, ai_regs *g, uint32_t ret) {
    ai_push(g, ret);
    if (rd32(FS_LOCALE_CHANGED) == 0u) {
        g->r[0] = rd32(g->r[4] + 4u);
        g->r[1] = g->r[0] - 0x41u;
        if (g->r[1] <= 0x19u)
            g->r[0] += 0x20u;
        g->r[4] += 4u;
        return;
    }
    memcpy(c->r, g->r, sizeof g->r);
    if (recomp_profile_enabled)
        recomp_call(c, 0x10db08d6u);
    else
        FN_10db08d6(c);
    memcpy(g->r, c->r, sizeof g->r);
}

/* FUN_10da9d30: ESI = ECX (pattern), EDI = EAX (name), returns AL; the return address `ret` is
 * pushed here, the RET pops it. */
static void ai_match(X86 *c, ai_regs *gp, uint32_t ret) {
    ai_regs gl = *gp, *const g = &gl; /* a local copy the compiler keeps in registers */
    uint32_t *const r = g->r;
    enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
    ai_push(g, ret);
    ai_push(g, r[ESI]);
    ai_push(g, r[EDI]);
    r[EDI] = r[EAX];
    r[ESI] = r[ECX];
    if (r[ESI] == r[EDI]) {
        r[EDI] = ai_pop(g);
        ai_setlo8(&r[EAX], 1);
        r[ESI] = ai_pop(g);
        goto ret;
    }
    if (r[ESI] == 0u || r[EDI] == 0u) {
    l43:
        r[EDI] = ai_pop(g);
        ai_setlo8(&r[EAX], 0);
        r[ESI] = ai_pop(g);
        goto ret;
    }
    {
        const uint8_t first = rd8(r[ESI]);
        ai_push(g, r[EBX]);
        if (first == 0)
            goto de3;
    }
d56:
    ai_setlo8(&r[ECX], rd8(r[EDI]));
    if ((uint8_t)r[ECX] == 0)
        goto de3;
    ai_setlo8(&r[EAX], rd8(r[ESI]));
    if ((uint8_t)r[EAX] == 0x2a)
        goto d91;
    if ((uint8_t)r[EAX] == 0x3f || (uint8_t)r[EAX] == (uint8_t)r[ECX])
        goto d89;
    r[EAX] = (uint32_t)(int32_t)(int8_t)r[EAX];
    ai_push(g, r[EAX]);
    ai_tolower(c, g, 0x10da9d77u);
    r[ECX] = (uint32_t)(int32_t)(int8_t)rd8(r[EDI]);
    ai_push(g, r[ECX]);
    r[EBX] = r[EAX];
    ai_tolower(c, g, 0x10da9d82u);
    r[ESP] += 8u;
    if (r[EBX] != r[EAX])
        goto e03;
d89:
    r[ESI] += 1u;
    r[EDI] += 1u;
    goto dda;
d91:
    for (;;) {
        const uint8_t al = (uint8_t)r[EAX];
        if (al == 0 || (al != 0x2a && al != 0x3f))
            break;
        ai_setlo8(&r[EAX], rd8(r[ESI] + 1u));
        r[ESI] += 1u;
        if ((uint8_t)r[EAX] == 0)
            break;
    }
    r[EDX] = (uint32_t)(int32_t)(int8_t)rd8(r[ESI]);
    ai_push(g, r[EDX]);
    ai_tolower(c, g, 0x10da9db0u);
    ai_setlo8(&r[ECX], rd8(r[EDI]));
    r[ESP] += 4u;
    if ((uint8_t)r[ECX] == 0)
        goto dda;
    r[EBX] = (uint32_t)(int32_t)(int8_t)r[EAX];
    for (;;) {
        r[EAX] = (uint32_t)(int32_t)(int8_t)r[ECX];
        ai_push(g, r[EAX]);
        ai_tolower(c, g, 0x10da9dc9u);
        r[ESP] += 4u;
        if (r[EAX] == r[EBX])
            break;
        ai_setlo8(&r[ECX], rd8(r[EDI] + 1u));
        r[EDI] += 1u;
        if ((uint8_t)r[ECX] == 0)
            break;
    }
dda:
    if (rd8(r[ESI]) != 0)
        goto d56;
de3:
    if (rd8(r[ESI]) == 0x2a)
        do
            r[ESI] += 1u;
        while (rd8(r[ESI]) == 0x2a);
    if (rd8(r[ESI]) != 0 || rd8(r[EDI]) != 0)
        goto e09;
    r[EBX] = ai_pop(g);
    r[EDI] = ai_pop(g);
    r[EAX] = 1u;
    r[ESI] = ai_pop(g);
    goto ret;
e03:
    r[EBX] = ai_pop(g);
    r[EDI] = ai_pop(g);
    ai_setlo8(&r[EAX], 0);
    r[ESI] = ai_pop(g);
    goto ret;
e09:
    r[EBX] = ai_pop(g);
    r[EDI] = ai_pop(g);
    r[EAX] = 0u;
    r[ESI] = ai_pop(g);
ret:
    r[ESP] += 4u; /* RET: back to `ret`, which is where the caller continues */
    *gp = gl;
}

/* One of the original's copy loops: the byte in reg_lo's low byte, then while it is not NUL (nor
 * '.', with stop_at_dot): src+1, [dst] = it, it = [src], dst+1. Every byte goes through guest
 * memory, so a name longer than its 256-byte buffer runs over the next one as in the original. The
 * last byte read ends in reg_lo's low byte (CL or AL). */
static inline void ai_copy_part(uint32_t *srcp, uint32_t *dstp, uint32_t *reg_lo, int stop_at_dot) {
    uint32_t src = *srcp, dst = *dstp; /* locals: a byte store through g_mem cannot alias them */
    uint8_t ch = (uint8_t)*reg_lo;
#if defined(RECOMP_NULL_CHECKS) && RECOMP_NULL_CHECKS
    while (ch != 0 && (!stop_at_dot || ch != 0x2e)) {
        src += 1u;
        wr8(dst, ch);
        ch = rd8(src);
        dst += 1u;
    }
#else
    uint8_t *const m = g_mem; /* g_mem itself is a global a byte store could alias */
    while (ch != 0 && (!stop_at_dot || ch != 0x2e)) {
        src += 1u;
        m[dst] = ch;
        ch = m[src];
        dst += 1u;
    }
#endif
    *srcp = src;
    *dstp = dst;
    ai_setlo8(reg_lo, ch);
}

/* bool FUN_10da9e10(EAX = wanted name, EDX = entry name), RET. Splits each at its first '.' into
 * base and extension (buffers at ESP+0x10/0x110/0x210/0x310 after its four pushes), then
 * FUN_10da9d30(wanted base, entry base) && FUN_10da9d30(wanted ext, entry ext); EAX = 0 or 1. */
void g3n_10da9e10(X86 *c) {
    if (!G3OPT(AI)) {
        fn_10da9e10(c);
        return;
    }
    ai_regs g;
    memcpy(g.r, c->r, sizeof g.r);
    uint32_t *const r = g.r;
    enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
    ai_setlo8(&r[ECX], rd8(r[EAX]));
    r[ESP] -= 0x400u;
    ai_push(&g, r[EBX]);
    ai_push(&g, r[EBP]);
    ai_push(&g, r[ESI]);
    ai_push(&g, r[EDI]);
    r[ESI] = r[ESP] + 0x10u;
    r[EBP] = r[ESP] + 0x210u;
    r[EDI] = r[ESP] + 0x310u;
    r[EBX] = r[ESP] + 0x110u;
    /* wanted name: base into ESI's buffer, extension into EDI's */
    ai_copy_part(&r[EAX], &r[ESI], &r[ECX], 1);
    ai_setlo8(&r[ECX], rd8(r[EAX]));
    r[EAX] += 1u;
    wr8(r[ESI], 0);
    if ((uint8_t)r[ECX] == 0x2e) {
        ai_setlo8(&r[ECX], rd8(r[EAX]));
        ai_copy_part(&r[EAX], &r[EDI], &r[ECX], 0);
    }
    /* entry name: base into EBP's buffer, extension into EBX's */
    ai_setlo8(&r[EAX], rd8(r[EDX]));
    wr8(r[EDI], 0);
    ai_copy_part(&r[EDX], &r[EBP], &r[EAX], 1);
    ai_setlo8(&r[EAX], rd8(r[EDX]));
    r[EDX] += 1u;
    wr8(r[EBP], 0);
    if ((uint8_t)r[EAX] == 0x2e) {
        ai_setlo8(&r[EAX], rd8(r[EDX]));
        ai_copy_part(&r[EDX], &r[EBX], &r[EAX], 0);
    }
    r[EAX] = r[ESP] + 0x210u;
    r[ECX] = r[ESP] + 0x10u;
    wr8(r[EBX], 0);
    ai_match(c, &g, 0x10da9ec7u);
    const uint8_t al = (uint8_t)r[EAX];
    r[EDI] = ai_pop(&g);
    r[ESI] = ai_pop(&g);
    r[EBP] = ai_pop(&g);
    r[EBX] = ai_pop(&g);
    if (al != 0) {
        r[EAX] = r[ESP] + 0x100u;
        r[ECX] = r[ESP] + 0x300u;
        ai_match(c, &g, 0x10da9ee2u);
        r[EAX] = (uint8_t)r[EAX] != 0 ? 1u : 0u;
    } else {
        r[EAX] = 0u;
    }
    r[ESP] += 0x400u;
    memcpy(c->r, g.r, sizeof g.r);
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u;
    recomp_return(c);
}

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

/* ---- FUN_10da29f0: does a directory entry's name fit the wanted one ---------------------- */

void fn_10da29f0(X86 *c); /* FileSystem FUN_10da29f0, thiscall (entry), RET 4 */
void fn_1000c800(X86 *c); /* SharedBase bCMemoryAdmin::GetInstance */
void fn_1000c9a0(X86 *c); /* SharedBase bCMemoryAdmin::Malloc, thiscall, RET 4 */
void fn_10020090(X86 *c); /* SharedBase MemHeap::Free, stdcall, RET 4 */
void fn_10078780(X86 *c); /* SharedBase _memcpy, cdecl */
void fn_10003ea0(X86 *c); /* SharedBase bCString::Release, thiscall */
void fn_100029c0(X86 *c); /* SharedBase bCString::bCString(char const *), RET 4 */
#ifndef FN_1000c800
#define FN_1000c800 fn_1000c800
#endif
#ifndef FN_1000c9a0
#define FN_1000c9a0 fn_1000c9a0
#endif
#ifndef FN_10020090
#define FN_10020090 fn_10020090
#endif
#ifndef FN_10078780
#define FN_10078780 fn_10078780
#endif
#ifndef FN_10003ea0
#define FN_10003ea0 fn_10003ea0
#endif
#ifndef FN_100029c0
#define FN_100029c0 fn_100029c0
#endif
#ifndef FN_10da9e10
#define FN_10da9e10 fn_10da9e10
#endif
#ifndef FN_1006b740
#define FN_1006b740 fn_1006b740
#endif

/* FileSystem's imports from SharedBase that the chain calls through, and what they hold in the
 * game (the port maps every module at its own base). Any other value takes the original. */
#define IAT_CTOR 0x10dbc1b4u    /* bCString::bCString() 10002a50 */
#define IAT_CSTR 0x10dbc224u    /* bCString::operator char const *() 10005680 */
#define IAT_DTOR 0x10dbc1f0u    /* bCString::~bCString() 10001a00 */
#define IAT_RFIND 0x10dbc1acu   /* bCString::ReverseFind(char) 10004550 */
#define IAT_RIGHT 0x10dbc1a8u   /* bCString::Right(int) 10004a10 */
#define IAT_ASSIGN 0x10dbc210u  /* bCString::operator=(bCString const &) 10005040 */
#define IAT_ISEMPTY 0x10dbc204u /* bCString::IsEmpty() 100052f0 */
#define SB_EMPTY_TEXT 0x1008b4e4u /* SharedBase's "" for an empty bCString */
#define SB_MEMADMIN 0x10231a58u   /* bCMemoryAdmin's instance; bit 0 of +4 = constructed */

/* CALL target with the continuation `ret` pushed; the registers go to the guest and back. */
static inline void ai_gcall(X86 *c, ai_regs *g, uint32_t ret, uint32_t target, void (*f)(X86 *)) {
    ai_push(g, ret);
    memcpy(c->r, g->r, sizeof g->r);
    if (recomp_profile_enabled)
        recomp_call(c, target);
    else
        f(c);
    memcpy(g->r, c->r, sizeof g->r);
}

enum { AX, CX, DX, BX, SP, BP, SI, DI };

/* bCMemoryAdmin::GetInstance (CALL from `ret`): MOV EAX,1 / TEST [inst+4],AL / JNZ -> MOV EAX,inst
 * / RET; the first call (construction) goes to the guest. */
static inline void ai_getinstance(X86 *c, ai_regs *g, uint32_t ret) {
    if (rd8(SB_MEMADMIN + 4u) & 1u) {
        wr32(g->r[SP] - 4u, ret);
        g->r[AX] = SB_MEMADMIN;
        return;
    }
    ai_gcall(c, g, ret, 0x1000c800u, FN_1000c800);
}

/* bCString::IsEmpty (ECX = string), called from `ret`. */
static inline void ai_isempty(ai_regs *g, uint32_t ret) {
    uint32_t *const r = g->r;
    wr32(r[SP] - 4u, ret);
    r[AX] = rd32(r[CX]);
    if (r[AX] != 0u)
        r[AX] = rd32(r[AX] - 8u);
    r[CX] = r[AX] == 0u;
    ai_setlo8(&r[AX], (uint8_t)r[CX]);
}

/* bCString::~bCString (ECX = string), called from `ret`: the reference count down, MemHeap::Free
 * of the block when it reaches 0. */
static void ai_dtor(X86 *c, ai_regs *g, uint32_t ret) {
    uint32_t *const r = g->r;
    ai_push(g, ret);
    if (r[CX] != 0u && (r[AX] = rd32(r[CX])) != 0u) {
        wr16(r[AX] - 4u, (uint16_t)(rd16(r[AX] - 4u) - 1u));
        ai_push(g, r[SI]);
        r[SI] = r[AX] - 8u;
        r[AX] = rd16(r[SI] + 4u);
        if ((uint16_t)r[AX] == 0u) {
            ai_getinstance(c, g, 0x10001a22u);
            ai_push(g, r[SI]);
            ai_gcall(c, g, 0x10001a28u, 0x10020090u, FN_10020090);
        }
        r[SI] = ai_pop(g);
    }
    r[SP] += 4u;
}

/* bCString::bCString(char const *, int) 100027e0 (ECX = this), called from `ret`, RET 8. */
static void ai_ctor_n(X86 *c, ai_regs *g, uint32_t ret) {
    uint32_t *const r = g->r;
    ai_push(g, ret);
    ai_push(g, r[SI]);
    r[SI] = rd32(r[SP] + 0xcu);
    ai_push(g, r[DI]);
    r[DI] = r[CX];
    if ((int32_t)r[SI] <= 0) {
        wr32(r[DI], 0u);
        r[AX] = r[DI];
    } else {
        r[AX] = r[SI] + 9u;
        ai_push(g, r[AX]);
        ai_getinstance(c, g, 0x100027ffu);
        r[CX] = r[AX];
        ai_gcall(c, g, 0x10002806u, 0x1000c9a0u, FN_1000c9a0);
        wr32(r[AX], r[SI]);
        wr16(r[AX] + 4u, 1u);
        r[AX] += 8u;
        wr32(r[DI], r[AX]);
        wr8(r[AX] + r[SI], 0);
        r[CX] = rd32(r[SP] + 0xcu);
        r[DX] = rd32(r[DI]);
        ai_push(g, r[SI]);
        ai_push(g, r[CX]);
        ai_push(g, r[DX]);
        ai_gcall(c, g, 0x10002825u, 0x10078780u, FN_10078780);
        r[SP] += 0xcu;
        r[AX] = r[DI];
    }
    r[DI] = ai_pop(g);
    r[SI] = ai_pop(g);
    r[SP] += 4u + 8u;
}

/* bCString::Right(int) 10004a10 (ECX = this; result pointer and count on the stack), RET 8. */
static void ai_right(X86 *c, ai_regs *g, uint32_t ret) {
    uint32_t *const r = g->r;
    ai_push(g, ret);
    r[DX] = rd32(r[SP] + 8u);
    ai_push(g, r[SI]);
    if ((int32_t)r[DX] < 0) {
        r[AX] = rd32(r[CX]);
        r[SI] = rd32(r[SP] + 8u);
        ai_push(g, r[AX]);
        r[CX] = r[SI];
        ai_gcall(c, g, 0x10004a27u, 0x100029c0u, FN_100029c0);
        r[AX] = r[SI];
    } else {
        r[CX] = rd32(r[CX]);
        r[AX] = r[CX] ? rd32(r[CX] - 8u) : 0u;
        if ((int32_t)r[DX] < (int32_t)r[AX]) {
            r[AX] = r[CX] ? rd32(r[CX] - 8u) : 0u;
            r[SI] = rd32(r[SP] + 8u);
            r[CX] = r[CX] - r[DX] + r[AX];
            ai_push(g, r[DX]);
            ai_push(g, r[CX]);
            r[CX] = r[SI];
            ai_ctor_n(c, g, 0x10004a6du);
            r[AX] = r[SI];
        } else {
            r[AX] = rd32(r[SP] + 8u);
            wr32(r[AX], r[CX]);
            if (r[CX] != 0u)
                wr16(r[CX] - 4u, (uint16_t)(rd16(r[CX] - 4u) + 1u));
        }
    }
    r[SI] = ai_pop(g);
    r[SP] += 4u + 8u;
}

/* bCString::operator=(bCString const &) 10005040 -> 10002e20 (ECX = this), called from `ret`. */
static void ai_assign(X86 *c, ai_regs *g, uint32_t ret) {
    uint32_t *const r = g->r;
    ai_push(g, ret);
    r[AX] = rd32(r[SP] + 4u);
    ai_push(g, r[SI]);
    ai_push(g, r[AX]);
    r[SI] = r[CX];
    /* 10002e20 */
    ai_push(g, 0x1000504du);
    ai_push(g, r[SI]);
    ai_push(g, r[DI]);
    r[DI] = rd32(r[SP] + 0xcu);
    r[AX] = rd32(r[DI]);
    r[SI] = r[CX];
    if (r[AX] == 0u || rd32(r[AX] - 8u) == 0u) {
        r[AX] = rd32(r[SI]);
        if (r[AX] != 0u) {
            if (rd16(r[AX] - 4u) <= 1u && (int32_t)rd32(r[AX] - 8u) >= 0) {
                wr32(r[AX] - 8u, 0u);
                r[AX] = rd32(r[SI]);
                r[DI] = ai_pop(g);
                wr8(r[AX], 0);
                r[SI] = ai_pop(g);
                goto done;
            }
            wr16(r[AX] - 4u, (uint16_t)(rd16(r[AX] - 4u) - 1u));
            r[DI] = r[AX] - 8u;
            r[AX] = rd16(r[DI] + 4u);
            if ((uint16_t)r[AX] == 0u) {
                ai_getinstance(c, g, 0x10002e6fu);
                ai_push(g, r[DI]);
                ai_gcall(c, g, 0x10002e75u, 0x10020090u, FN_10020090);
            }
        }
        r[DI] = ai_pop(g);
        wr32(r[SI], 0u);
        r[SI] = ai_pop(g);
        goto done;
    }
    r[CX] = rd32(r[SI]);
    if (r[CX] != 0u) {
        if (r[CX] == r[AX]) {
            r[DI] = ai_pop(g);
            r[SI] = ai_pop(g);
            goto done;
        }
        r[CX] = r[SI];
        ai_gcall(c, g, 0x10002e91u, 0x10003ea0u, FN_10003ea0);
        r[AX] = rd32(r[DI]);
    }
    wr32(r[SI], r[AX]);
    wr16(r[AX] - 4u, (uint16_t)(rd16(r[AX] - 4u) + 1u));
    r[DI] = ai_pop(g);
    r[SI] = ai_pop(g);
done:
    r[SP] += 4u + 4u; /* 10002e20's RET 4 */
    r[AX] = r[SI];
    r[SI] = ai_pop(g);
    r[SP] += 4u + 4u; /* RET 4 */
}

/* bCString::ReverseFind(char) 10004550 (ECX = this, the char on the stack), RET 4, called from
 * `ret`; _strrchr through FN_1006b740. */
static void ai_reversefind(X86 *c, ai_regs *g, uint32_t ret) {
    uint32_t *const r = g->r;
    ai_push(g, ret);
    ai_push(g, r[SI]);
    r[SI] = r[CX];
    r[AX] = rd32(r[SI]);
    if (r[AX] != 0u) {
        r[CX] = (uint32_t)(int32_t)(int8_t)rd8(r[SP] + 8u);
        ai_push(g, r[CX]);
        ai_push(g, r[AX]);
        ai_gcall(c, g, 0x1000456cu, 0x1006b740u, FN_1006b740);
        r[SP] += 8u;
        if (r[AX] != 0u) {
            r[AX] -= rd32(r[SI]);
            goto out;
        }
    }
    r[AX] = 0xffffffffu;
out:
    r[SI] = ai_pop(g);
    r[SP] += 4u + 4u;
}

/* FUN_10da2a40, the entry's vtable +0x24 (ECX = entry, [ESP+4] = the bCString to fill): for a
 * file entry (+0x1c, FUN_10da29d0: its path at +4 not empty) the part of its path after the last
 * '/' (Right, a new string), assigned to the argument; returns !IsEmpty(argument). */
static void ai_name(X86 *c, ai_regs *g, uint32_t ret) {
    uint32_t *const r = g->r;
    ai_push(g, ret);
    ai_push(g, r[CX]);
    ai_push(g, r[SI]);
    r[SI] = r[CX];
    /* MOV EAX,[ESI] / MOV EDX,[EAX+0x1c] / CALL EDX = FUN_10da29d0:
     * ADD ECX,4 / CALL [IsEmpty] / NEG AL / SBB EAX,EAX / ADD EAX,1 / RET */
    r[AX] = rd32(r[SI]);
    r[DX] = rd32(r[AX] + 0x1cu);
    ai_push(g, 0x10da2a4bu);
    r[CX] += 4u;
    ai_isempty(g, 0x10da29d9u);
    r[AX] = (uint8_t)r[AX] ? 0u : 1u;
    r[SP] += 4u;
    if ((uint8_t)r[AX] == 0u) {
        r[SI] = ai_pop(g);
        r[CX] = ai_pop(g);
        r[SP] += 4u + 4u;
        return;
    }
    r[SI] += 4u;
    ai_push(g, 0x2fu);
    r[CX] = r[SI];
    ai_reversefind(c, g, 0x10da2a61u);
    r[CX] = rd32(r[SI]);
    if (r[CX] != 0u)
        r[CX] = rd32(r[CX] - 8u);
    r[CX] = r[CX] - r[AX] - 1u;
    ai_push(g, r[CX]);
    r[AX] = r[SP] + 8u;
    ai_push(g, r[AX]);
    r[CX] = r[SI];
    ai_right(c, g, 0x10da2a7du);
    r[SI] = rd32(r[SP] + 0xcu);
    ai_push(g, r[AX]);
    r[CX] = r[SI];
    ai_assign(c, g, 0x10da2a8au);
    r[CX] = r[SP] + 4u;
    ai_dtor(c, g, 0x10da2a94u);
    r[CX] = r[SI];
    ai_isempty(g, 0x10da2a9cu);
    r[AX] = (uint8_t)r[AX] ? 0u : 1u;
    r[SI] = ai_pop(g);
    r[CX] = ai_pop(g);
    r[SP] += 4u + 4u;
}

static inline int ai_chain_is_original(uint32_t entry) {
    const uint32_t vt = rd32(entry);
    return rd32(vt + 0x24u) == 0x10da2a40u && rd32(vt + 0x1cu) == 0x10da29d0u && rd32(IAT_CTOR) == 0x10002a50u &&
           rd32(IAT_CSTR) == 0x10005680u && rd32(IAT_DTOR) == 0x10001a00u && rd32(IAT_RFIND) == 0x10004550u &&
           rd32(IAT_RIGHT) == 0x10004a10u && rd32(IAT_ASSIGN) == 0x10005040u && rd32(IAT_ISEMPTY) == 0x100052f0u;
}

/* bool FUN_10da29f0(entry = ECX, bCString const &wanted), RET 4: the entry's name (vtable +0x24)
 * into a local bCString, then FUN_10da9e10(wanted, name), the local destroyed. Natively for the
 * file entries (vtable +0x24 FUN_10da2a40, +0x1c FUN_10da29d0) with every SharedBase string
 * function of the chain in place (the same instructions, registers and stack); the guest is
 * called for what changes the heap (GetInstance's first call, Malloc, Free, Release), the copy
 * (_memcpy), and for _strrchr and FUN_10da9e10 (native above). */
void g3n_10da29f0(X86 *c) {
    if (!G3OPT(AI) || !ai_chain_is_original(c->r[1])) {
        fn_10da29f0(c);
        return;
    }
    ai_regs gl, *const g = &gl;
    memcpy(gl.r, c->r, sizeof gl.r);
    uint32_t *const r = gl.r;
    ai_push(g, r[CX]);
    ai_push(g, r[BX]);
    ai_push(g, r[SI]);
    r[SI] = r[CX];
    ai_push(g, r[DI]);
    /* CALL [bCString::bCString()]: MOV EAX,ECX / MOV [EAX],0 / RET */
    r[CX] = r[SP] + 0xcu;
    wr32(r[SP] - 4u, 0x10da2a00u);
    r[AX] = r[CX];
    wr32(r[AX], 0u);
    /* CALL [vtable+0x24] (ECX = entry, the local) */
    r[AX] = rd32(r[SI]);
    r[DX] = rd32(r[AX] + 0x24u);
    r[CX] = r[SP] + 0xcu;
    ai_push(g, r[CX]);
    r[CX] = r[SI];
    ai_name(c, g, 0x10da2a0eu);
    /* MOV ESI,[operator char const *]; CALL ESI twice: MOV EAX,[ECX] / TEST / JNZ / MOV EAX,"" / RET */
    r[SI] = rd32(IAT_CSTR);
    r[CX] = r[SP] + 0xcu;
    wr32(r[SP] - 4u, 0x10da2a1au);
    r[AX] = rd32(r[CX]);
    if (r[AX] == 0u)
        r[AX] = SB_EMPTY_TEXT;
    r[CX] = rd32(r[SP] + 0x14u);
    r[DI] = r[AX];
    wr32(r[SP] - 4u, 0x10da2a22u);
    r[AX] = rd32(r[CX]);
    if (r[AX] == 0u)
        r[AX] = SB_EMPTY_TEXT;
    r[DX] = r[DI];
    ai_gcall(c, g, 0x10da2a29u, 0x10da9e10u, FN_10da9e10);
    r[CX] = r[SP] + 0xcu;
    ai_setlo8(&r[BX], (uint8_t)r[AX]);
    ai_dtor(c, g, 0x10da2a35u);
    r[DI] = ai_pop(g);
    r[SI] = ai_pop(g);
    ai_setlo8(&r[AX], (uint8_t)r[BX]);
    r[BX] = ai_pop(g);
    r[CX] = ai_pop(g);
    memcpy(c->r, gl.r, sizeof gl.r);
    c->eip = rd32(c->r[4]);
    c->r[4] += 4u + 4u;
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
