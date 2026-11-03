# SPDX-License-Identifier: MIT
"""Trace how macOS starts the Apple Neural Engine on an M3-family Mac (H15: T8122, T6030, T6031).

An m1n1 hypervisor script. It is PASSIVE: it never reads or writes a device register itself, and it
never touches guest memory. It only logs what the macOS guest does to the ANE:

  * the ANE power-domain registers in the power manager (ps registers of the ANE devices),
  * the ASC wrapper (CPU_CONTROL, CPU_STATUS, the mailbox control words), RVBAR, the SCRATCH words and the
    doorbell, and a second register block at engine + 0x1900000,
  * the ANE DART windows (raw register accesses; m1n1's DARTTracer is not used, because its start reads the
    DART registers before the guest has powered the ANE), and
  * with ANE_TRACE_FULL=1, every access to the whole ANE aperture until the event cap.

Run it from an m1n1 checkout with the Mac in proxy mode and macOS as the guest, then use the ANE in the
guest (one CoreML model run on the Neural Engine):

    python3 proxyclient/tools/run_guest.py -l ane-trace.log \\
        -m proxyclient/hv/trace_ane_h15.py <your usual run_guest.py arguments and kernelcache>

Do not add other m1n1 tracers to the same run unless you know they read nothing: several prime their register
cache with reads when they start, and a read of an unpowered ANE block can fault the host.

Environment:
    ANE_TRACE_PLAN_ONLY=1  print every range the script would hook and stop; installs no hook
    ANE_TRACE_FULL=1       hook the whole ANE aperture, not only the named windows (heavy; use for the boot only)
    ANE_TRACE_MAX=N        stop logging after N events (default 2000000)

Scope: before it hooks anything the script prints one `hook <start> +<size> (<what>)` line per range. Each range
is a trapping window that m1n1 maps in 16 KiB pages: the named ANE windows, the ANE DART windows from the ADT, and
the 16 KiB power-manager page of every ANE ps register. Other registers that share a page are trapped too and
are not logged.

Event cap: when N events have been logged the script stops logging and puts every range back on the hardware
mapping (a TraceMode.OFF entry per range). m1n1 applies that at the next hypervisor exception, so the hooks can
stay installed for a short time after the cap, silently.

Output lines: `<seconds since first event> W|R<width> <label> <offset or address> = <value> [note]`.
Consecutive identical accesses (polling) collapse into one line with `x<count>`.
"""
import os
import time

from m1n1.hv import TraceMode
from m1n1.trace import Tracer
from m1n1.utils import irange

CHIPS = {0x8122: "T8122", 0x6030: "T6030", 0x6031: "T6031"}
PAGE = 0x4000

# Offsets from the engine base (ANE reg 0). DECODED: read in the macOS kext (receipts/2026-10-28-m3-ane).
# INFERRED: labelled by analogy with the Linux apple-mailbox layout; the offset is real, the name is a guess.
NAMED = {
    0x1050000: "RVBAR",
    0x1400044: "CPU_CONTROL",
    0x1400048: "CPU_STATUS",
    0x1408110: "ASC_INBOX0_CTRL",
    0x1408114: "ASC_OUTBOX0_CTRL",
    0x1408830: "ASC_I2A_RECV0 (inferred)",
    0x1408838: "ASC_I2A_RECV1 (inferred)",
    0x1844000: "DOORBELL",
    0x1840078: "SET2_UNDECODED",
    0x1840080: "SET2_UNDECODED",
    0x1840084: "SET2_UNDECODED",
    0x1908000: "SET2_UNDECODED",
}
for _i in range(8):
    NAMED[0x1840050 + 4 * _i] = f"SCRATCH{_i}"

# (offset from the engine base, size, what it covers). The DART windows come from the ADT.
WINDOWS = [
    (0x1050000, PAGE, "RVBAR page"),
    (0x1400000, 0x10000, "ASC wrapper"),
    (0x1840000, 0x8000, "SCRATCH words and doorbell"),
    (0x1900000, 0x10000, "second register block"),
]

READY_WORD = 0x08042006
WAKE_WORD = 0xF7FBDFF9
RVBAR_IOVA_MASK = 0xFF7EFFFFFFFFF800


def note_for(label, write, value):
    """A short note for the accesses that mark steps of the start sequence."""
    if label == "CPU_CONTROL" and write:
        return {0: "<release: 0>", 0x10: "<RUN>"}.get(value, "")
    if label == "RVBAR" and write:
        return f"<iova {value & RVBAR_IOVA_MASK:#x} valid {value & 1}>"
    if label == "SCRATCH7":
        if value == READY_WORD and not write:
            return "<firmware ready word>"
        if value == WAKE_WORD and write:
            return "<wake word>"
    return ""


class Labeler:
    """Names an absolute address: a power-domain register, a DART window, a named engine register or an engine offset."""

    def __init__(self, engine, engine_size, ps_names, dart_windows=()):
        self.engine = engine
        self.engine_size = engine_size
        self.ps_names = ps_names
        self.dart_windows = list(dart_windows)

    def label(self, addr):
        if addr in self.ps_names:
            return f"PS:{self.ps_names[addr]}", f"{addr:#x}"
        for start, size, name in self.dart_windows:
            if start <= addr < start + size:
                return name, f"+{addr - start:#x}"
        off = addr - self.engine
        if 0 <= off < self.engine_size:
            return NAMED.get(off, "ENG"), f"+{off:#x}"
        return None, None


def make_plan(engine, engine_size, darts, ps_pages, full):
    """Every range the script hooks, as (start, size, what it covers)."""
    plan = []
    if full:
        plan.append((engine, engine_size, "whole ANE aperture"))
    else:
        for off, size, why in WINDOWS:
            plan.append((engine + off, size, why))
    for start, size, name in darts:
        if not (full and engine <= start < engine + engine_size):
            plan.append((start, size, f"ANE DART window {name}"))
    for page in ps_pages:
        plan.append((page, PAGE, "power-manager page of an ANE ps register"))
    return plan


class ANEH15Tracer(Tracer):
    DEFAULT_MODE = TraceMode.ASYNC

    def __init__(self, hv, labeler, cap, full):
        super().__init__(hv, verbose=0, ident="ANEH15")
        self.labeler = labeler
        self.cap = cap
        self.full = full
        self.zones = []
        self.events = 0
        self.t0 = None
        self.pending = None
        self.pending_count = 0
        self.pending_time = 0.0
        self.capped = False

    def watch(self, plan):
        """Install one hook for every range of the plan."""
        for start, size, _why in plan:
            self.zones.append((start, size))
            self.trace(start, size, self.DEFAULT_MODE)

    def release(self):
        """Give every hooked range back to the hardware mapping.

        A tracer entry in TraceMode.OFF makes m1n1 map the range straight to the hardware. Removing the entries
        with clear_tracers() would not return the range to the hardware: pt_update() drops the emptied entries
        (mmio_maps.compact()) and unmaps the dirty range, so the guest's accesses would fault. The change is
        applied by pt_update() at the next hypervisor exception, not at once.
        """
        for start, size in self.zones:
            self.hv.add_tracer(irange(start, size), self.ident, TraceMode.OFF)

    def evt_rw(self, evt, regmap=None, prefix=None):
        label, where = self.labeler.label(evt.addr)
        if label is None:
            return
        if self.events >= self.cap:
            if not self.capped:
                self.flush()
                self.release()
                self.log(f"event cap {self.cap} reached; logging stopped; hooks released "
                         "(hardware mapping returns at the next hypervisor exception)")
                self.capped = True
            return
        self.events += 1
        write = bool(evt.flags.WRITE)
        width = 1 << evt.flags.WIDTH
        now = time.monotonic()
        if self.t0 is None:
            self.t0 = now
        key = (label, where, write, width, evt.data)
        if key == self.pending:
            self.pending_count += 1
            return
        self.flush()
        self.pending = key
        self.pending_count = 1
        self.pending_time = now - self.t0

    def flush(self):
        if self.pending is None:
            return
        label, where, write, width, value = self.pending
        note = note_for(label, write, value)
        line = (f"{self.pending_time:10.6f} {'W' if write else 'R'}{width} {label} {where} = {value:#x}"
                + (f" x{self.pending_count}" if self.pending_count > 1 else "")
                + (f" {note}" if note else ""))
        self.log(line)
        self.pending = None
        self.pending_count = 0


def ane_node(adt):
    for path in ("/arm-io/ane", "/arm-io/ane0"):
        try:
            return path, adt[path]
        except KeyError:
            continue
    raise RuntimeError("no /arm-io/ane or /arm-io/ane0 node in the ADT")


def ps_registers(adt, node):
    """Power-manager ps registers of the ANE devices: every device named ANE*, plus the node's power-gates."""
    pmgr = adt["/arm-io/pmgr"]
    gates = set(getattr(node, "power_gates", None) or [])
    found = {}
    for dev in pmgr.devices:
        if dev.flags.no_ps:
            continue
        name = dev.name if isinstance(dev.name, str) else str(dev.name)
        if name.upper().startswith("ANE") or adt.pmgr_dev_get_id(dev) in gates:
            found[adt.pmgr_dev_get_addr(dev)] = name
    return found


def dart_windows(adt):
    """The register windows of the ANE DART node as (start, size, name); empty if the ADT has none."""
    for path in ("/arm-io/dart-ane", "/arm-io/dart-ane0"):
        try:
            node = adt[path]
        except KeyError:
            continue
        windows = []
        for index in range(len(node.reg)):
            start, size = node.get_reg(index)
            if size:
                windows.append((start, size, f"DART{index}"))
        return windows
    return []


def install(hv):
    adt = hv.adt
    chip = adt["/chosen"].chip_id
    path, node = ane_node(adt)
    engine, engine_size = node.get_reg(0)
    print(f"ANE trace: chip {CHIPS.get(chip, hex(chip))}{'' if chip in CHIPS else ' (not an M3 chip this script was written for)'}")
    print(f"ANE trace: node {path} engine {engine:#x} size {engine_size:#x}")
    for name in ("ane_type", "power_gates", "clock_gates", "clock_ids", "segment_ranges", "pre_loaded"):
        value = getattr(node, name, None)
        if value is not None:
            print(f"ANE trace: {name} = {value}")
    ps = ps_registers(adt, node)
    for addr, name in sorted(ps.items()):
        print(f"ANE trace: ps register {name} at {addr:#x}")
    pages = sorted({addr & ~(PAGE - 1) for addr in ps})
    full = os.environ.get("ANE_TRACE_FULL") == "1"
    cap = int(os.environ.get("ANE_TRACE_MAX", "2000000"))
    darts = dart_windows(adt)
    if not darts:
        print("ANE trace: no ANE DART node found; the DART is not traced")
    plan = make_plan(engine, engine_size, darts, pages, full)
    for start, size, why in plan:
        print(f"ANE trace: hook {start:#x} +{size:#x} ({why})")
    if os.environ.get("ANE_TRACE_PLAN_ONLY") == "1":
        print("ANE trace: plan only; no hook is installed")
        return None
    labeler = Labeler(engine, engine_size, ps, darts)
    tracer = ANEH15Tracer(hv, labeler, cap, full)
    tracer.watch(plan)
    hv.ane_h15_tracer = tracer
    print("ANE trace: running. In the shell, hv.ane_h15_tracer.flush() writes the last pending line.")
    return tracer


if "hv" in globals():
    install(hv)  # noqa: F821
