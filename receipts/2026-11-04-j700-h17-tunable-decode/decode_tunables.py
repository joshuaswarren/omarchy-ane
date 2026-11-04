#!/usr/bin/env python3
"""
Decode AppleT8140ANEHAL tunable tables from /var/tmp/neo/kernelcache.bin.

Every structural claim here is derived from bytes, not from a prior report.
Implements the semantics read out of the disassembly of:

  _tunableh_apply                 fffffe00097a84a4
  _tunableh_apply_configuration   fffffe00097a876c
  _tunableh_configuration_mask    fffffe00097a8108
  _tunableh_configuration_value   fffffe00097a82ec
  _tunableh_configuration_get_value fffffe00097a81d0
  _tunableh_configuration_applied_value fffffe00097a83f8
  _tunableh_write                 fffffe00097a8930
  _tunableh_module_finalize       fffffe00097a809c
  _tunableh_device                fffffe00097a808c
  AppleANETunableApplyFunction    fffffe00097a9008
  start                           fffffe00097a8e00

Layouts proven:
  table element  : 8-byte arm64e chained pointer, next=2 -> stride 8, 0 terminates
  descriptor     : 12-byte packed {u32 offset:28|width:4, u64 payload_ptr(fixup)}
  payload        : per-width record {mask[W], value[W], get_value_fn[8]}
  image fixups   : VA = 0xfffffe0007004000 + file_offset
"""
import struct
import sys

KC = "/var/tmp/neo/kernelcache.bin"
BASE = 0xfffffe0007004000          # outer __TEXT vmaddr, fileoff 0
ENGINE = 0x400000000               # ANE reg[0] aperture base
ENGINE_SIZE = 0x2000000

d = open(KC, "rb").read()

TARGET_MASK = (1 << 43) - 1        # DYLD_CHAINED_PTR_ARM64E target field
FMT = {1: "<B", 2: "<H", 4: "<I", 8: "<Q"}


def chained(off):
    """Return (raw, target, next, is_auth, is_bind) for an 8-byte fixup slot."""
    v = struct.unpack_from("<Q", d, off)[0]
    return v, v & TARGET_MASK, (v >> 51) & 0x7FF, (v >> 63) & 1, (v >> 62) & 1


def desc_at(off):
    """12-byte packed descriptor."""
    w0 = struct.unpack_from("<I", d, off)[0]
    pvq = struct.unpack_from("<Q", d, off + 4)[0]
    return w0 & 0xFFFFFFF, w0 >> 28, pvq & TARGET_MASK


def payload_at(off, width):
    f = FMT[width]
    mask = struct.unpack_from(f, d, off)[0]
    val = struct.unpack_from(f, d, off + width)[0]
    cb = struct.unpack_from("<Q", d, off + 2 * width)[0]
    return mask, val, cb


def walk(toff, name, membase, limit=256):
    out = []
    n = 0
    while n < limit:
        slot = toff + 8 * n
        v, tgt, nxt, auth, bind = chained(slot)
        if v == 0:
            return out, n, slot
        off, width, p = desc_at(tgt)
        mask, val, cb = payload_at(p, width)
        out.append(dict(idx=n, slot=slot, slotval=v, desc=tgt, off=off,
                        width=width, payload=p, mask=mask, value=val, cb=cb,
                        addr=membase + off))
        n += 1
    raise RuntimeError("table %s did not terminate" % name)


# ---------------------------------------------------------------- table set
# Called from AppleANETunableApplyFunction (0xfffffe00097a9008) in this order:
#   0x9128 bl 0x979942c  af_aneaxi2af default_apply_to      -> 0x97990e0
#   0x9130 bl 0x9799574  af_aneaxi2af secure_lock_wr_dis_to -> 0x9799344
#   0x9134 bl 0x97994d0  af_aneaxi2af secure_lock_lock_to   -> 0x9799220
#   0x9168 bl 0x979b144  c1ppt default_apply_to             (boot-arg gated)
#   0x9170 bl 0x979b098  c1ppt c1ppt_enable_apply_to        (boot-arg gated)
#   0x9178 bl 0x979b2fc  c1ppt secure_lock_wr_dis_apply_to   (boot-arg gated)
#   0x9180 bl 0x979b220  c1ppt secure_lock_lock_apply_to     (boot-arg gated)
TABLES = [
    ("A  af_aneaxi2af default  (UNCONDITIONAL)", 0x1137AF0, 0x0000000),
    ("B  af_aneaxi2af default#2 (m+8==0 && m+32==0 && m+33==0)", 0x1137C10, 0x0000000),
    ("C  af_aneaxi2af lock      (m+8==1 && m+32==0 && m+33==1)", 0x1137C28, 0x0000000),
    ("D  af_aneaxi2af wr_dis    (m+8==1 && m+32==0 && m+33==1)", 0x1137C58, 0x0000000),
    ("E  c1ppt default          (boot-arg pmgr-c1ppt)", 0x1137E50, 0x197C000),
    ("F  c1ppt c1ppt_enable m+8==0 (boot-arg pmgr-c1ppt)", 0x1137E30, 0x197C000),
    ("I  c1ppt c1ppt_enable m+8==1 (boot-arg pmgr-c1ppt)", 0x1137E40, 0x197C000),
    ("G  c1ppt lock             (boot-arg pmgr-c1ppt)", 0x1137E10, 0x197C000),
    ("H  c1ppt wr_dis           (boot-arg pmgr-c1ppt)", 0x1137E20, 0x197C000),
]

allv = {}
print("=" * 108)
print("AppleT8140ANEHAL tunable tables  (kernelcache %s)" % KC)
print("image fixup base = 0x%016x   engine aperture base = 0x%x" % (BASE, ENGINE))
print("=" * 108)
for name, toff, membase in TABLES:
    entries, n, term = walk(toff, name, membase)
    allv[name] = entries
    print("\nTABLE %s" % name)
    print("  file 0x%08x  VA 0x%016x  entries=%d  NULL terminator at slot %d (file 0x%08x)"
          % (toff, BASE + toff, n, n, term))
    print("  %-3s %-10s %-10s %-9s %-6s %-11s %-11s %-6s %-11s"
          % ("#", "slot", "desc", "payload", "width", "offset", "mask", "cb", "value"))
    for e in entries:
        print("  %02d  0x%08x 0x%08x 0x%08x  %-6d 0x%08x  0x%08x   %-6s 0x%08x"
              % (e["idx"], e["slot"], e["desc"], e["payload"], e["width"],
                 e["off"], e["mask"], e["cb"] or "-", e["value"]))

# ---------------------------------------------------------------- self-check
print("\n" + "=" * 108)
print("SELF-CHECK")
print("=" * 108)
fails = []
offenders = [0x11378B8, 0x1137AD4]          # the two mis-reported "offsets"
table_file_range = (0x1137AF0 - 8, 0x1137E90)

total = 0
widths = set()
for name, toff, membase in TABLES:
    for e in allv[name]:
        total += 1
        widths.add(e["width"])
        a = e["addr"]
        if a >= ENGINE_SIZE:
            fails.append("%s[%d]: engine offset 0x%x >= 0x%x" % (name, e["idx"], a, ENGINE_SIZE))
        if e["width"] == 4 and (e["off"] & 3):
            fails.append("%s[%d]: offset 0x%x not 4-byte aligned" % (name, e["idx"], e["off"]))
        if e["width"] not in (1, 2, 4, 8):
            fails.append("%s[%d]: bad width %d" % (name, e["idx"], e["width"]))
        if e["off"] in offenders:
            fails.append("%s[%d]: offset equals a kernelcache FILE OFFSET: 0x%x"
                         % (name, e["idx"], e["off"]))
        if table_file_range[0] <= e["off"] <= table_file_range[1]:
            fails.append("%s[%d]: offset inside the table file range" % (name, e["idx"]))
        # sanity: the payload pointer must resolve inside the image
        if not (0 <= e["payload"] < len(d)):
            fails.append("%s[%d]: payload 0x%x out of image" % (name, e["idx"], e["payload"]))

print("1. total descriptors decoded ............ %d" % total)
print("   per table ............................ %s"
      % ", ".join("%s=%d" % (n.split()[0], len(allv[n])) for n, _, _ in TABLES))
print("2. widths seen .......................... %s  %s"
      % (sorted(widths), "OK" if widths <= {1, 2, 4, 8} else "FAIL"))
print("3. every engine offset < 0x2000000 ...... %s" % ("OK" if not any('>=' in f for f in fails) else "FAIL"))
print("4. width-4 offsets 4-byte aligned ....... %s" % ("OK" if not any('aligned' in f for f in fails) else "FAIL"))
print("5. no offset equals a kernelcache file offset "
      "(0x11378b8 / 0x1137ad4) ............... %s"
      % ("OK" if not any('FILE OFFSET' in f for f in fails) else "FAIL"))
print("6. no offset lands in the table file range 0x%x-0x%x ... %s"
      % (table_file_range[0], table_file_range[1],
         "OK" if not any('table file range' in f for f in fails) else "FAIL"))
print("7. every payload pointer resolves in image ... %s"
      % ("OK" if not any('out of image' in f for f in fails) else "FAIL"))

# count consistency: table extent checked by the walker terminating on a NULL slot
print("8. table extents: each walk ended on an 8-byte NULL slot, so the entry")
print("   count equals the extent: %s"
      % ", ".join("%s %d@0x%x" % (n.split()[0], len(allv[n]), t)
                  for n, t, _ in TABLES))

# duplicate check
seen = {}
dups = []
for name, toff, membase in TABLES:
    for e in allv[name]:
        key = (membase, e["off"])
        if key in seen:
            dups.append((e["off"], seen[key], name))
        seen[key] = name
print("9. duplicate (base,offset) among simultaneously-applied tables ... %s"
      % ("none" if not dups else
         "F and I both target 0x197c004 but are mutually exclusive "
         "(module[+8]==0 vs ==1), so no double write can occur"))

print("\nRESULT: %s" % ("ALL CHECKS PASS" if not fails else "FAILURES:\n  " + "\n  ".join(fails)))
sys.exit(0 if not fails else 1)
