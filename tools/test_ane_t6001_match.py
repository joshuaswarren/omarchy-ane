#!/usr/bin/env python3
"""Offline check that the T600x secondary and die-1 ANE engines are
reachable in the driver's tables. aurora-silicon/linux
integration/aurora names every non-die-0-primary T600x engine
"apple,t6001-ane" (ane1 on t6000/t6001; ane1/ane2/ane3 on t6002), so:

1. ane_of_match must list apple,t6001-ane, or those nodes never probe.
2. ane_qual_table must key the measured die-1 SET base 0x228e08c000 to
   t6001-ane, or the die-1 row sits behind a compatible no Aurora node
   carries and ane2 can never leave ANE_UNSUPPORTED.
3. ane_soc_t6001 must keep .ps_base = 0: with no proven die-0 second-
   engine base, ane1/ane3 fail -ENODEV instead of touching a guessed
   SET window (a wrong base external-aborts the SoC).

No device, no module load; source text only.
"""
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
repo = Path(__file__).resolve().parents[1]
text = (repo / 'ane/src/ane_drv.c').read_text()

match = text.split('of_device_id ane_of_match[] = {', 1)[1].split('};', 1)[0]
matched = set(re.findall(r'\.compatible = "([^"]+)"', match))
assert 'apple,t6001-ane' in matched, \
    f'ane_of_match lacks apple,t6001-ane: {sorted(matched)}'

qual = text.split('ane_soc_qual ane_qual_table[] = {', 1)[1].split('};', 1)[0]
rows = set(re.findall(r'\{ "(apple,t[0-9]+-ane)", (0x[0-9a-fA-F]+)ULL?, (\w+) \}', qual))
assert ('apple,t6001-ane', '0x228e08c000', 'ANE_RECOGNIZED') in rows, \
    f'no t6001-ane row at the measured die-1 SET base: {sorted(rows)}'

soc = text.split('ane_soc ane_soc_t6001 = {', 1)[1].split('};', 1)[0]
assert '.ps_base = 0' in soc and '.qual = ANE_UNSUPPORTED' in soc, \
    'ane_soc_t6001 must not carry a fabricated SET base'

print('PASS t6001-ane coverage: match entry, die-1 qual row, no fabricated base')
