#!/usr/bin/env python3
"""docs/ane-constants-t6021-t8152.md against the source it describes.

Every T6021 value in the table is re-read from ane/t6021 (a #define, or a field of ane_t6021_soc); every
[NAME] reference in the T8152 or H15 column must be a #define of that value in ane/h18 or ane/h15; every numeric
#define of ane/t6021 is a row or an entry of EXCLUDED; the channel table rings follow offset + slots * 0x40; the
RVBAR value iBoot leaves on the M6 survives the T6021 entry fold. Offline, no build.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DOC = ROOT / "docs/ane-constants-t6021-t8152.md"
DEF = re.compile(r"^#define\s+(\w+)\s+\(?(BIT\(\d+\)|0x[0-9a-fA-F]+|\d+)[uUlL]*\)?(?=\s|\)|/|$)", re.MULTILINE)
# Not chip constants (see the doc's last paragraph) or layouts of a transport T8152 does not have.
EXCLUDED = re.compile(
    r"ANE_LEGACY_|ANE_T6021_BO_|ANE_TRACE_|ANE_PMGR_PS_LAST_OFF|ANE_CALL_COOKIE|ANE_T2H_CALL_|ANE_MBI_CHAN_MAX_DUMP"
    r"|MH_MAGIC_64|MH_PRELOAD|CPU_TYPE_ARM64|LC_SEGMENT_64|LC_UNIXTHREAD|ARM_THREAD_STATE64"
    r"|ANE_ASC_MBOX_CTRL_|ANE_PS_(BUSY|AUTO_ENABLE)|ANE_SEC_F_PRESENT|ANE_T6021_PROG_|ANE_T6021_BOOT_TABLE_POLLS")


def num(tok):
    tok = tok.strip()
    m = re.fullmatch(r"BIT\((\d+)\)", tok)
    if m:
        return 1 << int(m.group(1))
    m = re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)[uUlL]*", tok)
    return int(m.group(1), 0) if m else None


def defines(paths):
    out = {}
    for p in paths:
        for name, val in DEF.findall(p.read_text()):
            out[name] = num(val)
    return out


def soc_fields(text):
    body = re.search(r"const struct ane_t602x_soc ane_t6021_soc = \{(.*?)\n\};", text, re.DOTALL).group(1)
    return {k: int(v, 16) for k, v in re.findall(r"\.(\w+)\s*=\s*(0x[0-9a-fA-F]+)", body)}


def load():
    t = sorted((ROOT / "ane/t6021").glob("*.[ch]"))
    return (defines(t), soc_fields((ROOT / "ane/t6021/ane_t6021_fwload.c").read_text()),
            defines(sorted((ROOT / "ane/h18").glob("*.[ch]"))),
            defines(sorted((ROOT / "ane/h15").glob("*.[ch]"))))


def first(cell):
    m = re.search(r"`([^`]+)`", cell)
    return m.group(1) if m else None


def check_rows(text, t6021, soc, h18, h15):
    errs, seen = [], set()
    for line in text.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        m = re.fullmatch(r"`([\w.]+)`", cells[0]) if len(cells) == 5 else None
        if not m:
            continue
        name = m.group(1)
        seen.add(name)
        want = soc.get(name.split(".")[1]) if name.startswith("ane_t6021_soc.") else t6021.get(name)
        got = num(first(cells[1]) or "")
        if want is None or got != want:
            errs.append(f"{name}: T6021 cell {first(cells[1])} but source {want}")
        for col, defs in ((2, h18), (3, h15)):
            for ref in re.findall(r"\[(ANE_\w+)\]", cells[col]):
                if defs.get(ref) is None or num(first(cells[col]) or "") != defs[ref]:
                    errs.append(f"{name}: {ref} is {defs.get(ref)} but cell says {first(cells[col])}")
    for name in t6021:
        if name not in seen and not EXCLUDED.match(name) and t6021[name] is not None:
            errs.append(f"{name}: numeric #define in ane/t6021 with no row")
    return errs


def check_channels(text, boot_h, t6021):
    errs = []
    layout = [(n, int(t), int(b), int(s, 16), int(o, 16)) for n, t, b, s, o in re.findall(
        r'\{ "(\w+)",\s*(\d),\s*(\d),\s*(0x[0-9a-f]+),\s*(0x[0-9a-f]+) \}', boot_h)]
    rows = []
    for line in text.splitlines():
        c = [x.strip().strip("`") for x in line.strip().strip("|").split("|")]
        if len(c) == 7 and c[0].isupper() and c[3].startswith("0x"):
            rows.append((c[0], int(c[1]), int(c[2]), int(c[3], 16), int(c[4], 16), int(c[5], 16), int(c[6], 16)))
    if len(rows) != 8 or [r[:3] for r in rows] != [x[:3] for x in layout]:
        return errs + [f"channel table has {len(rows)} rows or names/types/bits differ from ane_t6021_chman_layout"]
    for (n, _, _, s6, o6, s8, o8), src in zip(rows, layout):
        if (s6, o6) != src[3:]:
            errs.append(f"{n}: T6021 columns {s6:#x}/{o6:#x} but source {src[3]:#x}/{src[4]:#x}")
    for col, tot in ((3, t6021["ANE_T6021_CHMAN_TOTAL"]), (5, None)):
        off = 0x800
        for r in rows:
            if r[col + 1] != off:
                errs.append(f"{r[0]}: ring {r[col + 1]:#x} but packing gives {off:#x}")
            off += r[col] * 0x40
        if tot is not None and off + 0x40 != tot:
            errs.append(f"T6021 total {tot:#x} but last ring end + 0x40 is {off + 0x40:#x}")
        if tot is None and not re.search(rf"^\| `ANE_T6021_CHMAN_TOTAL` \|[^|]*\| `{off + 0x40:#x}` \|", text, re.MULTILINE):
            errs.append(f"CHMAN_TOTAL T8152 cell is not {off + 0x40:#x}")
    return errs


def check_rvbar(t6021):
    mask = t6021["ANE_T6021_RVBAR_ADDR_MASK"]
    latch = 0x10000000001  # ADT segment-ranges remap IOVA 0x10000000000 | valid bit; the M6 reads this back
    entry = latch & mask
    errs = []
    if entry != 0x10000000000 or (entry & ~mask & (2**64 - 1)) != 0:
        errs.append(f"boot-loader RVBAR {latch:#x} folds to {entry:#x}")
    return errs


def main():
    t6021, soc, h18, h15 = load()
    text = DOC.read_text()
    boot_h = (ROOT / "ane/t6021/ane_t6021_boot.h").read_text()
    errs = check_rows(text, t6021, soc, h18, h15) + check_channels(text, boot_h, t6021) + check_rvbar(t6021)
    # the checks must fail when the table or the code drifts
    bad = text.replace("| `ANE_MBI_DOORBELL` | `0x1844000` |", "| `ANE_MBI_DOORBELL` | `0x1844004` |", 1)
    assert check_rows(bad, t6021, soc, h18, h15), "a wrong T6021 cell passed"
    bad = text.replace("`0x18a4000` [ANE_H18_DOORBELL_OFF]", "`0x18a4004` [ANE_H18_DOORBELL_OFF]", 1)
    assert check_rows(bad, t6021, soc, h18, h15), "a wrong T8152 reference passed"
    bad = re.sub(r"^\| `ANE_FW_NSEGS`.*\n", "", text, flags=re.MULTILINE)
    assert check_rows(bad, t6021, soc, h18, h15), "a missing row passed"
    assert check_channels(text.replace("`0x8800`", "`0x8801`", 1), boot_h, t6021), "a wrong ring passed"
    for e in errs:
        print("FAIL", e)
    print(f"{len(errs)} failure(s)")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
