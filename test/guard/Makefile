CFLAGS ?= -Wall -Werror -O2

all: main.out

main.out: main.c ../../ane/src/ane_ps.h
	gcc $(CFLAGS) -I. -I../../ane/src main.c -o main.out

# Failing check, two halves:
# 1. main.out: verdict vectors from the 2026-09-16/20 journal captures run
#    through the same aggregate/predicate the driver consults.
# 2. call site: inside ane_submit, engine_lock -> removed -> wedged ->
#    power guard -> enqueue -> unlock, in that order, with the guard's
#    -ENODEV error path leaving through the unlock label. A tree without
#    the guard, with it outside the lock, or behind the enqueue fails.
check: main.out
	./main.out
	awk 'BEGIN {lck = rem = wed = grd = enq = unl = endev = gotolabel = -1} /static int ane_submit\(/ {body = 1} body && /^\}$$/ {body = 0} !body {next} /mutex_lock\(&ane->engine_lock\)/ && lck < 0 {lck = NR} /if \(ane->removed\)/ && rem < 0 {rem = NR} /atomic_read\(&ane->wedged\)/ {wed = NR} /ane_tm_islands_on\(ane, &ps_act\)/ && grd < 0 {grd = NR} /ane_tm_enqueue\(ane, &req\)/ && enq < 0 {enq = NR} /mutex_unlock\(&ane->engine_lock\)/ && unl < 0 {unl = NR} grd > 0 && grd + 8 > NR && /-ENODEV/ && endev < 0 {endev = NR} grd > 0 && grd + 8 > NR && /goto unlock;/ && gotolabel < 0 {gotolabel = NR} END {if (lck < 0 || rem < 0 || wed < 0 || grd < 0 || enq < 0 || unl < 0) {print "FAIL: ane_submit missing a submit-path anchor (pre-submit power guard?)"; exit 1} if (!(lck < rem && rem < wed && wed < grd && grd < enq && enq < unl)) {print "FAIL: guard ordering must be lock, removed, wedged, power, enqueue, unlock (got " lck " " rem " " wed " " grd " " enq " " unl ")"; exit 1} if (endev < 0 || gotolabel < 0) {print "FAIL: guard error path must -ENODEV and leave via unlock"; exit 1} print "ok   call site: lock, removed, wedged, power guard, enqueue, unlock"}' ../../ane/src/ane_drv.c

clean:
	rm -f *.out

.PHONY: all check clean
