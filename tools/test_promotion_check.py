#!/usr/bin/env python3
"""tools/promotion_check.py on synthetic rows: each criterion of the README
"Promotion" rule can fail a row, the chip verdict counts only passing rows, and
an on-by-default chip reverts when its latest row is not clean."""
import copy
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import promotion_check as pc  # noqa: E402


def row(n, soc='t6020', board='j414s', kernel='7.1.13-3-1-ARCH', machine=None, owner=None, at=None):
    return {
        'content_sha256': f'{n:02x}' + '0' * 62, 'chip': f'apple,{soc}', 'kernel': kernel,
        'received_at': at or f'2026-10-0{n % 9 + 1}T00:00:00.000Z',
        'summary': {'ane_port_detail': {
            'devicetree': {'boot': {'compatible': [f'apple,{board}', f'apple,{soc}']}},
            'runtime': {'omarchy_ane': {
                'machine_id': machine or f'm{n}', 'owner_id': owner or f'o{n}',
                'check': {'exit': 0, 'status': 'ready'},
                'module': {'name': pc.DRIVER[soc]},
                'smoke': {'sha256': [pc.GOLDEN['t6020']] * 20, 'errors': 0},
                'uptime_s': 1800,
                'dmesg': ['[    3.1] ane_t6021 284000000.ane: fwload: own memory'],
                'dmesg_faults': [],
            }}}}}


def oa(r):
    return r['summary']['ane_port_detail']['runtime']['omarchy_ane']


good = [row(1, board='j414s', owner='a'), row(2, board='j416s', owner='a'),
        row(3, board='j414s', kernel='7.1.13-3-2-ARCH', owner='b')]

# 1. Three passing rows: three machines, two owners, two boards, two kernels -> promote.
assert all(pc.failures(r) == [] for r in good)
v = pc.verdict(good)['t6020']
assert v['promote'] and v['needs'] == [], v

# 2. Each per-row criterion fails the row on its own, at its boundary.
def broken(change):
    r = copy.deepcopy(good[0])
    change(oa(r))
    return pc.failures(r)

assert broken(lambda o: o['check'].update(exit=1, status='FAILED'))
assert broken(lambda o: o['module'].update(name='ane'))
assert broken(lambda o: o['smoke'].update(sha256=[pc.GOLDEN['t6020']] * 19))
assert broken(lambda o: o['smoke']['sha256'].__setitem__(7, '0' * 64))
assert broken(lambda o: o['smoke'].update(errors=1))
assert broken(lambda o: o.update(uptime_s=1799))
assert broken(lambda o: o['dmesg'].append('[ 900.2] apple-dart 285800000.iommu: DART fault: STT_FAULT'))
assert broken(lambda o: o.update(dmesg_faults=['[ 9.0] ane_t6021: timeout']))
# A fault word on a line that is not about the ANE does not count.
assert not broken(lambda o: o['dmesg'].append('[ 2.0] apple-dart 3860e8000.iommu: DART fault'))

# 3. Counting: each diversity count is short on its own.
assert pc.verdict(good[:2])['t6020']['needs'] == ['passing rows 2/3', 'machines 2/3', 'owners 1/2',
                                                  'kernel releases 1/2']
same = [row(i, machine='m', board='j414s' if i % 2 else 'j416s', kernel=f'k{i % 2}') for i in (1, 2, 3)]
assert pc.verdict(same)['t6020']['needs'] == ['machines 1/3']
one_owner = [row(i, owner='a', board='j414s' if i % 2 else 'j416s', kernel=f'k{i % 2}') for i in (1, 2, 3)]
assert pc.verdict(one_owner)['t6020']['needs'] == ['owners 1/2']
one_board = [row(i, board='j414s', kernel=f'k{i % 2}') for i in (1, 2, 3)]
assert pc.verdict(one_board)['t6020']['needs'] == ['boards 1/2']

# 4. A failing row blocks a chip that otherwise has enough passing rows.
bad = row(4)
oa(bad)['smoke']['sha256'][0] = '0' * 64
v = pc.verdict(good + [bad])['t6020']
assert not v['promote'] and v['needs'] == ['1 failing row(s) to explain'], v

# 5. SoC tables: T6002 has one board; the M1 family has no smoke fixture, so
# T6000 and T6002 cannot pass; T8112 runs the same add fixture as T6020.
t6002 = [row(i, soc='t6002', board='j375d', kernel=f'k{i % 2}') for i in (1, 2, 3)]
for r in t6002:
    oa(r)['smoke']['sha256'] = []
assert pc.verdict(t6002)['t6002']['needs'] == ['passing rows 0/3', 'machines 0/3', 'owners 0/2',
                                               'boards 0/1', 'kernel releases 0/2', '3 failing row(s) to explain']
assert 'no smoke golden for t6002' in pc.failures(t6002[0])
assert 'no smoke golden for t6000' in pc.failures(row(8, soc='t6000', board='j314s'))
assert pc.failures(row(9, soc='t8112', board='j413')) == []

# 6. Rows without the omarchy-ane block are not judged.
legacy = copy.deepcopy(good[0])
del legacy['summary']['ane_port_detail']['runtime']['omarchy_ane']
assert pc.verdict([legacy]) == {}


# 7. Regression: an on-by-default chip (T6021) reverts when its latest row
# shows check not ready or a fault line, and stays on once a clean row lands.
# An opt-in chip never reverts.
assert pc.ON == {'t8103', 't6001', 't6021'}, pc.ON
# Listed newest first, so "latest" must come from received_at, not list order.
on = [row(i, soc='t6021', board='j414c', at=f'2026-10-0{4 - i}T00:00:00.000Z') for i in (1, 2, 3)]
new, older = oa(on[0]), oa(on[1])
v = pc.verdict(on)['t6021']
assert v['on'] and v['latest'] == '010000000000' and v['revert'] == [], v
new['check'].update(exit=1, status='FAILED')
assert pc.verdict(on)['t6021']['revert'] == ['omarchy-ane-check not ready (exit 1, FAILED)']
new['check'].update(exit=0, status='ready')
new['dmesg_faults'] = ['[ 9.0] ane_t6021: call completion wait failed -110']
assert pc.verdict(on)['t6021']['revert'][0].startswith('1 ANE/DART/mailbox fault line(s)')
older['dmesg_faults'], new['dmesg_faults'] = new['dmesg_faults'], []
assert pc.verdict(on)['t6021']['revert'] == [], 'a clean latest row lands after a bad one'
late = row(4, at='2026-10-09T00:00:00.000Z')
oa(late)['check'].update(exit=1, status='FAILED')
v = pc.verdict(good + [late])['t6020']
assert not v['on'] and v['revert'] == [], v
print('test_promotion_check: ok')
