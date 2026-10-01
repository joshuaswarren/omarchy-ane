#!/bin/bash
# Arm netconsole target gap11 on jw16 -> omp-studio-local 6666, then send a
# /dev/kmsg marker (verification happens studio-side).
set -e
NC=/sys/kernel/config/netconsole/gap11
if [ -d $NC ]; then echo 0 | sudo tee $NC/enabled >/dev/null; sudo rmdir $NC; fi
sudo mkdir $NC
cd $NC
echo enu1 | sudo tee dev_name >/dev/null
echo 192.168.10.244 | sudo tee local_ip >/dev/null
echo 192.168.10.235 | sudo tee remote_ip >/dev/null
echo 6666 | sudo tee remote_port >/dev/null
echo bc:24:11:82:c5:1e | sudo tee remote_mac >/dev/null
echo 1 | sudo tee enabled >/dev/null
echo "armed:"
grep -H . dev_name local_ip remote_ip remote_port remote_mac enabled | paste -sd" "
echo '<6>GAP11-KMSG-MARK-ARM-'"$(date -u +%H%M%S)"' netconsole gap11 armed' | sudo tee /dev/kmsg >/dev/null
echo marker sent
