#!/usr/bin/env python3
# tools/sample_modules.py <sample.txt>: self time of translated code on the guest main thread by module (game.toml
# address ranges of the staged DLLs), and of native g3n_ code.
import re, sys, collections, struct, glob, os
def ranges():
    out = []
    for p in glob.glob('stage/*.dll') + glob.glob('stage/*.exe') + glob.glob('stage/*.DLL'):
        d = open(p, 'rb').read(4096)
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        base = struct.unpack_from('<I', d, pe + 24 + 28)[0]; size = struct.unpack_from('<I', d, pe + 24 + 56)[0]
        out.append((base, base + size, os.path.basename(p)))
    return out
R = ranges()
lines = open(sys.argv[1]).read().split('\n')
pat = re.compile(r'^(\s+)([+!:| ]+)(\d+)\s+(.*?)(\s+\(in ([^)]*)\))?(\s+\+.*)?$')
threads = []; cur = None; stack = []
for l in lines:
    m = re.match(r'^    (\d+) (Thread_\S+)', l)
    if m: cur = {'total': int(m.group(1)), 'recs': [], 'fn': 0}; threads.append(cur); stack = []; continue
    if l.startswith('Total number in stack'): cur = None
    m = pat.match(l)
    if not m or cur is None: continue
    d = len(m.group(1)) + len(m.group(2)); n = int(m.group(3)); name = m.group(4).strip()
    while stack and stack[-1][0] >= d: stack.pop()
    if stack: stack[-1][2][0] -= n
    if name.startswith('fn_'): cur['fn'] += n
    rec = [d, name, [n]]; stack.append(rec); cur['recs'].append(rec)
main = max(threads, key=lambda t: t['fn'])
mods = collections.Counter()
for d, name, n in main['recs']:
    m = re.match(r'(fn|body|g3n|g3x)_([0-9a-f]{8})', name)
    if not m: continue
    a = int(m.group(2), 16)
    mod = next((r[2] for r in R if r[0] <= a < r[1]), '?')
    mods[('natywny ' if m.group(1) in ('g3n', 'g3x') else '') + mod] += n[0]
T = main['total']
for k, v in mods.most_common(): print(f'{v * 100.0 / T:5.1f}%  {k}')
