/* native/fmodmem.c - FMOD's memory apart from the game's heap. Group `fmodmem` (G3OPT_FMODMEM), off by default.
 *
 * eCAudioSystem hands FMOD_Memory_Initialize (Engine 102b71d0) three callbacks that take FMOD's memory from
 * SharedBase's MemHeap: FModAllocMemory 103cb030, FModReallocMemory 102b7190 and FModFreeMemory 102b71b0. fmodex
 * calls them from its own threads, which run without the baton, so FMOD's blocks land between the game's at moments
 * no run repeats: two pinned runs of one script parted by a pixel in exactly the frames where fmodex's threads took
 * MemHeap's lock 1011d018 (RECOMP_CS_TRACE, session 30), and with the logic on a second thread it would be a third
 * thread on that lock. With the group on, FMOD's blocks come from the kit's span (heap_alloc_kit), which the game's
 * own allocations never use.
 *
 * Whether the group is on is read once, at FMOD's first allocation (at start), and kept: a block must be freed by
 * the heap it came from. */
#include "chunk.h"
#include "g3_opt.h"

uint32_t heap_alloc_kit(uint32_t size, _Bool zero, uint32_t align) __asm__("__Z14heap_alloc_kitjbj");
uint32_t heap_realloc(uint32_t addr, uint32_t new_size, _Bool zero) __asm__("__Z12heap_reallocjjb");
_Bool heap_free(uint32_t addr) __asm__("__Z9heap_freej");

void fn_103cb030(X86 *c); /* Engine eCAudioSystem::FModAllocMemory (stdcall: size, type, source) */
void fn_102b7190(X86 *c); /* Engine eCAudioSystem::FModReallocMemory (stdcall: block, size, type, source) */
void fn_102b71b0(X86 *c); /* Engine eCAudioSystem::FModFreeMemory (stdcall: block, type, source) */

static int fm_on = -1; /* -1 until FMOD's first allocation */

static int fm_apart(void) {
    if (fm_on < 0)
        fm_on = G3OPT(FMODMEM) ? 1 : 0;
    return fm_on;
}

static inline void fm_ret(X86 *c, uint32_t value, uint32_t args) {
    const uint32_t esp = c->r[4];
    c->r[0] = value;
    c->eip = rd32(esp);
    c->r[4] = esp + 4u + args;
    recomp_return(c);
}

void g3n_103cb030(X86 *c) {
    if (!fm_apart()) {
        fn_103cb030(c);
        return;
    }
    fm_ret(c, heap_alloc_kit(rd32(c->r[4] + 4u), 0, 16), 12u);
}

void g3n_102b7190(X86 *c) {
    if (!fm_apart()) {
        fn_102b7190(c);
        return;
    }
    const uint32_t block = rd32(c->r[4] + 4u), size = rd32(c->r[4] + 8u);
    uint32_t out = 0;
    if (!block)
        out = heap_alloc_kit(size, 0, 16);
    else if (!size)
        heap_free(block);
    else
        out = heap_realloc(block, size, 0);
    fm_ret(c, out, 16u);
}

void g3n_102b71b0(X86 *c) {
    if (!fm_apart()) {
        fn_102b71b0(c);
        return;
    }
    const uint32_t block = rd32(c->r[4] + 4u);
    if (block)
        heap_free(block);
    fm_ret(c, 0, 12u);
}
