# Insmod-time ANE test module (staged 2026-09-24)

Built on jw14m2-linux as /var/tmp/ane-rtb2/ane_t6021_rtclient.ko
(sha256 eeccb07d...). The `fw_cache_test=1` path does not bind the platform driver.

- fw_cache_test=1: cached segment map, sid15 TCR 0x2, PTE dump,
  release, 60 s SCRATCH7/outbox poll.
- fw_asc_reset=1: adds isp_reset_coproc (isp-fw.c:215) before the
  release: EDPRCR 0x1010310=2, 0x738/0x798/0x7f8/0x858=0xff00ff,
  0x1400a00..a14=~0, poll 0x818/0x81c, poll STATUS&3.

Not run with fw_asc_reset=1. T6001AscDebug tests the reset on jw16 first.
