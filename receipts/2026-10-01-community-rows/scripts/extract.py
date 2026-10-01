#!/usr/bin/env python3
"""Derived ANE facts per community row (values only, no raw row text)."""
import ast, json, glob, sys


def _one(v):
    return f"{v:#x}(die{v >> 12},{v & 0xfff})"


def irq(spec):
    """AIC numbers from IOInterruptSpecifiers in either stored shape."""
    if not isinstance(spec, str) or not spec:
        return None
    if spec.startswith("[b'") or spec.startswith('[b"'):
        specs = ast.literal_eval(spec)
        return ' '.join(_one(int.from_bytes(b[:4], 'little')) for b in specs)
    if '\ufffd' in spec:
        return 'lossy'
    return _one(int.from_bytes(spec.encode('latin-1')[:4].ljust(4, b'\0'), 'little'))


def regs(n):
    if n.get('reg_ranges'):
        return n['reg_ranges']
    h = n.get('reg')
    if not isinstance(h, str) or len(h) < 32:
        return None
    b = bytes.fromhex(h)
    return [f"{int.from_bytes(b[i:i + 8], 'little'):#x}/{int.from_bytes(b[i + 8:i + 16], 'little'):#x}"
            for i in range(0, len(b) - 15, 16)]


def macos_facts(r):
    apd = (r.get('summary') or {}).get('ane_port_detail') or {}
    mac = apd.get('macos')
    if not isinstance(mac, dict) or not mac.get('available'):
        return None
    drv = mac.get('driver') or {}
    out = {'platform': mac.get('platform'), 'set_base_candidate': mac.get('set_base_candidate'),
           'driver': {k: drv.get(k) for k in ('ane_version', 'ane_minor_version', 'arch', 'cores',
                                              'kext_version', 'instance_count', 'firmware_loaded')} if drv else None,
           'instances': [{k: i.get(k) for k in ('arch', 'hw_board_type', 'version', 'minor_version', 'cores',
                                                'firmware_loaded')} for i in mac.get('instances') or []],
           'ane': [], 'dart': [], 'pmgr0': None, 'truncated': mac.get('truncated')}
    skip = {'reg', 'reg_ranges', 'range_kinds', 'IOInterruptSpecifiers', 'IOInterruptControllers', 'compatible',
            'name', 'phandle', 'dart_id', 'dart_options'}
    for n in mac.get('ane_nodes') or []:
        out['ane'].append({'name': n.get('name'), 'compatible': n.get('compatible'), 'reg': regs(n),
                           'kinds': n.get('range_kinds'), 'irq': irq(n.get('IOInterruptSpecifiers')),
                           'keys': sorted(k for k in n if k not in skip), 'children': n.get('children')})
    for n in mac.get('dart_nodes') or []:
        opt = n.get('dart_options')
        out['dart'].append({'name': n.get('name'), 'reg': regs(n), 'irq': irq(n.get('IOInterruptSpecifiers')),
                            'dart_id': n.get('dart_id'),
                            'dart_options': opt.encode('latin-1').hex() if isinstance(opt, str) else opt,
                            'keys': sorted(k for k in n if k not in skip)})
    pm = mac.get('pmgr_nodes') or []
    if pm:
        out['pmgr0'] = {'name': pm[0].get('name'), 'first': (pm[0].get('reg_ranges') or [None])[0],
                        'total': pm[0].get('reg_ranges_total')}
    return out


DART_KEYS = ('85800000', '85810000', '85820000', '6b800000', '6b810000', '6b820000')


def linux_facts(r):
    apd = (r.get('summary') or {}).get('ane_port_detail') or {}
    dt = apd.get('devicetree')
    if not isinstance(dt, dict) or 'pmgr_domains' not in dt:
        return None
    rt = apd.get('runtime') or {}
    darts = dt.get('darts') or {}
    return {'ane_node_present': dt.get('ane_node_present'), 'ane_nodes': dt.get('ane_nodes'),
            'ane_reg': dt.get('ane_reg'),
            'pmgr_domains': [(d.get('label'), d.get('path')) for d in dt.get('pmgr_domains') or []],
            'pmgr_blocks': [(b.get('path'), b.get('reg')) for b in dt.get('pmgr_blocks') or []],
            'aic': dt.get('aic'),
            'ane_darts': {k: v for k, v in darts.items() if any(s in k for s in DART_KEYS)},
            'n_darts': len(darts),
            'board': (dt.get('boot') or {}).get('compatible'),
            'chosen': {k: v for k, v in ((dt.get('boot') or {}).get('chosen') or {}).items() if 'version' in k},
            'dtb_sha256': dt.get('dtb_sha256'), 'truncated': apd.get('truncated'),
            'runtime_nonnull': {k: v for k, v in rt.items() if v is not None and k != 'dmesg'},
            'dmesg_n': len(rt.get('dmesg') or []),
            'dmesg_ane': [l for l in rt.get('dmesg') or []
                          if any(s in l for s in ('ane', 'ANE', '2858', '2854', '26b8', '26b4', 'mailbox'))]}


rows = []
for f in sorted(glob.glob(sys.argv[1] + '/*.json')):
    r = json.load(open(f))
    rows.append({'sha12': r['content_sha256'][:12], 'kind': r.get('kind'), 'chip': r.get('chip'),
                 'model': r.get('model'), 'kernel': r.get('kernel'),
                 'generated_at': (r.get('summary') or {}).get('generated_at'),
                 'macos': macos_facts(r), 'linux': linux_facts(r)})
with open(sys.argv[2], 'w') as fh:
    json.dump(rows, fh, indent=1, sort_keys=True)
print(len(rows))
