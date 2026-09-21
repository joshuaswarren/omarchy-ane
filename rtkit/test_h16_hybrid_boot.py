#!/usr/bin/env python3
"""Offline CLI safety checks. These do not prove a safe hardware boot."""
import contextlib
import importlib.util
import io
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('hybrid', Path(__file__).with_name('h16_hybrid_boot.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

for options, expected in [
    (['--dry-run'], 0),
    (['--dry-run', '--with-table'], 0),
    (['--power-cycle'], 2),
    (['--island-cycle'], 2),
    (['--dry-run', '--power-cycle'], 2),
    (['--dry-run', '--island-cycle'], 2),
    (['--polls', '0'], 2),
]:
    with patch.object(module.sys, 'argv', ['hybrid', *options]), \
         patch.object(module, 'DevMem', side_effect=AssertionError('hardware opened')), \
         patch.object(module.os, 'system', side_effect=AssertionError('shell write')), \
         contextlib.redirect_stdout(io.StringIO()), \
         contextlib.redirect_stderr(io.StringIO()):
        try:
            result = module.main()
        except SystemExit as exc:
            result = exc.code
        assert result == expected, (options, result)
assert module.REG_SCRATCH1 == module.REG_SCRATCH0 + 4
print('PASS dry-run and refused options never open hardware or write kmsg')
