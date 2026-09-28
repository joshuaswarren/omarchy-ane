# LOAD_PROGRAM install path (2026-09-28)

No device write. Run these on the M2 lane after SSH or serial is back.

## 1. Write the command file

On a checkout of `ane-linux-experiments` branch `agent/h14-load-program`:

```
python3 tools/h14_load_program.py --stage /tmp/load_program.bin
```

That file is 440 bytes. Id at offset 4 is `0x0200`. The generic record is present and its device address is 0. The driver allocates the section and patches the address. Do not put a guessed address in the file.

## 2. Install it where request_firmware looks

```
sudo install -D -m 0644 /tmp/load_program.bin /lib/firmware/apple/ane/load_program.bin
```

The driver name is `apple/ane/load_program.bin`. The directory is the same one that holds `t602x_ane0_fw_selene_rc4x.macho`.

## 3. Load the client from this branch

Build `ane/t6021` from `omarchy-ane` branch `agent/h14-load-program-submit`. Do not load `ane_t6021.ko` on this box.

```
sudo insmod ane/t6021/ane_t6021_rtclient.ko csne_load_program=1
```

Default is off. A missing file, a size other than 440, or an id other than `0x0200` logs and does not submit.

## 4. What success is, and what it is not

dmesg should show `csne: submit` with `cursor=256` and `len=440`. That is the doorbell, not a finished program. The staged section is the firmware's minimum header (word 1, count 1, one absent nested record). The next command after a firmware reply is `CREATE_PROCESS` (`0x0202`), then `PROCEDURE_CALL` (`0x0204`). Those wait on the reply. Do not send them before the load reply names a program id.
