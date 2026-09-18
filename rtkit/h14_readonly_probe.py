import os, mmap, struct, sys, time
ANE=0x284000000
fd=os.open("/dev/mem", os.O_RDWR|os.O_SYNC)
def rd(addr):
    b=addr&~0xFFF
    m=mmap.mmap(fd,0x2000,mmap.MAP_SHARED,mmap.PROT_READ|mmap.PROT_WRITE,offset=b)
    v=struct.unpack_from("<I",m,addr-b)[0]
    m.close()
    return v
def pmgr(off):
    return rd(0x28E080000+off)
cpu=pmgr(0x2e0)
print(f"ane_cpu ps: {cpu:#010x} target={cpu&0xF:#x} actual={(cpu>>4)&0xF:#x}")
if (cpu & 0xF) != 0xF:
    print("ane_cpu not powered - stopping (read-only mode, no raise)")
    sys.exit(0)
for name,off in [("CPU_CONTROL",0x1600044),("CPU_STATUS",0x1600048),
                 ("MBOX_A2I_CTRL",0x1608110),("MBOX_I2A_CTRL",0x1608114),
                 ("MBOX_A2I_S0",0x1608800),("MBOX_A2I_S1",0x1608808),
                 ("MBOX_I2A_R0",0x1608830),("MBOX_I2A_R1",0x1608838),
                 ("RTB_M3_IRQ_EN",0x1840048),("RTB_M3_A2I_CTRL",0x1840050),
                 ("RTB_M3_I2A_CTRL",0x1840080),("RTB_M3_I2A_R0",0x18400A0),
                 ("RTB_UNK40",0x1840040),("RTB_UNK44",0x1840044),
                 ("RTB_STATUS7C",0x184007C),("RTB_STATUS88",0x1840088),
                 ("RVBAR",0x1050000),("VERS",0x1840000)]:
    v=rd(ANE+off)
    extra=""
    if "CTRL" in name:
        extra=f" full={bool(v>>16&1)} empty={bool(v>>17&1)} wptr={(v>>8)&0xF} rptr={(v>>12)&0xF}"
    print(f"{name:16s} +{off:#08x} = {v:#010x}{extra}")
print("DONE-ALIVE")
