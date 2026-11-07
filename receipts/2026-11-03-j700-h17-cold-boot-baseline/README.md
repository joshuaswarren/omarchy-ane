# J700/H17: zero-ANE baseline on a cold boot

Boot `8ee27fc5-c329-4e24-9841-0c72d0f71a5a`, kernel `7.2.11-14-1-ARCH`.
Cold boot: shutdown, charger disconnected, power on at 18:16:14 PST.

Repo state at start: `9b2b1ad`, clean, pushed, `git fsck` silent.
`sync` completed before any hardware action.

## Purpose

Every ANE-experiment boot so far has reset. Every no-ANE boot so far was a
*warm* reboot off a dirty state and also reset, in 2–4 min. The one
untested case is a cold boot with **no ANE access at all**.

If this boot resets with nothing touched, the "ANE access causes the
reset" model is wrong for this boot and the search moves to the host.
If it stays up, the clock starts at the moment of ANE access and the
raise path becomes the next variable.

## Rules for this run

- No `insmod` of any `ane_h17_*` module.
- No writes to any device.
- No `/dev/mtd1ro` reads, no ADT work, no module builds — nothing that
  touches the ANE windows.
- Check uptime and `journalctl -b 0` only.

## Result

Pending. Target: 40 minutes without a reset.
