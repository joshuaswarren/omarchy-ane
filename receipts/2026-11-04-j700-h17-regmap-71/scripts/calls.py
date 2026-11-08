import sys,struct
from capstone import *
from capstone.arm64 import *
BASE=0xfffffe0007004000
data=open('/var/tmp/neo/kernelcache.bin','rb').read()
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
def dis(lo,hi):
    return list(md.disasm(data[lo-BASE:hi-BASE],lo))
def parse_calls(lo,hi,target):
    ins=dis(lo,hi)
    regs={}
    out=[]
    for i in ins:
        m,o=i.mnemonic,i.op_str
        if m=='mov' and o.startswith('w') and ',' in o:
            r,v=o.split(', ')
            if v.startswith('#'):
                try: regs[r]=int(v[1:],0)
                except: regs.pop(r,None)
            else: regs.pop(r,None)
        elif m=='movk' and ',' in o:
            parts=[p.strip() for p in o.split(',')]
            r=parts[0]; v=int(parts[1][1:],0); sh=0
            for p in parts[2:]:
                if p.startswith('lsl'): sh=int(p.split('#')[1],0)
            regs[r]=(regs.get(r,0)&~(0xffff<<sh))|(v<<sh)
        elif m in ('bl','b'):
            tgt=int(o[1:],16) if o.startswith('#') else None
            if tgt==target:
                out.append((i.address,dict(regs)))
            regs={}
        elif m in ('ldr','ldp','str','stp','add','sub','cmp','csel','tbnz','tbz','cbnz','cbz','autda','blraa','ret','retab','br','blraa','adrp','lsr','uxtw','and','lsl','umull','umaddl','madd','b.eq','b.hs','b.lo','b.ne','b.le','b.gt','b.ge'):
            if m in ('add','sub') and ',' in o:
                parts=[p.strip() for p in o.split(',')]
                if len(parts)==3 and parts[2].startswith('#'):
                    try: regs[parts[0]]=regs.get(parts[1],0)+int(parts[2][1:],0)*(1 if m=='add' else -1)
                    except: regs.pop(parts[0],None)
                else: regs.pop(parts[0],None)
    return out
for a,r in parse_calls(0xfffffe0009836e84,0xfffffe00098372d0,0xfffffe000944879c):
    print('call %#x  map=%s idx=%s w3=%s w4=%s'%(a,r.get('w1'),r.get('w2'),r.get('w3'),r.get('w4')))
