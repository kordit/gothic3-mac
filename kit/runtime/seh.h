/* Guest x86 SEH checkpoints. Included by generated C as well as the runtime. */
#ifndef RECOMP_SEH_H
#define RECOMP_SEH_H
#include "x86.h"
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* setjmp must be executed by the live generated function, never in enter(). */
jmp_buf *recomp_seh_frame_enter(X86 *c);
/* Helpers orphan their surviving records before returning. The caller adopts
 * the newest one and executes setjmp in its own live host frame. */
uint64_t recomp_seh_frame_mark(X86 *c);
void recomp_seh_frame_orphan(X86 *c, uint64_t mark);
jmp_buf *recomp_seh_frame_adopt(X86 *c);
/* Drop records below ESP at this callback level; retain active landings. */
void recomp_seh_frame_leave(X86 *c);
void recomp_seh_land(X86 *c);
/* The registration an RtlUnwind left for this thread's next computed jump to land on, or 0.
 * Every recomp_jump asks first. The target is thread-local, and on macOS each read of a
 * thread-local is a call (_tlv_get_addr): with the call to fetch it, about 1% of a Gothic 3
 * town's main thread for an answer that is nearly always 0 (host profile, 24 Oct 2026). So a
 * process-wide count of the threads that have one pending answers first, as one load; only while
 * it is nonzero does the thread look at its own. A thread sees its own count update in order, so
 * the answer is the thread-local's own. */
extern uint32_t recomp_seh_pending_threads;
uint32_t recomp_seh_pending_target_tls(void);
static inline uint32_t recomp_seh_pending_target(void) {
    if (__builtin_expect(__atomic_load_n(&recomp_seh_pending_threads, __ATOMIC_RELAXED) == 0, 1))
        return 0;
    return recomp_seh_pending_target_tls();
}
void recomp_seh_intercept(X86 *c, uint32_t target);
/* Verbose-only, bounded validation. Logs the first failure and last good point. */
void recomp_seh_validate_chain(X86 *c, const char *phase, const char *detail);

/* Context reuse/teardown on its owning host thread; NULL drops all its state. */
void recomp_seh_reset(X86 *c);
/* A callback return abandons frames/landings created at this call depth. */
void recomp_seh_callback_leave(X86 *c, uint32_t depth);
/* Returns zero for an exhausted chain; kernel32 retains its abort diagnostics.
 * A nonzero return is possible only through the test-only unhandled hook. */
int recomp_seh_raise(X86 *c, uint32_t code, uint32_t flags, uint32_t nargs, uint32_t args);
void recomp_seh_unwind(X86 *c, uint32_t target, uint32_t target_ip, uint32_t record,
                       uint32_t retval);

#ifdef POPM_TESTING
typedef void (*RecompSehUnhandledHook)(X86 *, uint32_t record, uint32_t context);
void recomp_seh_test_unhandled_hook(RecompSehUnhandledHook hook);
uint32_t recomp_seh_test_frame_count(X86 *c);
#endif

#ifdef __cplusplus
}
#endif
#endif
