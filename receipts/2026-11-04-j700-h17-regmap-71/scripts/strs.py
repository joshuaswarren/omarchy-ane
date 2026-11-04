import sys
BASE=0xfffffe0007004000
data=open('/var/tmp/neo/kernelcache.bin','rb').read()
def s(va, maxlen=300):
    o=va-BASE
    e=data.find(b'\0',o,o+maxlen)
    return data[o:e].decode('utf-8','replace')
if __name__=='__main__':
    for a in sys.argv[1:]:
        va=int(a,16)
        print('%#x: %r'%(va,s(va)))
