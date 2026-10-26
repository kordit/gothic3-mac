// The host-PC sampler (hostprof.cpp, RECOMP_HOST_PROFILE=<file prefix>).
#pragma once
#include "../platform/os.h"

// PROFILE=reset / PROFILE=report in the trace control file: clear the samples,
// or write them to <prefix>-<n>.txt. Both do nothing without the variable.
void hostprof_reset();
void hostprof_report();
// Baton hold time and guest CPU per guest thread in ms per frame, between the same two
// keys (kernel32.cpp); Present counts the frames.
void sched_hold_reset();
void sched_hold_report();
void sched_note_present();
// The host thread that holds the guest baton now, 0 when none (kernel32.cpp).
OsThreadId sched_current_holder_host();
