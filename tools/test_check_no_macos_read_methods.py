#!/usr/bin/env python3
"""Behaviour test for tools/check_no_macos_read_methods.py: builds a throwaway git repo and runs the check in it.

Cases: a clean tree passes; each banned kind of text fails when it is new; a file in the baseline may keep its count but not
gain a line; fewer matches than the baseline pass; the --list output names the file. Exit 0 = all cases behaved.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "check_no_macos_read_methods.py")
BANNED = {
    "kernel address": ["the handler sits at 0xfffffe0007f48000 in the kernel\n"],
    "code offset": ["see kext+0x1234 for the branch\n", "at kernelcache @ 0x20 the table\n", "kc+0x3f0 holds it\n"],
    "tool or command": ["run ipsw extract on the image\n", "ipsw kernel dump the file\n", "ipsw macho info it\n", "ipsw dl the build\n", "ipsw download the build\n", "use pyimg4 on it\n", "run img4tool -e x\n",
                        "otool -tV the binary\n", "open it in ghidra\n", "radare2 shows it\n", "objdump -d the file\n", "disarm64 the bytes\n", "jtool2 lists them\n"],
    "restore-image locator": ["member kernelcache.release.mac14j in the image\n", "at hdr_off=0x62be4d7db in it\n", "section __PRELINK_INFO holds it\n",
                              "segment __TEXT_EXEC has it\n", "read BuildManifest.plist first\n",
                              "https://updates.cdn-apple.com/2023SummerFCS/x/UniversalMac_13.5_Restore.ipsw\n", "the file UniversalMac_Restore.ipsw is big\n",
                              "the BuildManifest names the image\n", "members read by HTTP range\n", "fetch it from updates.cdn-apple.com/anything\n"],
    "Apple symbol": ["com.apple.driver.Foo is loaded\n", "AppleH16ANEInterface::start is called first\n", "AppleH11ANEInterface starts it\n", "AppleASCWrapV8 owns it\n",
                     "AppleA7IOP handles it\n", "AppleT6020PMGR::apply sets it\n", "AppleANELoadBalancer picks one\n", "ApplePMGR::send does it\n",
                     "AppleDART programs it\n", "AppleH16CamIn carries IPC\n"],
    "macOS-sourced value": ["macOS writes 0 to register 0x199c first\n", "the window is in the macOS 27.0 device tree: physical 0x292290000\n",
                            "ASC = 0x1600000  # kext 0x1600044 control\n", "0x20c  iBoot tables\n", "Apple's driver sets 0x400 and then 0x10\n",
                            "the restore image holds 0x8000 bytes\n", "the kernelcache entry is at 0x4000\n"],
    "decode wording": ["a static decode of the driver shows the order\n", "an offline decode of the kext\n", "decoded from the kext\n",
                       "decoded statically from the kernelcache\n", "reverse engineered the macOS driver\n", "reverse-engineering the kext\n", "decoded from the kernel extension\n"],
}
failures = []


def run(repo, *args):
    return subprocess.run([sys.executable, os.path.join(repo, "tools", "check_no_macos_read_methods.py"), *args], capture_output=True, text=True, cwd=repo, check=False)


def make_repo():
    repo = tempfile.mkdtemp(prefix="chk-")
    os.makedirs(os.path.join(repo, "tools"))
    shutil.copy(CHECK, os.path.join(repo, "tools"))
    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
    with open(os.path.join(repo, "clean.md"), "w") as f:
        f.write("The core stops before its first status write. Register 0x199c is the gate word.\nObserved macOS behaviour: it clears the gate word first (0x10 is the run bit).\n")
    commit(repo)
    return repo


def commit(repo):
    # The check reads `git ls-files`, so staging is enough; no commit (and no commit identity) is needed.
    subprocess.run(["git", "add", "-A"], cwd=repo, check=True)


def expect(name, got, want_rc, needle=""):
    if got.returncode != want_rc or needle not in got.stdout + got.stderr:
        failures.append(f"{name}: rc {got.returncode} want {want_rc}, output {(got.stdout + got.stderr)[:160]!r}")


repo = make_repo()
expect("clean tree passes", run(repo), 0, "ok")
for kind, samples in BANNED.items():
    for text in samples:
        with open(os.path.join(repo, "new.md"), "w") as f:
            f.write(text)
        commit(repo)
        expect(f"new {kind} fails: {text.strip()[:30]}", run(repo), 1, "new.md")
        with open(os.path.join(repo, "new.md"), "w") as f:
            f.write("Observed macOS behaviour: it writes 0 to the gate word first.\n")
        commit(repo)
# the firmware fetch tool carries its download addresses by decision; no other file may
os.makedirs(os.path.join(repo, "packaging"), exist_ok=True)
url = '"13.5": "https://updates.cdn-apple.com/2023SummerFCS/x/UniversalMac_13.5_Restore.ipsw"\n'
with open(os.path.join(repo, "packaging", "omarchy-ane-firmware-fetch"), "w") as f:
    f.write(url)
commit(repo)
expect("the fetch tool may hold its download addresses", run(repo), 0, "ok")
with open(os.path.join(repo, "packaging", "omarchy-ane-other-tool"), "w") as f:
    f.write(url)
commit(repo)
expect("another packaging file may not", run(repo), 1, "omarchy-ane-other-tool")
with open(os.path.join(repo, "packaging", "omarchy-ane-other-tool"), "w") as f:
    f.write("clean\n")
commit(repo)
# captured output is evidence and is never edited: the explicit list may carry the words, any other path may not
cap = "receipts/2026-10-01-t6021-default-on-gate/logs/B/dmesg-ane.txt"
other = "receipts/2026-10-01-t6021-default-on-gate/logs/B/dmesg-other.txt"
os.makedirs(os.path.dirname(os.path.join(repo, cap)), exist_ok=True)
line = "fwload: own memory: iBoot patches replayed (soc 0x6021 rev 0x11 DATA 0x100000c4000)\n"
with open(os.path.join(repo, cap), "w") as f:
    f.write(line)
commit(repo)
expect("a listed captured log may carry the words", run(repo), 0, "ok")
with open(os.path.join(repo, other), "w") as f:
    f.write(line)
commit(repo)
expect("an unlisted log in the same folder may not", run(repo), 1, "dmesg-other.txt")
for name in (cap, other):
    with open(os.path.join(repo, name), "w") as f:
        f.write("clean\n")
commit(repo)
# baseline: an existing offender may keep its count, not gain
with open(os.path.join(repo, "old.md"), "w") as f:
    f.write(BANNED["kernel address"][0] + "ordinary line\n")
commit(repo)
expect("offender without baseline fails", run(repo), 1, "old.md")
expect("write-baseline", run(repo, "--write-baseline"), 0, "baseline written")
commit(repo)
expect("offender at baseline passes", run(repo), 0, "ok")
with open(os.path.join(repo, "old.md"), "a") as f:
    f.write(BANNED["code offset"][0])
commit(repo)
expect("offender gaining a line fails", run(repo), 1, "old.md")
with open(os.path.join(repo, "old.md"), "w") as f:
    f.write("ordinary line\n")
commit(repo)
expect("offender with fewer lines fails until the baseline is lowered", run(repo), 1, "Lower the baseline")
expect("write-baseline lowers", run(repo, "--write-baseline"), 0, "baseline written")
commit(repo)
expect("lowered baseline passes", run(repo), 0, "ok")
with open(os.path.join(repo, "old.md"), "w") as f:
    f.write(BANNED["code offset"][0])
commit(repo)
expect("cleaned file regaining a different match fails", run(repo), 1, "old.md")
with open(os.path.join(repo, "old.md"), "w") as f:
    f.write("ordinary line\n")
commit(repo)
expect("baseline lowered again after cleaning", run(repo, "--write-baseline"), 0, "baseline written")
commit(repo)
expect("cleaned tree passes", run(repo), 0, "ok")
with open(os.path.join(repo, "old.md"), "w") as f:
    f.write(BANNED["kernel address"][0])
commit(repo)
expect("list names the file", run(repo, "--list"), 0, "old.md")
shutil.rmtree(repo)
if failures:
    print("FAIL")
    print("\n".join(failures))
    sys.exit(1)
print("ok: check_no_macos_read_methods behaves on", sum(len(v) for v in BANNED.values()) + 18, "cases")
