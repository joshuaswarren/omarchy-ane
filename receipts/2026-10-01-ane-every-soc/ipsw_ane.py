#!/usr/bin/env python3
"""ANE facts from an Apple macOS IPSW, without downloading the IPSW.

  ipsw_ane.py fetch URL DIR   BuildManifest.plist and every DeviceTree.*.im4p
                              (HTTP range reads), each ADT unwrapped to .adt
  ipsw_ane.py firmware DIR    board -> chip -> ANE firmware (BuildManifest)
  ipsw_ane.py adt DIR         per SoC: the /arm-io/ane* nodes, their DARTs and
                              the pmgr ANE power states, board by board

Needs pyliblzfse (the ADTs are LZFSE), and for "adt" construct plus m1n1's
proxyclient on PYTHONPATH (AsahiLinux/m1n1 proxyclient/m1n1/adt.py parses the
ADT and the pmgr tables). The range reader is packaging/omarchy-ane-firmware-fetch.
"""
import collections
import glob
import os
import plistlib
import re
import sys
import zipfile
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path

sys.dont_write_bytecode = True
tool = Path(__file__).resolve().parents[2] / 'packaging/omarchy-ane-firmware-fetch'
spec = spec_from_loader('fetch', SourceFileLoader('fetch', str(tool)))
fetch = module_from_spec(spec)
spec.loader.exec_module(fetch)


def cmd_fetch(url, out):
    import liblzfse
    os.makedirs(out, exist_ok=True)
    z = zipfile.ZipFile(fetch.RangeFile(url))
    for info in z.infolist():
        name = info.filename
        if name.startswith('Firmware/ane/') and not name.endswith('/'):
            print(f'firmware {name} {info.file_size}')
        if name != 'BuildManifest.plist' and not name.startswith('Firmware/all_flash/DeviceTree.'):
            continue
        data = z.read(info)
        if name.endswith('.im4p'):
            data = fetch.im4p_payload(data)
            if data[:4] == b'bvx2':
                data = liblzfse.decompress(data)
            name = name[:-len('.im4p')] + '.adt'
        Path(out, os.path.basename(name)).write_bytes(data)
        print(f'got {os.path.basename(name)} {len(data)}')


def cmd_firmware(out):
    bm = plistlib.load(open(Path(out, 'BuildManifest.plist'), 'rb'))
    rows = collections.defaultdict(set)
    for bi in bm['BuildIdentities']:
        fw = tuple(sorted((k, v['Info']['Path'].rsplit('/', 1)[-1]) for k, v in bi['Manifest'].items()
                          if 'Firmware/ane/' in v.get('Info', {}).get('Path', '') and 'Restore' not in k))
        if fw:
            rows[(bi['ApChipID'].lower(), bi['Info']['DeviceClass'])].add(fw)
    for (chip, board), fws in sorted(rows.items()):
        print(chip, board, ' '.join(f'{k}={v}' for fw in sorted(fws) for k, v in fw))


def cmd_adt(out):
    from m1n1 import adt as madt
    socs = collections.defaultdict(list)
    for path in sorted(glob.glob(str(Path(out, 'DeviceTree.j*.adt')))):
        t = madt.load_adt(Path(path).read_bytes())
        pm = t[t._pmgr_path]
        by_id = {t.pmgr_dev_get_id(d): d for d in pm.devices}
        name = lambda i: ('die1:' if i >> 28 else '') + (by_id[i & 0xffff].name if (i & 0xffff) in by_id else hex(i))
        armio = t['/arm-io']
        lines = []
        for n in armio:
            if not re.fullmatch(r'(ane\d*|dart-ane\d*)', n.name):
                continue
            regs = ' '.join(f'{a:#x}+{s:#x}' for a, s in (n.get_reg(i) for i in range(len(n.reg))))
            ints = ' '.join(str(i) for i in n.getprop('interrupts', []))
            gates = ' '.join(name(i) for i in n.getprop('clock-gates', []))
            extra = ''.join(f' {k}={n.getprop(k)}' for k in ('ane-type', 'die-id') if n.getprop(k) is not None)
            lines.append(f'  {n.name}: compatible={n.compatible[0]} reg={regs} interrupts={ints} gates={gates}{extra}')
        for d in pm.devices:
            if 'ANE' in d.name.upper() and not d.flags.no_ps:
                parents = ' '.join(name(p) for p in t.pmgr_dev_get_parents(d) if p)
                lines.append(f'  pmgr {d.name} {t.pmgr_dev_get_addr(d):#x} parents={parents}')
        used = {i & 0xfff for n in t.walk_tree() if not isinstance(n.getprop('interrupts', b''), (bytes, str))
                for i in n.getprop('interrupts') if isinstance(i, int)}
        lines.append(f'  AIC lines named by any node: highest {max(used)}; 1833 named: {1833 in used}')
        board = os.path.basename(path)[len('DeviceTree.'):-len('ap.adt')]
        socs[armio.compatible[0]].append((board, lines))
    for soc, boards in sorted(socs.items()):
        print(f'== {soc}')
        first = boards[0]
        for board, lines in boards:
            if board != first[0] and lines == first[1]:
                print(f'{board}: same as {first[0]}')
            else:
                print(f'{board}:')
                print('\n'.join(lines))


if __name__ == '__main__':
    what = sys.argv[1]
    if what == 'fetch':
        cmd_fetch(sys.argv[2], sys.argv[3])
    elif what == 'firmware':
        cmd_firmware(sys.argv[2])
    else:
        cmd_adt(sys.argv[2])
