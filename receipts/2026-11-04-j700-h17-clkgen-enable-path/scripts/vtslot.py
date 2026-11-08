#!/usr/bin/env python3
"""Resolve an IOKit __ZTV vtable 'code slot' offset to a symbol.

Apple kexts are Itanium-ABI C++: __ZTV<Class> points at
[offset_to_top, typeinfo, entry0, entry1, ...], and the object's vptr points
at __ZTV + 0x10.  So a code sequence

    ldr x16,[x0] ; mov x17,#SLOT ; add x16,x16,x17 ; ldr x8,[x16]

resolves to *( __ZTV + 0x10 + SLOT ).

usage: vtslot.py <__ZTV-symbol-address> <slot> [<slot> ...]
       vtslot.py --find <symbol-substring>     # search all known vtables
"""
import struct,sys,json,bisect,glob

BASE=0xfffffe0007004000
KC='/var/tmp/neo/kernelcache.bin'
DATA=open(KC,'rb').read()

SYMS={}
VT={}
for f in glob.glob('/var/tmp/neo/syms_*.json'):
    for e in json.load(open(f)):
        v=e.get('v'); n=e.get('n','')
        if not v or e.get('t') not in (14,15): continue
        SYMS.setdefault(v,n)
        if n.startswith('__ZTV'): VT[v]=n
SK=sorted(SYMS)

def near(a):
    i=bisect.bisect_right(SK,a)-1
    if i<0: return '?'
    k=SK[i]
    return SYMS[k]+('' if a==k else '+%#x'%(a-k))

def word(va):
    o=va-BASE
    if o<0 or o+8>len(DATA): return None
    return struct.unpack_from('<Q',DATA,o)[0]

def resolve(zv,slot):
    w=word(zv+0x10+slot)
    if w is None: return None
    if not (w & 0xffffffff): return ('NULL',0)
    tgt=BASE+(w & 0xffffffff)
    return (near(tgt),tgt)

def find(sub):
    hits=[]
    for zv,nm in VT.items():
        for slot in range(0,0xc00,8):
            w=word(zv+0x10+slot)
            if not w: continue
            tgt=BASE+(w & 0xffffffff)
            s=SYMS.get(tgt)
            if s and sub in s: hits.append((nm,slot,s,tgt))
    return hits

if __name__=='__main__':
    if sys.argv[1]=='--find':
        for nm,slot,s,tgt in find(sys.argv[2]):
            print('%-46s +%#06x -> %#x %s'%(nm,slot,tgt,s))
    else:
        zv=int(sys.argv[1],16)
        for a in sys.argv[2:]:
            slot=int(a,16)
            r=resolve(zv,slot)
            print('%s +%#06x -> %s'%(VT.get(zv,'?'),slot,r))
