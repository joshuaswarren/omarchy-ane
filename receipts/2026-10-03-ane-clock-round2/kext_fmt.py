#!/usr/bin/env python3
"""Print os_log format strings and functions of a Mach-O kext whose symbol names match a regex.
Usage: kext_fmt.py <kext macho> <regex>   (read-only; LIEF parses the file, nothing is executed)"""
import re, sys, lief
b = lief.parse(sys.argv[1])
rx = re.compile(sys.argv[2])
def cstr(addr):
    try:
        data = bytes(b.get_content_from_virtual_address(addr, 400))
    except Exception:
        return None
    return data.split(b"\0")[0].decode(errors="replace")
for s in sorted(b.symbols, key=lambda s: s.value):
    if not rx.search(s.name) or not s.value:
        continue
    if "_os_log_fmt" in s.name:
        print("FMT 0x%x %s :: %r" % (s.value, s.name, cstr(s.value)))
    else:
        print("SYM 0x%x %s" % (s.value, s.name))
