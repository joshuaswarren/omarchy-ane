#!/usr/bin/env python3
"""Find every 'mov x17,#SLOT' dispatch site in a VA range, annotated with the
nearest preceding symbol (function) and the resolved ApplePMGR __ZTV target."""
import struct,sys,json,glob,bisect
from capstone import *
BASE=0xfffffe0007004000
DATA=open('/var/tmp/neo/kernelcache.bin','rb').read()
SYMS={}
for f in glob.glob('/var/tmp/neo/syms_*.json'):
    for e in json.load(open(f)):
        if e.get('v') and e.get('t') in (14,15) and not e['n'].startswith('__ZTV'):
            SYMS.setdefault(e['v'],e['n'])
SK=sorted(SYMS)
def near(a):
    i=bisect.bisect_right(SK,a)-1
    return (SK[i],SYMS[SK[i]]) if i>=0 else (0,'?')
def wd(va):
    return struct.unpack_from('<Q',DATA,va-BASE)[0]
ZV=0xfffffe0008060c30
def slotname(slot):
    v=wd(ZV+0x10+slot)
    if not v: return 'NULL'
    t=BASE+(v&0xffffffff); k,n=near(t)
    return n+('' if t==k else '+%#x'%(t-k))
lo,hi=int(sys.argv[1],16),int(sys.argv[2],16)
slots=[int(x,16) for x in sys.argv[3:]]
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
ins=list(md.disasm(DATA[lo-BASE:hi-BASE],lo))
for i in range(len(ins)-3):
    if ins[i].mnemonic=='mov' and ins[i].op_str.startswith('x17, #0x') \
       and ins[i+1].mnemonic=='add' and ins[i+2].mnemonic=='ldr' and ins[i+2].op_str.startswith('x8, [x16]'):
        try: slot=int(ins[i].op_str.split('#')[1],16)
        except: continue
        if slots and slot not in slots: continue
        k,n=near(ins[i].address)
        print('%s+%#x  @%#x  slot +%#04x -> %s'%(n,ins[i].address-k,ins[i].address,slot,slotname(slot)))
