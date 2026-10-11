#!/bin/bash
# state-sampler.sh OUTFILE : ~10 Hz CSV of everything readable that could change the ANE operating point.
# Read-only sysfs only. Stops when the file OUTFILE.stop appears or after 400 s.
# columns: t_s, busy_ns, jobs, sys_power_uW, heatpipe_uW, ac_in_uW, fan_rpm, nand_mC, charger_mC, tz0_mC, cpu0_kHz, cpu_hi_kHz, loadavg1
out=$1
HW=$(grep -l macsmc_hwmon /sys/class/hwmon/hwmon*/name | head -1 | xargs dirname)
STATS=/sys/class/accel/accel0/device/ane_stats
P0=$(ls -d /sys/devices/system/cpu/cpufreq/policy* | head -1)
PH=$(ls -d /sys/devices/system/cpu/cpufreq/policy* | tail -1)
echo "t_s,busy_ns,jobs,sys_power_uW,heatpipe_uW,ac_in_uW,fan_rpm,nand_mC,charger_mC,tz0_mC,cpu0_kHz,cpu_hi_kHz,loadavg1" > "$out"
t0=$(date +%s.%N)
end=$((SECONDS + 400))
while [ ! -e "$out.stop" ] && [ "$SECONDS" -lt "$end" ]; do
	now=$(date +%s.%N)
	read -r busy jobs < <(awk '/^busy_ns/{b=$2}/^jobs/{j=$2}END{print b, j}' "$STATS")
	echo "$(awk -v a="$now" -v b="$t0" 'BEGIN{printf "%.3f", a-b}'),$busy,$jobs,$(cat $HW/power1_input),$(cat $HW/power4_input),$(cat $HW/power2_input),$(cat $HW/fan1_input 2>/dev/null),$(cat $HW/temp1_input),$(cat $HW/temp3_input),$(cat /sys/class/thermal/thermal_zone0/temp),$(cat $P0/scaling_cur_freq),$(cat $PH/scaling_cur_freq),$(cut -d' ' -f1 /proc/loadavg)" >> "$out"
	sleep 0.1
done
