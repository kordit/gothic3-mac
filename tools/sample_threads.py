#!/usr/bin/env python3
# tools/sample_threads.py <sample.txt> [thread-substring] [top]: macOS `sample <pid> <s> 1 -file X` per thread: busy vs waiting, self and inclusive top functions.
import re, sys, collections
# parse macOS `sample` call graph: per thread, self counts and inclusive counts
lines=open(sys.argv[1]).read().split('\n')
start=next(i for i,l in enumerate(lines) if l.startswith('Call graph:'))
end=next(i for i,l in enumerate(lines) if l.startswith('Total number in stack'))
pat=re.compile(r'^(\s*)([+!:| ]*)\s*(\d+)\s+(.*)$')
threads=collections.OrderedDict(); cur=None; stack=[]
for l in lines[start+1:end]:
    if not l.strip(): continue
    m=re.match(r'^    (\d+) (Thread_\S+.*)$',l)
    if m:
        cur=m.group(2); threads[cur]={'total':int(m.group(1)),'self':collections.Counter(),'incl':collections.Counter()}; stack=[]; continue
    m=re.match(r'^(\s+)([+!:| ]+)(\d+)\s+(.*)$',l)
    if not m or cur is None: continue
    depth=len(m.group(1))+len(m.group(2)); n=int(m.group(3)); name=m.group(4)
    name=re.sub(r'\s+\(in [^)]*\).*$','',name); name=re.sub(r'\s+\[0x.*$','',name)
    while stack and stack[-1][0]>=depth: stack.pop()
    # self = n - sum(children); approximate by subtracting from parent
    if stack: stack[-1][2][0]-=n
    rec=[depth,name,[n]]
    stack.append(rec)
    t=threads[cur]
    if name not in [s[1] for s in stack[:-1]]: t['incl'][name]+=n
    t.setdefault('nodes',[]).append(rec)
for tn,t in threads.items():
    for rec in t.get('nodes',[]): t['self'][rec[1]]+=rec[2][0]
want=sys.argv[2] if len(sys.argv)>2 else None
for tn,t in threads.items():
    if want and want not in tn: continue
    wait=sum(v for k,v in t['self'].items() if re.search(r'wait|trap|kernreturn|semwait',k))
    print('==',tn,'total',t['total'],'waiting',wait)
    if want:
        print('-- self'); [print(f'{v:6d} {k}') for k,v in t['self'].most_common(int(sys.argv[3]) if len(sys.argv)>3 else 40)]
        print('-- incl'); [print(f'{v:6d} {k}') for k,v in t['incl'].most_common(int(sys.argv[3]) if len(sys.argv)>3 else 40)]
