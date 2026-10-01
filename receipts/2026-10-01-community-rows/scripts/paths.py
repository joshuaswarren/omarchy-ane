#!/usr/bin/env python3
"""Check every overlay target-path against the node paths a Linux row measured.

usage: paths.py ROWS_DIR DT_DIR
"""
import glob, json, re, sys

rows_dir, dt_dir = sys.argv[1:3]


def targets(soc):
    text = ''
    for f in (f'{dt_dir}/{soc}-ane.dts', f'{dt_dir}/{soc[:4]}x-ane.dtsi'):
        try:
            text += open(f).read()
        except OSError:
            pass
    return sorted(set(re.findall(r'target-path = "/([^"]+)"', text)))


for f in sorted(glob.glob(rows_dir + '/*.json')):
    r = json.load(open(f))
    chip = r.get('chip') or ''
    dt = ((r.get('summary') or {}).get('ane_port_detail') or {}).get('devicetree') or {}
    if not chip.startswith('apple,t') or not dt.get('pmgr_blocks'):
        continue
    soc = chip.split(',')[1]
    have = {dt.get('aic', {}).get('path'), 'soc'}
    labels = {}
    for b in dt['pmgr_blocks']:
        have.add(b['path'])
        for c in b.get('children') or []:
            p = b['path'] + '/' + c['name']
            have.add(p)
            labels[p] = c.get('label')
    want = targets(soc)
    if not want:
        continue
    res = []
    for t in want:
        if t == 'reserved-memory':
            res.append('reserved-memory=not-captured')
            continue
        ok = t in have
        res.append(f"{t.split('/')[-1]}{'' if ok else ' MISSING'}{'=' + labels[t] if t in labels else ''}")
    complete = all(len(b.get('children') or []) == b.get('children_total') for b in dt['pmgr_blocks'])
    print(r['content_sha256'][:12], r['kind'], soc, (dt.get('boot') or {}).get('compatible', [None])[0],
          'children-complete' if complete else 'children-TRUNCATED', ' '.join(res))
