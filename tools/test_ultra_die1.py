#!/usr/bin/env python3
"""Boot-free checks for the Ultra die-1 plumbing (docs/ultra-die1.md M1).

The die-keyed data is parsed out of the driver sources and modelled the way
the kernel resolves it: ane.ko takes the SET base from the node's "set" reg
when the overlay names one and falls back to the per-compatible descriptor,
then looks (compatible, SET base) up in ane_qual_table; ane_t6021 keys its
T6022 per-die data (pmu_pa, firmware pin) on the same SET reg. The overlays
must name windows whose translated addresses are exactly the table's bases,
die 0 results unchanged: every die-0 (compatible, base) pair keeps its tier,
and no tier moves without an allow_unqualified bind.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))

drv = (root / 'ane/src/ane_drv.c').read_text()
fwload = (root / 'ane/t6021/ane_t6021_fwload.c').read_text()
thead = (root / 'ane/t6021/ane_t6021.h').read_text()
runner = (root / 'tools/ane-run.c').read_text()
smoke = (root / 'packaging/omarchy-ane-smoke').read_text()


def cells(text):
    out = {}
    for m in re.finditer(r'\.(\w+)\s*=\s*(0x[0-9a-fA-F]+|\d+|true|false)(?:ull|ULL|UL|ul|U|u)?[,\n]', text):
        v = m.group(2)
        if v == 'true':
            out[m.group(1)] = 1
        elif v == 'false':
            out[m.group(1)] = 0
        else:
            out[m.group(1)] = int(v, 0)
    return out


# 1. ane.ko: the die-keyed qualification table and the descriptor fallbacks.
rows = [(m.group(1), int(m.group(2), 0), m.group(3)) for m in re.finditer(
    r'\{ "(apple,t[0-9a-z]+-ane)", (0x[0-9a-fA-F]+)ULL, (ANE_[A-Z]+) \}', drv)]
assert len(rows) == 4, rows
QUAL = {'ANE_QUALIFIED', 'ANE_RECOGNIZED', 'ANE_UNSUPPORTED'}
assert {q for _, _, q in rows} <= QUAL
die0 = {(c, b): q for c, b, q in rows if b < 0x2000000000}
die1 = {(c, b): q for c, b, q in rows if b >= 0x2000000000}
# Known die-0 bases keep QUALIFIED; the T6021 die-0 row stays RECOGNIZED
# exactly as the pre-M1 descriptor tier had it.
assert die0[('apple,t8103-ane', 0x23b70c000)] == 'ANE_QUALIFIED'
assert die0[('apple,t6000-ane', 0x28e08c000)] == 'ANE_QUALIFIED'
assert die0[('apple,t6021-ane', 0x28e08c000)] == 'ANE_RECOGNIZED'
# T6002 die 1 is the +0x20_0000_0000 translation (receipts/2026-10-03-ultra-die1),
# RECOGNIZED only: binds solely under ane.allow_unqualified=1 until a die-1 run.
assert die1 == {('apple,t6000-ane', 0x228e08c000): 'ANE_RECOGNIZED'}, die1
# Descriptors: the die-0 fallback bases match the die-0 table rows; t6020
# keeps no base (refuses as UNSUPPORTED before the table matters).
desc = {name: cells(re.search(rf'static const struct ane_soc {name} = \{{(.*?)\n\}};',
                              drv, re.S).group(1))
        for name in ('ane_soc_t8103', 'ane_soc_t6000', 'ane_soc_t6020', 'ane_soc_t6021')}
assert desc['ane_soc_t8103']['ps_base'] == 0x23b70c000
assert desc['ane_soc_t6000']['ps_base'] == 0x28e08c000
assert desc['ane_soc_t6021']['ps_base'] == 0x28e08c000
assert desc['ane_soc_t6020']['ps_base'] == 0
assert desc['ane_soc_t6000']['tm_retention'] == 1, 'T6002 die 1 keeps the die-0 retention drain'


def qual_for(compatible, ps_base, soc=None):
    """ane_qual_for() as the probe resolves it: node "set" reg absent ->
    descriptor base; then the table, else ANE_UNSUPPORTED."""
    if soc is None:
        soc = desc[compatible.replace('apple,t', 'ane_soc_t').replace('-ane', '')]
    base = ps_base if ps_base is not None else soc['ps_base']
    return next((q for c, b, q in rows if c == compatible and b == base),
                'ANE_UNSUPPORTED')


# Model: every die-0 probe result is unchanged. Trees without a "set" reg
# (the t8103 overlay, every pre-M1 T600x tree) take the descriptor base.
assert qual_for('apple,t8103-ane', None) == 'ANE_QUALIFIED'
assert qual_for('apple,t6000-ane', None) == 'ANE_QUALIFIED'
assert qual_for('apple,t6000-ane', 0x28e08c000) == 'ANE_QUALIFIED'
assert qual_for('apple,t6021-ane', 0x28e08c000) == 'ANE_RECOGNIZED'
# T6002 die-1 node: RECOGNIZED (the gate demands allow_unqualified=1).
assert qual_for('apple,t6000-ane', 0x228e08c000) == 'ANE_RECOGNIZED'
# A guessed base cannot bind silently, whatever the compatible.
assert qual_for('apple,t6000-ane', 0x28e08d000) == 'ANE_UNSUPPORTED'
assert qual_for('apple,t8103-ane', 0x23b70d000) == 'ANE_UNSUPPORTED'
# t6020 still refuses with no node window.
assert qual_for('apple,t6020-ane', None) == 'ANE_UNSUPPORTED'
# The gate reads allow_unqualified only for the RECOGNIZED tier.
gate = ('if (qual == ANE_RECOGNIZED && !allow_unqualified)', 'ane_platform_probe')
assert all(g in drv for g in gate)

# 2. ane_t6021: the T6022 per-die data, keyed by the same SET reg.
die1_soc = re.search(r'const struct ane_t602x_soc ane_t6022_soc_die1 = \{(.*?)\n\};',
                     fwload, re.S).group(1)
d1 = cells(die1_soc)
assert d1['pmu_pa'] == 0x228e084000, d1
assert d1['pmu_pa'] == 0x228e08c000 - 0x8000, 'pmu_pa is the SET base - 0x8000'
assert d1['soc'] == 0x6022 and d1['soc_revision'] == 0x11
assert '"t602x_ane1_fw_selene_rc4x"' in die1_soc, 'the die-1 firmware pin is a data field'
assert 'fw_pin' not in re.search(r'const struct ane_t602x_soc ane_t6022_soc = \{(.*?)\n\};',
                                 fwload, re.S).group(1), 'die 0 carries no pin: no behavior change'
for name in ('ane_t6020_soc', 'ane_t6021_soc', 'ane_t6022_soc'):
    row = cells(re.search(rf'const struct ane_t602x_soc {name} = \{{(.*?)\n\}};', fwload, re.S).group(1))
    assert row['pmu_pa'] == 0x28e084000 == 0x28e08c000 - 0x8000, (name, row)
t8112 = cells(re.search(r'const struct ane_t602x_soc ane_t8112_soc = \{(.*?)\n\};',
                        fwload, re.S).group(1))
assert t8112['pmu_pa'] == 0x23b70c000 and t8112['ps_off'] == 0x8, 'T8112 keeps its own derivation'
# The die table and the selector: SET reg keys the die, non-T6022 and trees
# without the window keep the compatible row.
dies = re.findall(r'\{ (0x[0-9a-fA-F]+)ull, &(\w+) \}', fwload)
assert dies == [('0x28e08c000', 'ane_t6022_soc'), ('0x228e08c000', 'ane_t6022_soc_die1')], dies
assert 'soc != &ane_t6022_soc' in fwload and 'platform_get_resource_byname' in fwload
assert 'ane_t6021_soc_for' in thead
for src, want in (('ane/t6021/ane_t6021_fwload.c', 2),
                  ('ane/t6021/ane_t6021_rtclient_main.c', 0)):
    text = (root / src).read_text()
    uses = len(re.findall(r'of_device_get_match_data\(', text))
    assert uses == want, (src, uses)  # fwload: inside ane_t6021_soc_for + the
    # placement predicate, whose per-die data (preload_placement) is die-0 only

# 3. The overlays name windows whose reg cells carry exactly the table bases
# (bus-relative: die 1 rides the die-1 bus ranges; Linux translates before
# the driver sees the resource).
sys.path.insert(0, str(root))
from importlib.machinery import SourceFileLoader  # noqa: E402
from importlib.util import module_from_spec, spec_from_loader  # noqa: E402

with tempfile.TemporaryDirectory() as tmp:
    spec = spec_from_loader('oadt', SourceFileLoader('oadt', str(root / 'packaging/omarchy-ane-dt')))
    oadt = module_from_spec(spec)
    spec.loader.exec_module(oadt)

    def node_regs(source, nodename):
        dtbo = Path(tmp) / f'{source}.dtbo'
        subprocess.run(['dtc', '-q', '-@', '-I', 'dts', '-O', 'dtb', '-o', str(dtbo),
                        str(root / 'packaging/dt' / source)], check=True)
        t = oadt.Tree(dtbo.read_bytes())
        paths = [p for p in t.nodes
                 if p.endswith('/' + nodename) and '__local_fixups__' not in p]
        assert len(paths) == 1, (source, nodename, paths)
        r = oadt.cells(t.nodes[paths[0]]['reg'])
        names = t.strings(paths[0], 'reg-names')
        return {n: tuple(r[4 * i:4 * i + 2]) for i, n in enumerate(names)}

    # The die-0 windows live in the dtsi includes, compiled through their
    # board overlays: one T600x board, one T602x board, both die-1 overlays.
    assert node_regs('t6002-ane.dts', 'ane@284000000')['set'] == (0x2, 0x8e08c000)
    assert node_regs('t6021-ane.dts', 'ane@284000000')['set'] == (0x2, 0x8e08c000)
    assert node_regs('t6002-ane-die1.dts', 'ane@284000000')['set'] == (0x2, 0x8e08c000)
    assert node_regs('t6022-ane-die1.dts', 'ane@284000000')['set'] == (0x2, 0x8e08c000)
    # The die-1 engines sit on the die-1 bus (target /soc@2200000000 in the
    # overlay's fragment), die 0 through the plain /soc target-path.
    t = oadt.Tree((Path(tmp) / 't6002-ane-die1.dts.dtbo').read_bytes())
    assert len([p for p in t.nodes if p.endswith('/ane@284000000')
                and '__local_fixups__' not in p]) == 1

# 4. Tools: --dev / ANE_DEVICE reach libane's dev_id; smoke carries `die`.
assert '"--dev"' in runner and 'ANE_DEVICE' in runner
assert '__ane_init(anec, dev)' in runner
assert 'ane_m2_init_ports(anec, pr.ports, pr.count, dev)' in runner
assert re.search(r'struct ane_nn \*ane_m2_init_ports\(const char \*path,\s*'
                 r'const struct ane_m2_port_spec \*ports,\s*uint32_t port_count, int dev_id\)',
                 (root / 'libane/ane_m2.h').read_text())
assert 'DIE_STRIDE = 0x20_0000_0000' in smoke
assert smoke.count('"die": bound') == 2, smoke.count('"die": bound')

# 5. Two-device plumbing: the stats debugfs name is die-keyed and
# deterministic from the node — die 0 keeps the legacy name every die-0
# consumer reads (/sys/kernel/debug/ane, ane_t6021 for the M2 family);
# die >= 1 names by dev_name, so a lone die-1 node is suffixed too.
assert 'dev_name(ane->dev) : "ane"' in drv and 'eng->start >> 37' in drv
assert re.search(r'dev_name\(dev\)\s*:\s*"ane_t6021"',
                 (root / 'ane/t6021/ane_t6021_rtclient_main.c').read_text())
rtmain = (root / 'ane/t6021/ane_t6021_rtclient_main.c').read_text()
# The trace_td blob owns its own debugfs dir (37ffb57); the probe's die-0
# stats dir keeps the legacy "ane_t6021" name through the die-keyed expression.
assert 'debugfs_create_dir("ane_t6021_trace", NULL)' in rtmain
assert '(eng && (eng->start >> 37)) ?' in rtmain and '"ane_t6021",' in rtmain
assert 'debugfs_create_dir("ane", NULL)' not in drv
# The die-1 firmware pin reaches request_firmware, validated against the
# die-0 pin: a non-identical ane1 image refuses at load.
assert 'const char *fw_name = soc->fw_pin ? soc->fw_pin : img->name;' in fwload
assert 'request_firmware(&fw, fw_name, ane->dev)' in fwload
fetch_tool = (root / 'packaging/omarchy-ane-firmware-fetch').read_text()
assert 'no pin for T6022 die 1' in fetch_tool
assert 'soc@2200000000/ane@284000000' in fetch_tool

# 5. promotion_check: per-die keys exist and the die field is read from the
# collector block; deep details live in tools/test_promotion_check.py.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import promotion_check as pc  # noqa: E402

assert pc.die_of({'chip': 'apple,t6001'}) == 0
assert pc.die_key({'chip': 'apple,t6001', 'summary': {'ane_port_detail': {'runtime': {
    'omarchy_ane': {'die': 1}}}}}) == ('t6001', 1)
assert all(d == 0 for _, d in pc.ON)
assert ('t6002', 0) not in pc.ON, 'the T6002 die-0 overlay is opt-in'
assert pc.ANE_LINE.search('apple-dart 2285800000.iommu: translation fault')
assert pc.ANE_LINE.search('apple-mailbox 2285408000.mailbox: fifo error')
assert not pc.ANE_LINE.search('apple-dart 581008000.iommu: DART fault')
print('test_ultra_die1: ok')
