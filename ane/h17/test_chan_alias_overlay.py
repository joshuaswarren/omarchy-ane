# SPDX-License-Identifier: MIT
"""Compile only the manual overlay and inspect its IOVA reservations offline."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from chanreg_model import DM_IOVA, DM_BYTES, IPC_MAPPED, HEAP_IOVA


class ChannelAliasOverlayTests(unittest.TestCase):
    def test_manual_overlay_reserves_aliases_not_physical_ram(self):
        source = Path(__file__).resolve().parent.parent / 'h16/t8140-ane-stage-boot.dts'
        with tempfile.TemporaryDirectory(dir='/var/tmp/opencode') as tmp:
            dtbo = Path(tmp) / 'stage.dtbo'
            subprocess.run(['dtc', '-q', '-@', '-I', 'dts', '-O', 'dtb', '-o',
                            str(dtbo), str(source)], check=True)

            def prop(node, name):
                return subprocess.check_output(['fdtget', '-t', 'x', str(dtbo), node, name], text=True).split()

            node = '/fragment@4/__overlay__/ane-chan-alias-iova'
            cells = [int(value, 16) for value in prop(node, 'iommu-addresses')]
            self.assertEqual(len(cells), 5)
            self.assertEqual((cells[1] << 32) | cells[2], DM_IOVA)
            self.assertEqual((cells[3] << 32) | cells[4], DM_BYTES + IPC_MAPPED + 0xf00000)
            self.assertEqual(DM_IOVA + cells[4], HEAP_IOVA + 0xf00000)
            props = subprocess.check_output(['fdtget', '-p', str(dtbo), node], text=True).split()
            self.assertNotIn('reg', props)
            refs = [int(value, 16) for value in prop('/fragment@0/__overlay__', 'memory-region')]
            self.assertEqual(len(refs), 2)
            self.assertEqual(refs[1], int(prop(node, 'phandle')[0], 16))


if __name__ == '__main__':
    unittest.main()
