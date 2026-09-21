#!/usr/bin/env python3
"""Offline runner control-flow check with memory/register test doubles."""
import contextlib
import importlib.util
import io
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('hybrid', Path(__file__).with_name('h16_hybrid_boot.py'))
assert spec and spec.loader
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

class Memory:
    def __init__(self, observed):
        self.observed = observed
        self.released = False
    def window(self, *args):
        pass
    def rd64(self, addr):
        return m.ENTRY_IOVA | 1
    def rd32(self, addr):
        return self.observed if self.released and addr == m.ANE_BASE + m.REG_SCRATCH7 else 0
    def wr32(self, addr, value):
        if addr == m.ANE_BASE + m.REG_CPUCTRL:
            self.released = value == m.CPU_RUN_RELEASE

for diagnostic, staged, observed, expected, event in [
    (True, 'Y', 0x4d325431, 0, 'pollA.MARKER'),
    (False, 'N', m.BOOT_ACK, 0, 'pollA.READY'),
    (True, 'Y', m.BOOT_ACK, 2, 'pollA.TIMEOUT'),
    (False, 'N', 0x4d325431, 2, 'pollA.TIMEOUT'),
    (True, 'N', 0, 2, None),
    (False, 'Y', 0, 2, None),
]:
    events = []
    def open_param(path, *args, **kwargs):
        values = {'fw_diag_marker': staged, 'fw_load': 'Y', 'fw_iova': '0x100000', 'refcnt': '1'}
        return io.StringIO(values[Path(path).name])
    argv = ['hybrid', '--polls', '1'] + (['--diagnostic-marker'] if diagnostic else [])
    with patch.object(m.sys, 'argv', argv), patch('builtins.open', open_param), \
         patch.object(m, 'DevMem', return_value=Memory(observed)) as constructor, \
         patch.object(m.os, 'system', return_value=0), patch.object(m.time, 'sleep'), \
         patch.object(m, 'log', side_effect=lambda ev, **kw: events.append(ev)), \
         contextlib.redirect_stderr(io.StringIO()):
        try:
            status = m.main()
        except SystemExit as exc:
            status = exc.code
        assert status == expected, (diagnostic, staged, events, status)
        if event is None:
            constructor.assert_not_called()
        else:
            assert event in events, events
            assert not ('pollA.READY' in events and diagnostic), events
print('PASS mode mismatch refuses before MMIO; marker and READY are never interchangeable')
