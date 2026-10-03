#!/usr/bin/env python3
"""dkms.conf against a kernel that ships the ANE driver and one that does not:

  python3 tools/test_dkms_exclusive.py        (dkms on PATH, or DKMS=/path/to/dkms)

Runs the real dkms as the calling user: a temporary dkms tree, source tree and
install tree, and an `id` first on PATH that reports uid 0 for dkms's root
check. Each fake kernel source tree has include/, a .config and a Makefile
whose "modules" target records that it ran and fails. With
CONFIG_DRM_ACCEL_ANE=m or =y, "dkms build" must exit 77 (BUILD_EXCLUSIVE) and
never run make; without it, dkms must run make (and fail, exit 10). Without
dkms it does nothing."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

VERSION = '0.0.0test'
CASES = [('CONFIG_DRM_ACCEL_ANE=m\n', 77, False),
         ('CONFIG_DRM_ACCEL_ANE=y\n', 77, False),
         ('# CONFIG_DRM_ACCEL_ANE is not set\n', 10, True)]


def run_cases(dkms):
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        shim = work / 'bin'
        shim.mkdir()
        (shim / 'id').write_text(f'#!/bin/sh\n[ "$1" = -u ] && echo 0 && exit 0\nexec {shutil.which("id")} "$@"\n')
        (shim / 'id').chmod(0o755)
        env = dict(os.environ, PATH=f'{shim}:{os.environ["PATH"]}')
        src = work / 'src' / f'omarchy-ane-{VERSION}'
        (src / 'ane' / 't6021').mkdir(parents=True)
        (src / 'dkms.conf').write_text((root / 'dkms.conf').read_text().replace('@PKGVER@', VERSION))
        for n, (config, want, built) in enumerate(CASES):
            kernel, marker = work / f'kernel{n}', work / f'make-ran{n}'
            (kernel / 'include').mkdir(parents=True)  # dkms's check for kernel headers
            (kernel / '.config').write_text(config)
            (kernel / 'Makefile').write_text(f'modules:\n\ttouch {marker}\n\texit 1\n')
            (work / f'dkms{n}').mkdir()
            tree = ['--dkmstree', str(work / f'dkms{n}'), '--sourcetree', str(work / 'src'),
                    '--installtree', str(work / f'install{n}')]
            add = subprocess.run([dkms, 'add', '-m', 'omarchy-ane', '-v', VERSION, *tree],
                                 env=env, capture_output=True, text=True)
            assert add.returncode == 0, add.stdout + add.stderr
            build = subprocess.run([dkms, 'build', '-m', 'omarchy-ane', '-v', VERSION, '-k', f'7.1.0-test{n}',
                                    '--kernelsourcedir', str(kernel), *tree],
                                   env=env, capture_output=True, text=True)
            out = build.stdout + build.stderr
            assert build.returncode == want, (config, build.returncode, out)
            assert marker.exists() == built, (config, 'make ran' if marker.exists() else 'make did not run', out)
            print(f'test_dkms_exclusive: {config.strip()}: exit {build.returncode}, '
                  + ('built' if built else 'skipped'))
    print('test_dkms_exclusive: ok')


dkms = os.environ.get('DKMS') or shutil.which('dkms')
if dkms:
    run_cases(dkms)
else:
    print('test_dkms_exclusive: no dkms (PATH or DKMS); nothing checked')
