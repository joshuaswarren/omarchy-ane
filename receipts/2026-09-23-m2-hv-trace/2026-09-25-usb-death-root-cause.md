# The 34-36 s hv link loss: the guest has no root filesystem (2026-09-25)

## Symptom
Every m1n1 hv run of the macOS 13.5 kernelcache on the M2 ends with pyserial
"device reports readiness to read but returned no data" in `Uart.readfull()`
under `hv_start`. That is EOF on the proxy tty, the signature of the USB
device going away. The time from `run_guest` launch to the loss does not
depend on what is traced or on which USB nodes the guest sees:

| run | guest ADT | traced | trace size | launch to loss |
|---|---|---|---|---|
| trace-135, trace-135b | upstream port-0 scrub | ANE, pmgr, ISP ps | up to 3075 MMIO events | 35 s |
| 20260924T204142 (atcrt) | port-0 scrub + all atcrt | ANE, pmgr, ISP ps | 429 KB | 34 s |
| v4-20260925T114317 | `--strip-node usb-drd --strip-node atc` | ANE windows + AIC | 1.04 MB | 34 s |
| v4-20260925T114803 | same | ANE windows only (v5) | 138 KB | 36 s |

A trace flood cannot explain this: the traced volume differs by 7x and the
time does not move, and a slow link does not return EOF. The USB/ATC stack
cannot explain it either: the guest dies at the same time with every ATC
port, PHY, DRD and retimer node removed. The retimers were never in the
path: m1n1's gadget runs USB 2.0 high speed only (`src/usb_dwc3.c:1189-1190`
at m1n1 4184923).

## Cause
The guest boots with the ADT of the Asahi stub, and the stub cannot run
macOS. asahi-installer writes only `usr/standalone/bootcaches.plist`, the
volume icon, `SystemVersion.plist`, `PlatformSupport.plist` and the
"Finish Installation" app to the stub's System volume (`src/stub.py:269-290`).
There is no sealed root snapshot, no launchd and no userspace, so XNU has
nothing to mount. The 13.5 RELEASE kernelcache carries the panics for that
path (`vfs_mountroot() failed`, `Failed to find the root snapshot ...
Rooting from the live fs of a sealed volume is not allowed on a RELEASE
build`). XNU's panic path resets the SoC, and the reset removes the proxy.
The m1n1 hv does not stop a guest reset; its maintainer diagnosed the same
signature this way in AsahiLinux/m1n1#196 ("the guest is panicking and
rebooting the machine ... If you try to boot the macOS kernel under an Asahi
Linux volume it'll just panic when it can't mount the root filesystem").
The last traced writes in every run are driver power-downs that end device
probing (ANE islands, ANE_SYS `0x28e080260 = 0xf0003f0`, ISP, i2c6), which
is where boot moves on to mounting root.

Status: the root-filesystem gap is established from source. The panic text
itself has not been captured, because nobody opened the hv vuart:
m1n1 drops vuart bytes until the host raises DTR on the second ACM port
(`src/usb_dwc3.c:775-783`, `usb_dwc3_queue` returns 0 while `!ready` at
:1352). None of the runs above read that port.

## Keeping the link up
`tools/m2hv_catch_and_run.sh` changes four things against the v4 launcher:
1. It opens the second ACM port (the vuart) before the guest starts and logs
   it with UTC stamps to `vuart.log`, and it stamps the moment the port goes
   away.
2. It passes the boot-args that the Asahi m1n1-hypervisor guide uses for
   macOS 13.5 guests: `debug=0x14e serial=3 apcie=0xfffffffe
   -enable-kprintf-spam wdt=-1 clpc=0`. `serial=3` routes the console to
   the vuart, `wdt=-1` keeps the guest from arming the watchdog, and
   `debug=0x14e` carries DB_NMI, which is what sets `panicDebugging` on
   arm64; with it set, every reboot in the panic path is skipped and the
   kernel spins (xnu-8796.141.3 `osfmk/kern/debug.c`, `panic_init` and
   `debugger_collect_diagnostics`).
3. It passes `-d`, which sets `/chosen/debug-enabled` to 1. XNU takes
   `debug=` only with that property set (`pexpert/arm/pe_init.c`,
   `PE_i_can_has_debugger`); the stub policy is `bputil -nc` and the J414c
   ADT carries `debug-enabled = 0`.
4. It preloads `tools/m2hv_guest_debug.py`, which writes
   `/chosen/asmb lp-sip0 = 0x7f` as an 8-byte word. `panic_init` sets
   `panicDebugging` only when `csr_check(CSR_ALLOW_KERNEL_DEBUGGER)`
   passes, and `csr_config` is read from that word
   (`bsd/kern/kern_csr.c`, `csr_bootstrap`; a word of any other size
   panics in `_csr_get_dt_uint64`). asahi-installer's step2 runs
   `bputil -nc` and no `csrutil`, so nothing in the stub disables SIP.

Checks run on this host: the module was run against the decoded J414c ADT
with `lp-sip0` absent and with it present as an existing 8-byte word; both
rebuilt to an 8-byte little-endian `0x7f` that the ADT parser reads back.
A PTY smoke test drove the script with a stub `run_guest.py`: it waited
for two late-appearing ports, gave the first to `M1N1DEVICE`, passed `-d`,
the extra options, both `-m` modules, `-l`, the payload, `--` and the
boot-args string through unchanged, logged vuart lines with stamps,
stamped the port loss with the same pyserial message as the real deaths,
and exited with `run_guest`'s status.

## Run
On the proxy host (jwm1 Linux, the `~/m2proxy` flow), with both tools
files copied next to each other, inside tmux:

    cd ~/m2proxy && env M1N1TIMEOUT=600 PYTHONPATH="$HOME/m2proxy/pylib:." \
      ./m2hv_catch_and_run.sh ~/m2proxy/hvproxy/proxyclient \
      ~/m2proxy/kernelcache.mac13j ~/m2proxy/trace_ane_v5.py \
      ~/m2proxy/hvlogs/v6-$(date -u +%Y%m%dT%H%M%S) \
      --strip-node usb-drd --strip-node atc

Then reboot the M2 into the proxy window as before. Expected: `run.log`
shows `Setting boot arguments to ...`, `guest /chosen/asmb lp-sip0 = 0x7f`
and the ADT upload; `vuart.log` carries XNU's console from `serial=3`, and
34-36 s after launch a `panic(cpu N caller ...)` line, predicted to be the
root-mount panic, followed by the kernel parking with the ACM still
enumerated and `run_guest` still running. C-c in the tmux pane opens the
hv shell; `p.reboot()` restarts the M2. If the port still goes away, the
stamped `vuart lost` line and the last guest lines before it say what the
guest printed last.

A retro check that needs no hardware run: `journalctl --list-boots` on the
M2's Linux. A boot that begins about 80-100 s after each death (20:42:57,
11:44:14 and 11:49:04 UTC) without a reboot command from anyone is the SoC
resetting itself at the death.

## What it does not fix
It keeps the proxy alive and shows the panic. It cannot give the guest a
userspace, so the ANE user-client start (aned/CoreML) still cannot happen in
this guest. That needs a full macOS 13.5 install in its own APFS volume on
the M2, with m1n1 as that volume's boot object (`bputil -nkcas`,
`csrutil disable`, `kmutil configure-boot -c m1n1.bin --raw --entry-point
2048 --lowest-virtual-address 0 -v <volume>`), which is the setup the Asahi
guide lists for 13.5 on M2.

## Removed
`tools/m1n1-patches/0001-hv-scrub-atcrt.patch` and its README claimed the
retimer burst caused the link loss. The atcrt rerun disproved that, the
gadget is USB 2.0 only, and the current launcher's upstream
`--strip-node atc` already removes `atcrt*` along with every ATC node.
