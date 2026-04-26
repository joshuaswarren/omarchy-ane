
# Asahi Neural Engine

Reverse engineered Linux driver for the Apple Neural Engine (ANE).

All things Linux here.

- ane/: Kernel module (ane.ko). Should move into tree soon.
- docs/: Documentation. WIP. Please don't look.
- libane/: Userspace lib.
- python/: Python bindings for libane.

## How to install

```
make
cd ane && sh run.sh install
```

make sure ane is in lsmod


## Dump GEM

```
handle[0]=1 → BO for tile/buffer index 0 (where the command/weights live)
handle[4]=2 → BO for output tile (dst 0)
handle[5]=3 → BO for input 0
handle[6]=4 → BO for input 1

python3 /home/asahi/ane-ex/dump.py /tmp/sum_cmd.bin \
  --decode-cmd --cmd-sbs-compact-grouped

python3 /home/asahi/ane-ex/dump.py /tmp/sum_weights.bin --dtype fp16 --count 64


python3 /home/asahi/ane-ex/dump.py /tmp/ane_bo_04_post.bin --dtype fp16 --tile 1,64,1,1,64,64 --count 8
[3. 3. 3. 3. 3. 3. 3. 3.]

python3 /home/asahi/ane-ex/dump.py /tmp/ane_bo_05.bin --dtype fp16 --tile 1,64,1,1,64,64 --count 8
[1. 1. 1. 1. 1. 1. 1. 1.]

python3 /home/asahi/ane-ex/dump.py /tmp/ane_bo_06.bin --dtype fp16 --tile 1,64,1,1,64,64 --count 8
[2. 2. 2. 2. 2. 2. 2. 2.]
```

## IOCTL

```
asahi@fedora:~/ane-ex$ sudo bpftrace -e '
tracepoint:syscalls:sys_enter_ioctl /args->cmd == 0xc0186441/ { printf("ANE BO_INIT\n"); }
tracepoint:syscalls:sys_enter_ioctl /args->cmd == 0xc0086442/ { printf("ANE BO_FREE\n"); }
tracepoint:syscalls:sys_enter_ioctl /args->cmd == 0xc0986443/ { printf("ANE SUBMIT\n"); }'
Attaching 3 probes...
ANE BO_INIT
ANE BO_INIT
ANE BO_INIT
ANE BO_INIT
ANE BO_INIT
ANE SUBMIT
ANE BO_FREE
ANE BO_FREE
ANE BO_FREE
ANE BO_FREE
ANE BO_FREE
```

## Troubleshoot

1. /dev/accel/accelXXX not found https://github.com/eiln/ane/issues/6
```
To enable typec dp and ane,
- git clone https://github.com/AsahiLinux/linux
- git switch fairy-dust
- (for M1 only, idk what patch for other) apply patch in this commit https://github.com/eiln/linux/commit/6027c18cc9e9a125d0b2854b04a52e1672355886
- follows steps here to compile https://grzegorz-smajdor.com/blog/2026-monitor-asahi-fedora/
- git clone https://github.com/eiln/ane
- cd ane && sudo make && cd ane && sh run.sh install
- if make failed, try my fork https://github.com/allbilly/libane
```

I have device tree overlay working before but i cant make it work anymore, so just compile the kernel anyway as i need typec dp

```
/boot/dtbs/overlays/ane.dtbo
/boot/efi/m1n1/boot.conf

[device-tree]
overlay=overlays/ane.dtbo
```

2. Driver issue

```
GPT: I fixed it by changing the ANE driver’s file‑operations so /dev/accel/accel0 opens through the DRM accel path instead of the generic DRM path.

What was happening:

libane opens /dev/accel/accel0, which calls the driver’s fops->open.
Your driver had .open = drm_open but didn’t set fop_flags.
Newer kernels warn and return -EINVAL in drm_open_helper when FOP_UNSIGNED_OFFSET isn’t set (that’s the warning you saw).
So open() failed with ENODEV, and libane said “failed to find device.”
The fix in ane_drv.c:

Use the accel open path and set the required flag:
.open = accel_open,
...
.fop_flags = FOP_UNSIGNED_OFFSET,
I also restored the usual DRM file ops (drm_release, drm_poll, etc.) while keeping your custom ane_drm_unlocked_ioctl and ane_drm_mmap.

After rebuild + unload/reload the module, open() started working and dmesg showed:

ane_drm_open called
pm_runtime_resume_and_get returned 0
```

3. submit timeout and result all 0

```
Root cause was the ANEC data offset: your sum.ane has a 0x1000 header (the 0x800..0x1000 range is all zeros), but libane always reads payload from 0x800. That meant the driver was sending all‑zero command data, so DRM_IOCTL_ANE_SUBMIT timed out and outputs stayed zero.

I fixed libane to detect the padded header and read from 0x1000 when appropriate.

Changes:

ane.c
added anec_data_offset() to detect the extra 0x800 padding
ane_model_init() now uses that offset instead of a hardcoded 0x800
I rebuilt:

make -C /home/asahi/ane-ex/ane/libane
make -C /home/asahi/ane-ex/ane/bindings/python/dylib
After that, run_sum.py produced non‑zero outputs.
```



