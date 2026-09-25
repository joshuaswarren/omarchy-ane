# m1n1 hv module (run_guest.py -m): let a guest panic stop in the debugger
# instead of resetting the SoC, which takes the USB proxy down with it.
#
# XNU 13.5 sets panicDebugging only when debug= carries DB_NMI and
# csr_check(CSR_ALLOW_KERNEL_DEBUGGER) passes (osfmk/kern/debug.c
# panic_init); csr_config is /chosen/asmb lp-sip0, an 8-byte word
# (bsd/kern/kern_csr.c csr_bootstrap, _csr_get_dt_uint64). The Asahi stub's
# boot policy leaves SIP on, so the word is missing or 0. 0x7f is the value
# csrutil disable writes. run_guest -d supplies the other gate,
# /chosen/debug-enabled.
from construct import Int64ul

asmb = hv.adt["/chosen/asmb"]
asmb._types["lp-sip0"] = (Int64ul, False)
asmb.lp_sip0 = 0x7f
print("guest /chosen/asmb lp-sip0 = 0x7f (kernel debugger allowed)")
