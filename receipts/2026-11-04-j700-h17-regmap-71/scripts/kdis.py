import sys
from capstone import *
from capstone.arm64 import *
BASE=0xfffffe0007004000
data=open('/var/tmp/neo/kernelcache.bin','rb').read()
def off(va): return va-BASE
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
md.detail=True
def dis(va,n=60,showbytes=True):
    o=off(va)
    for i in md.disasm(data[o:o+n*4],va):
        bs=' '.join('%02x'%b for b in i.bytes)
        print('%#x: %-32s %-8s %s'%(i.address,bs,i.mnemonic,i.op_str))
        if i.mnemonic in ('ret','br','b') and i.address!=va:
            pass
if __name__=='__main__':
    va=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 60
    dis(va,n)
