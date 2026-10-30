// memtrace.h - which guest memory a stretch of guest code reads and writes (Gothic 3 D-054, step 1: the state the
// render reads, to size a snapshot of it).
//
// recomp_memtrace_begin protects the whole guest arena (PROT_NONE; sealed read-only pages stay as they are). The first
// touch of each host page faults: a read opens the page for reading and records who read it; a write copies the page
// to a preallocated shadow first and opens it for writing. recomp_memtrace_end compares each written page with its
// shadow, so writes are known to the byte (a store of the value already there is not seen) and reads to the page,
// restores the arena and appends a report to a file. Every thread faults while it is on, not only the one that began
// it; each record says which thread touched the page first.
//
// The kernel does not fault for a syscall that reads into protected memory, it fails with EFAULT: code that hands
// guest memory to a syscall calls recomp_memtrace_touch first (it costs one load while off).
//
// macOS on arm64 only (the fault's ESR tells a read from a write); elsewhere begin returns 0 and the rest do nothing.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// 1 when the arena is now traced; 0 when already on, unsupported, or the tables cannot be had.
int recomp_memtrace_begin(void);
// Stops, restores the arena and appends the report under `label` to `path` (stderr when NULL), and the written pages'
// contents to `path`.pages. Nothing when off.
void recomp_memtrace_end(const char *label, const char *path);
int recomp_memtrace_active(void);
// Opens [addr, addr+len) as a fault would (write: for writing), recorded as touched by the caller; before a syscall.
void recomp_memtrace_touch(uint32_t addr, uint32_t len, int write);
#ifdef __cplusplus
}
#endif

// The fault hook's first step (memory.cpp, readonly_fault): 1 when the fault was a traced page and is dealt with.
int memtrace_fault(uint64_t addr, void *uc);
