#!/bin/bash
W=/var/tmp/encoder-whole
mkdir -p /tmp/anep-x
flock -w 60 /tmp/m1-gpu.lock $W/build/tools/mlx-omarchy-ane-worker/mlx-omarchy-ane-worker --bundle $W/bundle --libane $W/libane-strict.so --deadline-ms 60000 --iterations 32 --input attention_mask=$W/smoke/in_attention_mask.bin --input input_features=$W/smoke/in_input_features.bin --save encoder_hidden=/tmp/anep-x/h.bin --save output_mask=/tmp/anep-x/m.bin > /var/tmp/ane-perf/run-boot/worker-postboot-n32.log 2>&1
echo "rc=$? $(grep -oE 'elapsed_ms=[0-9]+' /var/tmp/ane-perf/run-boot/worker-postboot-n32.log | tail -1) hidden16=$(sha256sum /tmp/anep-x/h.bin | cut -c1-16)"
dmesg | grep -c "285c04000.ane: boot"
