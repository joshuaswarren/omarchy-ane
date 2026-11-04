import sys
BASE=0xfffffe0007004000
data=open('/var/tmp/neo/kernelcache.bin','rb').read()
target=int(sys.argv[1],16)
res=[]
n=len(data)//4
import array
a=array.array('I')
a.frombytes(data[:n*4])
if sys.byteorder!='little': a.byteswap()
for i,w in enumerate(a):
    if (w & 0xFC000000)==0x94000000:
        imm=w & 0x3FFFFFF
        if imm>=0x2000000: imm-=0x4000000
        pc=BASE+i*4
        if pc+imm*4==target:
            res.append(pc)
print('callsites:',len(res))
for p in res: print('  call_va=%#x  file_off=%#x'%(p,p-BASE))
