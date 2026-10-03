#!/usr/bin/env python3
"""Offline checks for tools/aurora_dt.py on a fake aurora-silicon/linux tree:
the APPLE_ANE_UNTESTED switch (the T6000/T6001 shape), appended status blocks
(the T8112 shape, with a disabled parent power domain and an unlabeled
mailbox), nothing to do on an enabled chip, and the refusals: no ANE node, a
chip file that another file includes, a switch that leaves a node disabled.
Needs gcc and dtc; no network."""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import aurora_dt  # noqa: E402

SOC = '''/ {
  compatible = "apple,%(soc)s";
  #address-cells = <2>; #size-cells = <2>;
  soc {
    #address-cells = <2>; #size-cells = <2>; ranges;
    aic: interrupt-controller@100 { interrupt-controller; #interrupt-cells = <3>; };
    ps_ane_sys: power-controller@200 { #power-domain-cells = <0>; };
    ps_ane_cpu: power-controller@204 { #power-domain-cells = <0>; power-domains = <&ps_ane_sys>; status = "%(cpu)s"; };
    ps_ane_base: power-controller@208 { #power-domain-cells = <0>; power-domains = <&ps_ane_cpu>; status = "disabled"; };
    ane_dart0: iommu@300 { #iommu-cells = <1>; power-domains = <&ps_ane_sys>; status = "disabled"; };
    mailbox@600 { #mbox-cells = <0>; status = "disabled"; };
    ane: ane@400 { compatible = "apple,%(soc)s-ane"; interrupt-parent = <&aic>; iommus = <&ane_dart0 0>;
                   power-domains = <&ps_ane_base>; mboxes = <&{/soc/mailbox@600}>; status = "disabled"; };
    unrelated: thing@500 { status = "disabled"; };
  };
};
'''
SWITCH_BLOCK = '''
#ifndef APPLE_ANE_UNTESTED
&ane { status = "okay"; };
&ane_dart0 { status = "okay"; };
&ps_ane_base { status = "okay"; };
&{/soc/mailbox@600} { status = "okay"; };
#endif
'''
T6000 = '''/* This chip is a cut down t6001. */

/* The ANE has only run on the M1 Max so far: leave it off on the M1 Pro */
#define APPLE_ANE_UNTESTED

#include "t6001.dtsi"

/ { compatible = "apple,t6000"; };
'''


def board(soc):
    return f'/dts-v1/;\n#include "{soc}.dtsi"\n/ {{ model = "{soc} board"; }};\n'


def fake_tree(files):
    root = Path(tempfile.mkdtemp())
    dts = root / aurora_dt.DTS
    dts.mkdir(parents=True)
    (root / aurora_dt.INC).mkdir(parents=True)
    for name, text in files.items():
        (dts / name).write_text(text)
    return root


def refused(root, chip, why):
    try:
        aurora_dt.plan(root, chip)
    except aurora_dt.Refuse as e:
        assert why in str(e), (why, str(e))
        return
    raise AssertionError(f'{chip}: no refusal ({why})')


base = {'t6001.dtsi': SOC % {'soc': 't6000', 'cpu': 'okay'} + SWITCH_BLOCK, 't6001-j316c.dts': board('t6001'),
        't6000.dtsi': T6000, 't6000-j314s.dts': board('t6000'), 't6000-j316s.dts': board('t6000'),
        't8112.dtsi': SOC % {'soc': 't8112', 'cpu': 'disabled'}, 't8112-j413.dts': board('t8112'),
        't6022.dtsi': '/ { compatible = "apple,t6022"; };\n', 't6022-j180d.dts': board('t6022')}
root = fake_tree(base)
dts = root / aurora_dt.DTS

# The switch: the define and its comment go, nothing else; both boards checked.
p = aurora_dt.plan(root, 't6000')
assert p['how'] == 'switch' and p['file'] == f'{aurora_dt.DTS}/t6000.dtsi', p
assert p['new'] == T6000.replace('/* The ANE has only run on the M1 Max so far: leave it off on the M1 Pro */\n'
                                 '#define APPLE_ANE_UNTESTED\n\n', ''), p['new']
assert p['boards'] == ['t6000-j314s.dts', 't6000-j316s.dts'], p['boards']
assert p['enabled'] == ['/soc/mailbox@600', 'ane', 'ane_dart0', 'ps_ane_base'], p['enabled']
assert '-#define APPLE_ANE_UNTESTED' in aurora_dt.diff(p)
# The included file is enabled: nothing to do there, whoever includes it.
assert aurora_dt.plan(root, 't6001') is None

# The blocks: ANE first, then by label; the unlabeled mailbox by path; the
# disabled parent domain too; the unrelated disabled node untouched.
p = aurora_dt.plan(root, 't8112')
assert p['how'] == 'blocks', p
added = p['new'][len(base['t8112.dtsi'].rstrip('\n')):]
refs = [l.split(' {')[0] for l in added.splitlines() if l.endswith(' {')]
assert refs == ['&ane', '&{/soc/mailbox@600}', '&ane_dart0', '&ps_ane_base', '&ps_ane_cpu'], refs
assert added.startswith('\n\n/* The ANE has run on the M2: enable it and the nodes it uses. */\n&ane {\n'
                        '\tstatus = "okay";\n};\n'), added
assert 'unrelated' not in added and 'ps_ane_sys {' not in added

# --check writes nothing; --apply writes; a second plan has nothing to do.
for chip in ('t6000', 't8112'):
    before = (dts / f'{chip}.dtsi').read_text()
    q = subprocess.run([sys.executable, aurora_dt.__file__, '--tree', str(root), '--chip', chip, '--check'],
                       capture_output=True, text=True)
    assert q.returncode == 0 and q.stdout.startswith(f'--- a/{aurora_dt.DTS}/{chip}.dtsi'), q
    assert (dts / f'{chip}.dtsi').read_text() == before
    q = subprocess.run([sys.executable, aurora_dt.__file__, '--tree', str(root), '--chip', chip, '--apply'],
                       capture_output=True, text=True)
    assert q.returncode == 0 and '(written)' in q.stdout, q
    assert aurora_dt.plan(root, chip) is None, chip
    q = subprocess.run([sys.executable, aurora_dt.__file__, '--tree', str(root), '--chip', chip, '--check'],
                       capture_output=True, text=True)
    assert q.returncode == 0 and 'nothing to do' in q.stdout, q

# Refusals; the tree stays as it was.
root = fake_tree(base)
refused(root, 't6022', 't6022-j180d.dts has no apple,*-ane node')
refused(fake_tree({**base, 't8112-variant.dtsi': '#include "t8112.dtsi"\n'}), 't8112',
        't8112-variant.dtsi include t8112.dtsi')
short = fake_tree({**base, 't6001.dtsi': base['t6001.dtsi'].replace('&ps_ane_base { status = "okay"; };\n', '')})
refused(short, 't6000', 'still disabled after the switch change: /soc/power-controller@208')
q = subprocess.run([sys.executable, aurora_dt.__file__, '--tree', str(short), '--chip', 't6000', '--apply'],
                   capture_output=True, text=True)
assert q.returncode == 1 and (short / aurora_dt.DTS / 't6000.dtsi').read_text() == T6000, q
print('test_aurora_dt: ok')
