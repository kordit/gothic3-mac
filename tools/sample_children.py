#!/usr/bin/env python3
# tools/sample_children.py <one-thread call graph> <function> [top]: inclusive samples of each direct callee of <function> (call glue skipped).
import re, sys, collections
# One thread: awk "/Thread_<main id>/{on=1} /Thread_<next id>/{on=0} on" sample.txt > main.txt
# children (inclusive) of a function on one thread's call graph, merged over all its occurrences
lines=open(sys.argv[1]).read().split('\n'); target=sys.argv[2]
kids=collections.Counter(); tot=0; stack=[]
for l in lines:
    m=re.match(r'^(\s+)([+!:| ]+)(\d+)\s+(.*)$',l)
    if not m: stack=[]; continue
    d=len(m.group(1))+len(m.group(2)); n=int(m.group(3)); name=re.sub(r'\s+\(in .*$','',m.group(4))
    while stack and stack[-1][0]>=d: stack.pop()
    names=[s[1] for s in stack]
    if name==target and target not in names: tot+=n
    # direct "meaningful" child: first guest/native fn after target, skipping call glue
    if target in names and target!=name:
        i=len(names)-1-names[::-1].index(target)
        between=[x for x in names[i+1:] if not re.match(r'recomp_|imports_dispatch|recomp_shim_call',x)]
        if not between and not re.match(r'recomp_|imports_dispatch|recomp_shim_call',name) and target not in names[i+1:]:
            kids[name]+=n
    stack.append((d,name))
print(target,'total',tot)
for k,v in kids.most_common(int(sys.argv[3]) if len(sys.argv)>3 else 15): print(f'{v:6d} {k}')
