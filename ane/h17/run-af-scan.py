#!/usr/bin/env python3
"""Prepare durable identity; load only with --run --cold-boot-confirmed."""
import argparse
import hashlib
import json
import os
import pwd
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def cmd(*args):
    # sudo must not switch Git/SSH to root's unrelated credentials.
    if args[0] == 'git' and os.geteuid() == 0 and os.environ.get('SUDO_USER'):
        user = pwd.getpwnam(os.environ['SUDO_USER']).pw_name
        args = ('runuser', '-u', user, '--', *args)
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()


def durable(path, data):
    with path.open('x') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def syncdir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run', action='store_true')
    p.add_argument('--cold-boot-confirmed', action='store_true')
    a = p.parse_args()
    if not a.run:
        print('Prepared only. No device access. On a fresh cold boot use:')
        print('sudo python3 ane/h17/run-af-scan.py --run --cold-boot-confirmed')
        return
    if not a.cold_boot_confirmed or os.geteuid() != 0:
        p.error('--run requires root and --cold-boot-confirmed')
    if cmd('git', 'status', '--porcelain'):
        raise SystemExit('Refused: commit and push the working tree first.')
    head = cmd('git', 'rev-parse', 'HEAD')
    branch = cmd('git', 'branch', '--show-current')
    remote = cmd('git', 'ls-remote', '--heads', 'mine', f'refs/heads/{branch}').split()
    if not branch or not remote or remote[0] != head:
        raise SystemExit('Refused: exact branch HEAD must be pushed.')
    if any(line.split()[0].startswith('ane_h17')
           for line in Path('/proc/modules').read_text().splitlines()):
        raise SystemExit('Refused: another H17 module is loaded.')
    # Include previously unloaded experiments recorded in this boot.
    journal = cmd('journalctl', '-k', '-b', '0', '--no-pager')
    if 'ane_h17' in journal:
        raise SystemExit('Refused: H17 activity already recorded this boot.')
    module = ROOT / 'ane/h17/ane_h17_af_scan.ko'
    kernel = cmd('uname', '-r')
    vermagic = cmd('modinfo', '-F', 'vermagic', str(module))
    if vermagic.split()[0] != kernel:
        raise SystemExit('Refused: module/kernel mismatch.')
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    parent = Path('/var/log/omarchy-ane')
    parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    run = parent / ('af-scan-' + boot)
    run.mkdir(mode=0o700)  # Exclusive: a failed attempt consumes this boot.
    log = run / 'probe.log'
    args = ['insmod', str(module), 'optin=1', f'log_path={log}']
    durable(run / 'manifest.json', json.dumps({
        'boot': boot, 'commit': head, 'kernel': kernel, 'vermagic': vermagic,
        'module_sha256': hashlib.sha256(module.read_bytes()).hexdigest(),
        'engine_reads': [{'offset': '0x0', 'width': 4},
                         {'offset': '0x338', 'width': 4}],
        'command': args, 'cold_boot_confirmed_by_operator': True,
        'scope': 'genpd raise, two reads, genpd release; no engine writes',
    }, indent=2) + '\n')
    durable(log, '')
    syncdir(run)
    syncdir(parent)
    os.sync()
    print('Manifest and empty log synced:', run, flush=True)
    rc = subprocess.run(args, cwd=ROOT).returncode
    durable(run / 'insmod-result.json', json.dumps({'exit': rc}) + '\n')
    syncdir(run)
    if rc == 0:
        rc = subprocess.run(['rmmod', 'ane_h17_af_scan']).returncode
        durable(run / 'rmmod-result.json', json.dumps({'exit': rc}) + '\n')
        syncdir(run)
    os.sync()
    print(log.read_text())
    raise SystemExit(rc)


if __name__ == '__main__':
    main()
