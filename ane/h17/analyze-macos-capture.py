#!/usr/bin/env python3
"""Verify a collector bundle and emit an allowlisted, identifier-free summary."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import struct


def nodes(obj):
    if isinstance(obj, list):
        for item in obj:
            yield from nodes(item)
    elif isinstance(obj, dict):
        yield obj
        yield from nodes(obj.get('IORegistryEntryChildren', []))


def one(tree, name):
    found = [n for n in nodes(tree) if n.get('IORegistryEntryName') == name]
    if len(found) != 1:
        raise ValueError(f'Expected one {name}, got {len(found)}')
    return found[0]


def words(value):
    if not isinstance(value, bytes) or len(value) % 4:
        raise ValueError('Expected little-endian u32 data')
    return list(struct.unpack('<' + 'I' * (len(value) // 4), value))


def analyze(root):
    hashes = {}
    for line in (root / 'SHA256SUMS').read_text().splitlines():
        expected, filename = line.split(None, 1)
        path = (root / filename.strip().removeprefix('*')).resolve()
        if path.parent != root.resolve():
            raise ValueError('Checksum filename escapes capture directory')
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f'Checksum mismatch: {path.name}')
        hashes[path.name] = actual
    for required in ('service-tree.plist', 'device-tree.plist', 'os.txt', 'model.txt'):
        if required not in hashes:
            raise ValueError(f'Missing checksum: {required}')
    tree = plistlib.loads((root / 'service-tree.plist').read_bytes())
    ane = one(tree, 'ane')
    driver = one(tree, 'H11ANE')
    pmgr = one(tree, 'AppleT8140PMGR')
    props = driver.get('DeviceProperties', {})
    selected = ('ANEDevicePropertyANEVersion', 'ANEDevicePropertyANEHWBoardType',
                'ANEDevicePropertyANECPUSubType', 'ANEDevicePropertyANEMinorVersion',
                'ANEDevicePropertyNumANECores', 'ANEDevicePropertyTypeANEArchitectureTypeStr')
    return {
        'capture_hashes': hashes,
        'os': (root / 'os.txt').read_text().strip(),
        'model': (root / 'model.txt').read_text().strip(),
        'ane': {
            'properties': {k: props[k] for k in selected if k in props},
            'firmware_loaded': driver.get('FirmwareLoaded'),
            'exclave_proxy': driver.get('IOExclaveProxy'),
            'power_state': driver.get('IOPowerManagement'),
            'memory_resources': ane.get('IODeviceMemory'),
            'clock_gate_ids': words(ane['clock-gates']),
            'power_gate_ids': words(ane['power-gates']),
            'clock_ids': words(ane['clock-ids']),
        },
        'pmgr_class': pmgr.get('IOObjectClass'),
        'pmgr_mapping_property_names': [k for k in pmgr
                                        if 'regmap' in k.lower() or 'reggroup' in k.lower()],
        'limits': ['Registry state is not Linux device qualification.',
                   'No private kernel memory or clock-register map was captured.',
                   'A newer macOS build does not validate older private object offsets.'],
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.capture), indent=2))
