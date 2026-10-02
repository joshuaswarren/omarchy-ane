#!/usr/bin/env python3
"""Promotion rule, fault scoping, and real community-row classification."""
import copy
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import promotion_check as pc  # noqa: E402


# The opt-in H14 specimen: derived from the on set, so a promotion flip
# (tools/promote_chip.py) keeps this suite green.
OPT = min((s for s, d in pc.DRIVER.items() if d == 'ane_t6021' and s not in pc.ON), default=None)
assert OPT, 'the scenarios below need an opt-in H14 chip'


def row(n, soc=OPT, at=None):
    return {
        'content_sha256': f'{n:02x}' + '0' * 62, 'chip': f'apple,{soc}',
        'received_at': at or f'2026-10-0{n % 9 + 1}T00:00:00.000Z',
        'summary': {'ane_port_detail': {
            'runtime': {'omarchy_ane': {
                'check': {'available': True, 'exit': 0, 'status': 'ready'},
                'module': {'name': pc.DRIVER[soc]},
                'smoke': {'requested': True, 'available': True,
                          'sha256': [pc.GOLDEN[soc]] * 20, 'errors': 0},
                'dmesg': [], 'dmesg_faults': [],
            }}}}}


def oa(r):
    return r['summary']['ane_port_detail']['runtime']['omarchy_ane']


# The checker reads the same per-chip golden mapping that the smoke runner uses.
assert pc.GOLDEN is pc.SMOKE.GOLDEN

# One passing row is enough to promote an untested H14 or H13 chip.
good = row(1)
v = pc.verdict([good])[OPT]
assert v['promote'] and not v['conflict'] and v['passing'] == ['010000000000'], v
assert pc.failures(good) == []
h13_good = row(7, soc='t6000')
assert pc.failures(h13_good) == []
assert pc.verdict([h13_good])['t6000']['promote']

# Each part of the passing-row definition is enforced.
def broken(change):
    r = copy.deepcopy(good)
    change(oa(r))
    return pc.failures(r)

assert broken(lambda o: o['check'].update(exit=1, status='FAILED'))
assert broken(lambda o: o['module'].update(name='ane'))
assert broken(lambda o: o['smoke'].update(sha256=[pc.GOLDEN[OPT]] * 19))
assert broken(lambda o: o['smoke'].update(sha256=[pc.GOLDEN[OPT]] * 21))
assert broken(lambda o: o['smoke']['sha256'].__setitem__(7, '0' * 64))
assert broken(lambda o: o['smoke'].update(errors=1))
assert broken(lambda o: o['dmesg'].append('[ 900.2] apple-dart 285800000.iommu: DART fault: STT_FAULT'))
assert broken(lambda o: o['dmesg_faults'].append('[ 9.0] ane_t6021: timeout'))

# Faults from another device do not count, including apple-dcp RTKit syslog
# lines in either collector list and DARTs outside the ANE address set.
dcp = '[ 9.0] apple-dcp 38bc00000.dcp: RTKit: syslog message: FBPropertyManager.h:189'
assert not broken(lambda o: o['dmesg'].append(dcp))
assert not broken(lambda o: o['dmesg_faults'].append(dcp))
assert not broken(lambda o: o['dmesg'].append('[ 2.0] apple-dart 581008000.iommu: DART fault'))
assert not broken(lambda o: o['dmesg'].append('[ 2.0] systemd[1]: ane.service: failed'))

# A clean installed row with no smoke attempt is not judged. Busy/explicitly
# unattempted rows are also skipped, but not-ready or fault rows still fail.
clean_skip = row(3)
oa(clean_skip)['smoke'] = {'requested': False}
assert not pc.judged(clean_skip)
assert dict(pc.unattempted([clean_skip])) == {OPT: 1}
assert pc.verdict([clean_skip]) == {}
busy = row(4)
oa(busy)['smoke'] = {'requested': True, 'attempted': False, 'busy': True}
assert not pc.judged(busy)
no_smoke_fault = copy.deepcopy(clean_skip)
oa(no_smoke_fault)['dmesg'].append('[ 900.2] apple-dart 285800000.iommu: translation fault')
assert pc.judged(no_smoke_fault) and pc.failures(no_smoke_fault)
no_smoke_not_ready = copy.deepcopy(clean_skip)
oa(no_smoke_not_ready)['check'].update(exit=1, status='FAILED')
assert pc.judged(no_smoke_not_ready) and pc.failures(no_smoke_not_ready)
unavailable_attempt = row(5)
oa(unavailable_attempt)['smoke'] = {'requested': True, 'available': False, 'sha256': [], 'errors': 0}
assert pc.judged(unavailable_attempt) and pc.failures(unavailable_attempt)

# One passing and one failing row on an opt-in chip is a conflict, not promotion.
failed = row(2, at='2026-10-09T00:00:00.000Z')
oa(failed)['smoke']['sha256'][0] = '0' * 64
v = pc.verdict([good, failed])[OPT]
assert not v['promote'] and v['conflict'] and len(v['passing']) == len(v['failing']) == 1, v
v = pc.verdict([failed])[OPT]
assert not v['promote'] and not v['conflict'] and v['needs'] == ['one passing row'], v

# Rows without an ANE block or without installation are not judged.
legacy = copy.deepcopy(good)
del legacy['summary']['ane_port_detail']['runtime']['omarchy_ane']
assert pc.verdict([legacy]) == {}

# Default-on chips revert when their latest judged row is unclean; a later
# clean judged row clears the regression. The on set is the overlays table's
# enabled rows (derived, so a promotion flip keeps this suite green); the
# scenario below runs against whatever default-on chip is on the tree.
rows = [l.split() for l in (Path(__file__).resolve().parents[1] /
        'packaging/dt/overlays').read_text().splitlines() if l and l[0] != '#']
assert pc.ON == {p for p, src, state in rows
                 if src == f'{p}-ane.dts' and state == 'enabled'}, pc.ON
assert pc.ON, 'the scenario below needs a default-on chip'
spec = 't6021' if 't6021' in pc.ON else sorted(pc.ON)[0]
on = [row(i, soc=spec, at=f'2026-10-0{4 - i}T00:00:00.000Z') for i in (1, 2, 3)]
v = pc.verdict(on)[spec]
assert v['on'] and v['latest'] == '010000000000' and v['revert'] == [], v
oa(on[0])['check'].update(exit=1, status='FAILED')
assert pc.verdict(on)[spec]['revert'] == ['omarchy-ane-check not ready (exit 1, FAILED)']
oa(on[0])['check'].update(exit=0, status='ready')
oa(on[0])['dmesg_faults'] = ['[ 9.0] ane_t6021: call completion wait failed -110']
assert pc.verdict(on)[spec]['revert'][0].startswith('1 ANE/DART/mailbox fault line(s)')
oa(on[1])['dmesg_faults'] = list(oa(on[0])['dmesg_faults'])
oa(on[0])['dmesg_faults'] = []
assert pc.verdict(on)[spec]['revert'] == [], 'a clean latest judged row clears the revert'

# Four real rows from the live dataset reproduce both reported defects.
fixture_dir = Path(__file__).parent / 'fixtures' / 'promotion_check'
real = [json.loads(p.read_text()) for p in sorted(fixture_dir.glob('*.json'))]
real_by_id = {r['content_sha256'][:12]: r for r in real}
assert set(real_by_id) == {'66091f89cef6', 'b3d0b521403f', 'a994fe80c994', 'aec69f796113'}
t6001 = real_by_id['66091f89cef6']
assert pc.installed(oa(t6001)) and pc.unclean(oa(t6001)) == []
assert not pc.judged(t6001), 'clean T6001 row with requested:false smoke is not judged'
for sha in ('a994fe80c994', 'aec69f796113', 'b3d0b521403f'):
    assert not pc.installed(oa(real_by_id[sha]))
assert dict(pc.unattempted(real)) == {'t6000': 2, 't6001': 1, 't8112': 1}
assert pc.verdict(real) == {}, 'none of the four real rows is a promotion attempt'

# Synthetic positive controls prove each real ANE fault source still counts.
def injected(line, base, field='dmesg'):
    r = copy.deepcopy(base)
    oa(r)[field].append(line)
    return r

assert pc.failures(injected('[ 900.2] apple-dart 285800000.iommu: translation fault: status:0x81000404', good))
t8112_attempt = row(42, soc='t8112')
assert pc.failures(injected('[ 900.3] apple-dart 26b800000.iommu: translation fault: status:0x81000404', t8112_attempt))
assert pc.failures(injected('[ 901.1] ane 285c04000.ane: command timed out', t6001))
assert pc.failures(injected('[ 901.2] apple-mailbox 285408000.mailbox: fifo error', good))
assert not pc.unclean(oa(injected(dcp, good)))
assert not pc.unclean(oa(injected(dcp, t6001, 'dmesg_faults')))
assert not pc.unclean(oa(injected('[ 903.1] apple-dart 581008000.iommu: translation fault: status:0x81000404', good)))

# Not-installed rows stay unjudged even when the raw dmesg has unrelated faults.
not_installed = copy.deepcopy(real_by_id['a994fe80c994'])
assert dict(pc.unattempted([not_installed])) == {'t6000': 1}
assert pc.verdict([not_installed]) == {}
# The machine verdict: one chip object per verdict class.
jv = pc.json_verdict([good, h13_good])
optj = next(c for c in jv['chips'] if c['chip'] == OPT)
assert optj == {'chip': OPT, 'state': 'opt-in', 'verdict': 'PROMOTE',
                'rows': [{'row_sha': '010000000000', 'judged': True, 'passed': True,
                          'reasons': []}]}, optj
jchips = pc.json_verdict(on)['chips']
specj = next(c for c in jchips if c['chip'] == spec)
assert specj['verdict'] == 'ON' and specj['state'] == 'on', specj
oa(on[0])['check'].update(exit=1, status='FAILED')
specj = next(c for c in pc.json_verdict(on)['chips'] if c['chip'] == spec)
assert specj['verdict'] == 'REVERT' and specj['rows'][0]['passed'] is False, specj
conflict_row = copy.deepcopy(good)
oa(conflict_row)['smoke']['errors'] = 1
optj = next(c for c in pc.json_verdict([good, conflict_row])['chips'] if c['chip'] == OPT)
assert optj['verdict'] == 'CONFLICT', optj
unjudged = next(c for c in pc.json_verdict([legacy])['chips'] if c['chip'] == OPT)
assert unjudged['verdict'] == 'STAY' and unjudged['rows'] == [
    {'row_sha': '010000000000', 'judged': False, 'passed': False, 'reasons': []}], unjudged
t6000 = next(c for c in pc.json_verdict([not_installed])['chips'] if c['chip'] == 't6000')
# an uninstalled row is never judged: on an on-by-default chip the chip verdict
# is ON (nothing judged, nothing to revert), elsewhere STAY
assert t6000['verdict'] == ('ON' if 't6000' in pc.ON else 'STAY'), t6000
assert t6000['rows'] == [{'row_sha': 'a994fe80c994', 'judged': False,
                          'passed': False, 'reasons': []}], t6000
print('test_promotion_check: ok')
