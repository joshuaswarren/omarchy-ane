#!/usr/bin/env python3
"""Per-SoC derived ANE facts from the community rows: values and short row ids only.

usage: table.py FACTS.json ROWS_DIR > derived-facts.md
"""
import collections, glob, json, sys

facts = json.load(open(sys.argv[1]))
raw = {json.load(open(f))['content_sha256'][:12]: json.load(open(f)) for f in glob.glob(sys.argv[2] + '/*.json')}

MAC_SOC = {'Apple M2 Pro': 't6020', 'Apple M2 Max': 't6021', 'Apple M2 Ultra': 't6022', 'Apple M1 Ultra': 't6002',
           'Apple M2': 't8112', 'Apple M1 Pro': 't6000', 'Apple M1 Max': 't6001'}
WANT = ('t6000', 't6001', 't6002', 't6020', 't6021', 't6022', 't8112')


def soc_of(r):
    if (r['chip'] or '').startswith('apple,'):
        return r['chip'].split(',')[1]
    m = r['macos'] or {}
    return (m.get('platform') or {}).get('soc_id') or MAC_SOC.get(r['chip'])


def die(addr):
    """macOS arm-io relative address -> physical (arm-io base 0x200000000)."""
    a, s = addr.split('/')
    return f"{int(a, 16) + 0x200000000:#x}+{int(s, 16):#x}"


print('# Derived per-SoC facts (community rows, read 2026-10-01)\n')
print('Values only. Row = first 12 hex digits of the submission content sha256. macOS reg addresses are')
print('arm-io relative in IORegistry; the physical address below adds the arm-io base 0x200000000.\n')
for soc in WANT:
    rows = [r for r in facts if soc_of(r) == soc]
    print(f'## {soc}\n')
    print(f"Rows: {len(rows)} ({', '.join(sorted(r['sha12'] + ':' + r['kind'] for r in rows))})\n")
    agg = collections.defaultdict(list)
    for r in rows:
        m, l = r['macos'], r['linux']
        if m:
            p = m.get('platform') or {}
            agg[f"macOS host: {r['kernel']}, board {p.get('target_type')}"].append(r['sha12'])
            for a in m['ane']:
                if a['reg']:
                    agg[f"ane node {a['name']} reg " + ', '.join(die(x) for x in a['reg'])].append(r['sha12'])
                agg[f"ane node {a['name']} IOInterruptSpecifiers {a['irq']}"].append(r['sha12'])
            for d in m['dart']:
                if d['name'].startswith('mapper'):
                    continue
                if d['reg']:
                    agg[f"{d['name']} reg " + ', '.join(die(x) for x in d['reg'])].append(r['sha12'])
                agg[f"{d['name']} IOInterruptSpecifiers {d['irq']}"].append(r['sha12'])
                if d['dart_id'] is not None:
                    agg[f"{d['name']} dart-id {d['dart_id']}"].append(r['sha12'])
            sb = m.get('set_base_candidate')
            if sb:
                agg[f"set_base_candidate {sb.get('base')} (pmgr {sb.get('pmgr_block')} + {sb.get('offset')}), "
                    f"driver_window_confirms={sb.get('driver_window_confirms')}"].append(r['sha12'])
            for i in m['instances']:
                agg[f"H11ANEIn instance: hw_board_type {i['hw_board_type']}, ANEVersion {i['version']}, "
                    f"ANEMinorVersion {i['minor_version']}, arch {i['arch']}, cores {i['cores']}, "
                    f"firmware_loaded {i['firmware_loaded']}"].append(r['sha12'])
            if m.get('driver'):
                agg[f"AppleH11ANEInterface kext {m['driver'].get('kext_version')}"].append(r['sha12'])
        if l:
            if not l.get('board'):
                continue
            ch = l['chosen']
            agg[f"Linux board {l['board'][0]}"].append(r['sha12'])
            agg[f"Linux kernel {r['kernel']}"].append(r['sha12'])
            agg[f"boot: iBoot2 {ch.get('asahi,iboot2-version')}, os-fw {ch.get('asahi,os-fw-version')}, "
                f"m1n1 stage2 {ch.get('asahi,m1n1-stage2-version')}"].append(r['sha12'])
            agg[f"AIC {l['aic'].get('path')} {'/'.join(l['aic'].get('compatible') or [])}"].append(r['sha12'])
            for lab, path in l['pmgr_domains']:
                if lab and ('ane' in lab):
                    agg[f"pmgr ANE domain {lab} = {path}"].append(r['sha12'])
            for path, reg in l['pmgr_blocks'][:1]:
                agg[f"pmgr block {path} reg {reg}"].append(r['sha12'])
            agg[f"ANE node in booted DT: {l['ane_node_present']}"].append(r['sha12'])
            darts = (raw[r['sha12']].get('summary') or {}).get('ane_port_detail', {}).get('devicetree', {}).get('darts') or {}
            compats = sorted({'/'.join(v['compatible']) if isinstance(v.get('compatible'), list) else str(v.get('compatible'))
                              for v in darts.values()})
            cells = sorted({len(v.get('interrupts') or []) for v in darts.values()})
            agg[f"DART compatibles {compats}, AIC interrupt cells {cells}"].append(r['sha12'])
            agg[f"dtb_sha256 {l['dtb_sha256']}"].append(r['sha12'])
            agg[f"runtime module fields {sorted(l['runtime_nonnull'])}"].append(r['sha12'])
    for k in sorted(agg):
        print(f"- {k}: {len(agg[k])} row(s) {', '.join(sorted(agg[k]))}")
    print()
