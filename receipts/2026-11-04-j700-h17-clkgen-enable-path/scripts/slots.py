#!/usr/bin/env python3
"""List every IOKit virtual dispatch (autda .. mov x17,#SLOT .. add x16,x16,x17
.. ldr x8,[x16] .. blraa) inside a VA range, and resolve the slot against a
__ZTV using the Itanium vptr = __ZTV+0x10 convention."""
import struct,sys,json,glob,bisect
from capstone import *
BASE=0xfffffe0007004000
DATA=open('/var/tmp/neo/kernelcache.bin','rb').read()
SYMS={}
for f in glob.glob('/var/tmp/neo/syms_*.json'):
    for e in json.load(open(f)):
        if e.get('v') and e.get('t') in (14,15): SYMS.setdefault(e['v'],e['n'])
SK=sorted(SYMS)
def near(a):
    i=bisect.bisect_right(SK,a)-1
    return SYMS[SK[i]]+('' if a==SK[i] else '+%#x'%(a-SK[i])) if i>=0 else '?'
def wd(va):
    o=va-BASE
    return struct.unpack_from('<Q',DATA,o)[0]
def slotname(zv,slot):
    v=wd(zv+0x10+slot)
    if not v: return 'NULL'
    return near(BASE+(v & 0xffffffff))
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
lo,hi=int(sys.argv[1],16),int(sys.argv[2],16)
zv=int(sys.argv[3],16) if len(sys.argv)>3 else 0xfffffe0008060c30
ins=list(md.disasm(DATA[lo-BASE:hi-BASE],lo))
for i in range(len(ins)-4):
    if ins[i].mnemonic=='autda' and ins[i+1].mnemonic=='mov' and ins[i+1].op_str.startswith('x17, #0x') \
       and ins[i+2].mnemonic=='add' and ins[i+3].mnemonic=='ldr' and ins[i+3].op_str.startswith('x8, [x16]'):
        slot=int(ins[i+1].op_str.split('#')[1],16)
        print('%#x slot +%#04x -> %s'%(ins[i+1].address,slot,slotname(zv,slot)))
