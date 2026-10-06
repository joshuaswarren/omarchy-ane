#!/bin/bash
# Ticket 2: call_settle_us 0 (the commit's code path: no sleep after the
# finish event). Restores 1000 on every exit. Usage: after.sh
. "$(dirname "$0")/common.sh"
O=$S/after
mkdir -p "$O"
exec > >(tee "$O/run.log") 2>&1
restore() {
	echo 1000 | sudo -n tee $P >/dev/null
	echo "restored call_settle_us $(cat $P)"
}
trap restore EXIT
trap 'exit 143' TERM INT
state | tee "$O/state-start.txt"
sudo -n dmesg >"$O/dmesg"
echo 0 | sudo -n tee $P >/dev/null
[[ $(cat $P) == 0 ]] || { echo "STOP: could not set call_settle_us 0"; exit 2; }
echo "call_settle_us $(cat $P)"
lock $S/landing "$ADD" 5000 1 || { echo "STOP: landing mismatch"; exit 3; }
dmesg_ok "$O/dmesg" || exit 3
fail=0
for seed in $(seq 2 51); do
	lock $S/landing "$ADD" 20 "$seed" >"$O/landing-$seed.log" 2>&1 || fail=$((fail + 1))
done
echo "landing 50 processes x 20 calls: $fail failed"
cat "$O"/landing-2.log
[[ $fail == 0 ]] || { echo "STOP: landing mismatch"; exit 3; }
quiet && { add_runs "$O/y" || exit 3; }
dmesg_ok "$O/dmesg" || exit 3
for i in $(seq 1 10); do
	prog20 "$O/prog20-$i.f16" | grep -E 'exec ms|golden|REFUSE'
done
encoder "$O/enc" || exit 3
dmesg_ok "$O/dmesg" || exit 3
sha256sum "$S"/before/y-*.f16 "$O"/y-*.f16 "$S"/before/prog20-*.f16 "$O"/prog20-*.f16 2>/dev/null
echo "distinct add outputs, before+after: $(sha256sum "$S"/before/y-*.f16 "$O"/y-*.f16 | cut -d' ' -f1 | sort -u | wc -l)"
echo "distinct prog20 outputs, before+after: $(sha256sum "$S"/before/prog20-*.f16 "$O"/prog20-*.f16 | cut -d' ' -f1 | sort -u | wc -l)"
restore
state | tee "$O/state-end.txt"
echo AFTER_DONE
