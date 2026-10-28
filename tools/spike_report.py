#!/usr/bin/env python3
"""Stacks of slow frames (kit hostprof, RECOMP_HOST_PROFILE_SPIKE): per spike file the functions with the most
samples, self and anywhere on the stack, then the other guest threads of that frame (s-thr-*.txt, sampled every 2 ms):
their samples at work and waiting, and where the working ones were. python3 tools/spike_report.py <dir> [top]"""
import bisect, collections, glob, os, re, sys
d = sys.argv[1]; top = int(sys.argv[2]) if len(sys.argv) > 2 else 12
addrs, names = [], []
for line in open(os.path.join(d, "syms.txt")):  # nm -n: "<address> <type> <name>"; undefined ones have no address
    parts = line.split()
    if len(parts) < 3: continue
    addrs.append(int(parts[0], 16)); names.append(parts[2].lstrip("_"))
def name(tok):
    if re.fullmatch(r"[0-9a-f]+", tok):
        i = bisect.bisect_right(addrs, int(tok, 16)) - 1
        return names[i] if i >= 0 else tok
    return tok.split("+")[0]
# A thread whose sample stops in one of these is waiting, not working.
WAITS = ("__psynch_cvwait", "__psynch_mutexwait", "mach_msg2_trap", "__ulock_wait", "__ulock_wait2", "semaphore_wait_trap",
         "semaphore_timedwait_trap", "__semwait_signal", "__workq_kernreturn", "kevent", "__select", "nanosleep")
def threads_of(spike_path):
    n = re.search(r"spike-(\d+)-", spike_path).group(1)
    for path in sorted(glob.glob(os.path.join(d, "s-thr-%s-*.txt" % n))):
        tid, start = re.search(r"thr-\d+-\d+-(\d+)-([0-9a-f]+)\.txt", path).groups()
        work = collections.Counter(); busy = waiting = 0
        for line in open(path):
            parts = line.split()
            c = int(parts[0]); stack = [name(t) for t in parts[1:]]
            if stack and stack[0].startswith(WAITS):
                waiting += c
                continue
            busy += c
            for f in set(stack): work[f] += c
        yield tid, start, busy, waiting, work
tot_incl = collections.Counter(); tot_self = collections.Counter(); grand = 0
files = sorted(glob.glob(os.path.join(d, "s-spike-*.txt")), key=lambda p: int(re.search(r"spike-(\d+)-", p).group(1)))
for path in files:
    ms = re.search(r"spike-\d+-(\d+)\.txt", path).group(1)
    self_c = collections.Counter(); incl = collections.Counter(); n = 0
    for line in open(path):
        parts = line.split()
        c = int(parts[0]); stack = [name(t) for t in parts[1:]]
        n += c
        if stack: self_c[stack[0]] += c
        for f in set(stack): incl[f] += c
    grand += n; tot_incl.update(incl); tot_self.update(self_c)
    print("== %s ms, %d samples: self %s" % (ms, n, ", ".join("%s %d" % kv for kv in self_c.most_common(4))))
    print("   on stack: " + ", ".join("%s %d" % kv for kv in incl.most_common(top) if not kv[0].startswith(("start", "main", "boot_run", "run_entry"))))
    for tid, start, busy, waiting, work in threads_of(path):
        if busy:
            print("   thread %s (start %s): %d at work, %d waiting; %s" % (tid, start, busy, waiting, ", ".join(
                "%s %d" % kv for kv in work.most_common(top) if not kv[0].startswith(("_pthread_start", "thread_start")))))
print("\n== all %d spikes, %d samples" % (len(files), grand))
print("self:", ", ".join("%s %d" % kv for kv in tot_self.most_common(top)))
print("on stack:", ", ".join("%s %d" % kv for kv in tot_incl.most_common(top * 2)))
