#!/usr/bin/env python3
# tools/sample_subtree.py <sample.txt> <fn substring> [top]: within the subtree of a function on the guest main thread,
# self time by kind (translated, native g3n, kit, system, waiting) and the top kit/system symbols.
import re, sys, collections
lines = open(sys.argv[1]).read().split('\n'); key = sys.argv[2]
pat = re.compile(r'^(\s+)([+!:| ]+)(\d+)\s+(.*?)(\s+\(in ([^)]*)\))?(\s+\+.*)?$')
threads = []; cur = None; stack = []
for l in lines:
    m = re.match(r'^    (\d+) (Thread_\S+)', l)
    if m:
        cur = {'total': int(m.group(1)), 'recs': [], 'fn': 0}; threads.append(cur); stack = []; continue
    if l.startswith('Total number in stack'): cur = None
    m = pat.match(l)
    if not m or cur is None: continue
    d = len(m.group(1)) + len(m.group(2)); n = int(m.group(3)); name = m.group(4).strip(); lib = m.group(6) or ''
    while stack and stack[-1][0] >= d: stack.pop()
    if stack: stack[-1][3][0] -= n
    inside = (stack and stack[-1][4]) or key in name
    if name.startswith('fn_'): cur['fn'] += n
    rec = [d, name, lib, [n], inside]; stack.append(rec); cur['recs'].append(rec)
main = max(threads, key=lambda t: t['fn'])
def kind(name, lib):
    if re.match(r'(fn|body)_[0-9a-f]{8}', name): return 'przetlumaczony'
    if name.startswith('g3n_') or name.startswith('g3x_') or name.startswith('px'): return 'natywny'
    if any(w in name for w in ('psynch', 'semaphore', 'semwait', 'mach_msg', 'kernreturn')): return 'czekanie'
    if lib == 'Gothic3Recomp': return 'kit'
    return 'system/sterownik'
kinds = collections.Counter(); syms = collections.Counter(); tot = 0
for d, name, lib, n, inside in main['recs']:
    if inside:
        kinds[kind(name, lib)] += n[0]; tot += n[0]
        if kind(name, lib) not in ('przetlumaczony', 'natywny'): syms[name[:80]] += n[0]
T = main['total']
print(f'{key}: {tot} z {T} probek watku ({tot * 100.0 / T:.1f}%)')
for k, n in kinds.most_common(): print(f'  {n * 100.0 / T:5.1f}%  {k}')
for s, n in syms.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 15): print(f'    {n * 100.0 / T:5.1f}%  {s}')
