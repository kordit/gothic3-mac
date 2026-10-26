#!/usr/bin/env python3
# tools/sample_self.py <sample.txt> [top]: self time of the guest main thread (the busiest Thread_ running fn_ code)
# by symbol and by kind (translated fn_/body_, native g3n_, kit runtime, system, GPU driver).
import re, sys, collections
lines = open(sys.argv[1]).read().split('\n')
threads = []; cur = None; stack = []
pat = re.compile(r'^(\s+)([+!:| ]+)(\d+)\s+(.*?)(\s+\(in ([^)]*)\))?(\s+\+.*)?$')
for l in lines:
    m = re.match(r'^    (\d+) (Thread_\S+)', l)
    if m:
        cur = {'name': m.group(2), 'total': int(m.group(1)), 'self': collections.Counter(), 'fn': 0}; threads.append(cur); stack = []; continue
    if l.startswith('Total number in stack'): cur = None
    m = pat.match(l)
    if not m or cur is None: continue
    d = len(m.group(1)) + len(m.group(2)); n = int(m.group(3)); name = m.group(4).strip(); lib = m.group(6) or ''
    while stack and stack[-1][0] >= d: stack.pop()
    if stack: stack[-1][2][0] -= n
    if name.startswith('fn_'): cur['fn'] += n
    rec = [d, (name, lib), [n]]; stack.append(rec)
    cur.setdefault('recs', []).append(rec)
main = max(threads, key=lambda t: t['fn'])
self = collections.Counter()
for r in main.get('recs', []): self[r[1]] += r[2][0]
kinds = collections.Counter()
def kind(name, lib):
    if re.match(r'(fn|body)_[0-9a-f]{8}', name): return 'przetlumaczony'
    if name.startswith('g3n_') or name.startswith('px'): return 'natywny g3n'
    if lib == 'Gothic3Recomp': return 'kit (runtime, d3d9, host)'
    if 'AGX' in lib or 'Metal' in lib or 'IOGPU' in lib: return 'sterownik GPU'
    if 'psynch' in name or 'semaphore' in name or 'semwait' in name or 'mach_msg' in name or 'kernreturn' in name: return 'czekanie'
    return 'system (' + lib + ')'
for (name, lib), n in self.items(): kinds[kind(name, lib)] += n
tot = main['total']
print(f"{main['name']}: {tot} probek")
for k, n in kinds.most_common(): print(f"  {n * 100.0 / tot:5.1f}%  {k}")
print('top kit/system:')
for (name, lib), n in self.most_common(400):
    if kind(name, lib) not in ('przetlumaczony', 'natywny g3n') and n * 1000 >= tot * 3:
        print(f"  {n * 100.0 / tot:5.1f}%  {name[:90]} ({lib})")
print('top przetlumaczone:')
i = 0
for (name, lib), n in self.most_common(400):
    if kind(name, lib) == 'przetlumaczony':
        print(f"  {n * 100.0 / tot:5.1f}%  {name}"); i += 1
        if i >= int(sys.argv[2] if len(sys.argv) > 2 else 25): break
