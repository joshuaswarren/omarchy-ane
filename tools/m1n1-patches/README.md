# m1n1 patches

Patches against upstream AsahiLinux/m1n1 `b4654b3` (hv/trace_nvme: fix DBS
reg names). Each is a `git format-patch` output; apply with `git am` on a
clone, or with `patch -p1` on an unpacked proxyclient copy.

## 0001-hv-scrub-atcrt.patch

`proxyclient/m1n1/hv/__init__.py`, `setup_adt()`: remove every
`/arm-io/i2c*/atcrt*` node from the guest ADT when the hypervisor runs over
a USB iodev. Proxyclient-side only; no m1n1 binary rebuild.

Why: on the J414c (M2 Max) both traced macOS 13.5 boots died about 35 s in,
with the host raising pyserial "device reports readiness to read but
returned no data". The last ~500 lines of each hv trace are only ps_i2c6
(0x290280230) power cycles: 58 i2c6 transactions in run a, 53 in run b,
then the link drops. The only children of /arm-io/i2c6 in the J414c ADT are
atcrt0, atcrt1 and atcrt2, the Type-C retimers at i2c 0x18, 0x19, 0x1a,
matched by com.apple.driver.AppleTypeCRetimer. `setup_adt()` already scrubs
the proxy port's dart-usb0, atc-phy0, usb-drd0, acio0, hpm0 and atc0-dp*
nodes, but not the retimer, so macOS reprogrammed the retimer on the port
carrying the proxy. The port identity is pinned by the ADT: hpm0
(left-back), atc-phy0 and usb-drd0 share dock id 0x183, and the hv runs over
iodev USB0 (index 0).

Apply on the proxy host (the copy run_guest.py imports from):

    cd ~/m2proxy/hvproxy && patch -p1 < 0001-hv-scrub-atcrt.patch

Expected new lines in run.log, right after `Removing ADT node
/arm-io/atc-phy0`:

    Removing ADT node /arm-io/i2c6/atcrt0
    Removing ADT node /arm-io/i2c6/atcrt1
    Removing ADT node /arm-io/i2c6/atcrt2

Verified before landing: the patched `HV.setup_adt()` run against the real
J414c ADT (decoded from DeviceTree.j414cap.im4p with the m1n1 parser) prints
those three lines plus the unchanged hpm0/atc-phy0 lines; the tree rebuilds
to 353612 bytes and reparses with /arm-io/i2c6 empty. `patch -p1 --dry-run`
succeeds on the hvproxy proxyclient copy (its hv/__init__.py is identical to
upstream b4654b3).
