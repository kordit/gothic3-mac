#!/usr/bin/env python3
# tools/sample_keys.py <sample.txt> [fn ...]: share of the guest main thread (the busiest Thread_) spent in each
# function, inclusive and counted once per stack; default keys are the fight profile buckets (PROGRESS sesja 10/11).
import re, sys, collections
KEYS = {'10a74b60': 'AI gCScriptRoutine', '103aed10': 'czasteczki', '109f2120': 'ruch Game', '10292d40': 'animacja',
        '103d3e90': 'PhysX Simulate', '1027d680': 'moduly', '10400530': 'Execute (rys.)', '102766f0': 'cien LsPSM',
        '1033f530': 'GUI', '11217ca0': 'PhysXCore', 'Present': 'Present'}
lines = open(sys.argv[1]).read().split('\n')
want = sys.argv[2:] or list(KEYS)
threads = []; cur = None; stack = []
for l in lines:
    m = re.match(r'^    (\d+) (Thread_\S+)', l)
    if m:
        cur = {'name': m.group(2), 'total': int(m.group(1)), 'hit': collections.Counter()}; threads.append(cur); stack = []; continue
    m = re.match(r'^(\s+)([+!:| ]+)(\d+)\s+(.*)$', l)
    if not m or cur is None: continue
    d = len(m.group(1)) + len(m.group(2)); n = int(m.group(3)); name = m.group(4)
    while stack and stack[-1][0] >= d: stack.pop()
    for k in want:
        if k in name and not any(k in s[1] for s in stack): cur['hit'][k] += n
    stack.append((d, name))
# the guest main thread: the one with the most samples under recomp code (PhysX/Execute/modules)
main = max(threads, key=lambda t: sum(t['hit'].values()))
idle = 0
print(f"{sys.argv[1]}: {main['name']} {main['total']} probek")
for k in want:
    print(f"  {main['hit'][k] * 100.0 / main['total']:5.1f}%  {k} {KEYS.get(k, '')}")
