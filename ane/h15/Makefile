# SPDX-License-Identifier: GPL-2.0-only OR MIT
# OPT-IN EXPERIMENTAL module: not part of "make all", not in dkms.conf,
# not autoloaded (no MODULE_DEVICE_TABLE). Build by hand for bring-up:
#   make KERNELDIR=<kernel tree> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
#   make KERNELDIR=<kernel tree> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=1 modules
#   make check                 (host ADT walker self test)
ifneq ($(KERNELRELEASE),)
	obj-m := ane_h15.o
	ane_h15-objs := ane_h15_main.o ane_h15_soc.o ane_h15_overlay.o
	ccflags-y += -I$(src)
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

check: ane_h15_adt_test test_domain_window
	./ane_h15_adt_test
	./test_domain_window
	python3 test_trace_ane_h15.py

ane_h15_adt_test: ane_h15_adt_test.c ane_h15_adt.h
	$(CC) $(CFLAGS) -I. ane_h15_adt_test.c -o $@

# ---- userspace sim (PMP vehicle pattern; NOT part of make all) ----
# Compiles the REAL module sources against sim/shim.h and drives every
# stage. sim-test: positive controls, exit 0 required. The named
# sim-negative-* targets are the discriminating controls: each exits 0
# iff the module fails the induced fault correctly, and 1 (BAD) if the
# module could pass a broken environment. sim-negative-nopsguard builds
# a copy of the source with the PS guard call deleted and REQUIRES the
# harness to fail it (PMP sim-negative semantics).
SIM_CC ?= cc
SIM_CFLAGS ?= -g -O0 -Wall -Wextra -std=gnu11
SIM_SHA := $(abspath ../../libane/ane_sha256.c)
SIM_HDRS := $(wildcard ane_h15*.h) sim/shim.h \
	$(wildcard sim/inc/linux/*.h) sim/inc/crypto/sha2.h

test_domain_window: test_domain_window.c ane_h15_main.c ane_h15_soc.c $(SIM_HDRS) $(SIM_SHA)
	$(SIM_CC) $(SIM_CFLAGS) -I sim -I sim/inc -o $@ test_domain_window.c $(SIM_SHA)

sim/sim_h15: sim/sim_harness.c ane_h15_main.c ane_h15_soc.c $(SIM_HDRS) $(SIM_SHA)
	$(SIM_CC) $(SIM_CFLAGS) -I sim -I sim/inc -o $@ sim/sim_harness.c $(SIM_SHA)

test_rtk_startup: test_rtk_startup.c sim/sim_harness.c ane_h15_main.c ane_h15_soc.c $(SIM_HDRS) $(SIM_SHA)
	$(SIM_CC) $(SIM_CFLAGS) -I sim -I sim/inc -o $@ test_rtk_startup.c $(SIM_SHA)

test_t8122_start: test_t8122_start.c sim/sim_harness.c ane_h15_main.c ane_h15_soc.c $(SIM_HDRS) $(SIM_SHA)
	$(SIM_CC) $(SIM_CFLAGS) -I sim -I sim/inc -o $@ test_t8122_start.c $(SIM_SHA)

# P7 build: the fact table is substituted (synthetic, fully filled) so
# the real probe + stage_boot run the every-fact-filled fall-through.
sim/sim_h15_filled: sim/sim_harness.c ane_h15_main.c ane_h15_soc.c $(SIM_HDRS) $(SIM_SHA)
	$(SIM_CC) $(SIM_CFLAGS) -DANE15_SIM_FILL_FACTS -I sim -I sim/inc -o $@ sim/sim_harness.c $(SIM_SHA)

sim-probe-filled: sim/sim_h15_filled
	@test -r "$(H15_FW_INPUT_DIR)/t603x_ane0_fw_erebus_pc5x.im4p.payload" || { echo "private H15_FW_INPUT_DIR is required"; exit 1; }
	H15_FW_INPUT_DIR="$(H15_FW_INPUT_DIR)" ./sim/sim_h15_filled pos-filled

# The run-time overlay (apply_overlay=1): the REAL ane_h15_overlay.c against a fake live device tree, then the generated
# blob header must be what gen-runtime-overlay.py writes from the overlay sources.
sim/sim_overlay: sim/sim_overlay.c ane_h15_overlay.c ane_h15_overlay.h ane_h15_overlay_data.h $(wildcard sim/ovl/linux/*.h)
	$(SIM_CC) $(SIM_CFLAGS) -I sim/ovl -o $@ sim/sim_overlay.c

sim-overlay: sim/sim_overlay
	./sim/sim_overlay
	python3 gen-runtime-overlay.py --check

sim-overlay-mutation:
	python3 sim/mutate_overlay.py

sim-test: sim/sim_h15 sim-overlay test_rtk_startup test_t8122_start
	./sim/sim_h15 pos
	./sim/sim_h15 neg-run
	./test_rtk_startup
	./test_t8122_start

sim-negative-ps-stuck: sim/sim_h15
	./sim/sim_h15 neg-ps-stuck

sim-negative-wrong-stage: sim/sim_h15
	./sim/sim_h15 neg-wrong-stage

sim-negative-truncated-dt: sim/sim_h15
	./sim/sim_h15 neg-truncated-dt

sim-negative-rpm-fail: sim/sim_h15
	./sim/sim_h15 neg-rpm-fail

# Multiple power-domains (attach by id + runtime-PM device links).
sim-negative-pd-attach-fail: sim/sim_h15
	./sim/sim_h15 neg-pd-attach-fail

sim-negative-pd-link-fail: sim/sim_h15
	./sim/sim_h15 neg-pd-link-fail

sim-negative-pd-rpm-err: sim/sim_h15
	./sim/sim_h15 neg-pd-rpm-err

sim-negative-pd-eacces-alone: sim/sim_h15
	./sim/sim_h15 neg-pd-eacces-alone

# Stage 2 (confirm_read=1): the read-only preflight.
sim-negative-read-ps-stuck: sim/sim_h15
	./sim/sim_h15 neg-read-ps-stuck

sim-negative-read-window: sim/sim_h15
	./sim/sim_h15 neg-read-window

sim-negative-read-forbidden: sim/sim_h15
	./sim/sim_h15 neg-read-forbidden

# Mutation controls for the multi-domain code: build a copy of the real
# module with one sed edit and REQUIRE the named harness mode to fail it.
# $(call pd_mutant,<name>,<sed script>,<marker>,<harness mode>)
COMMA := ,
define pd_mutant
	rm -rf /tmp/ane_h15-$(1) && mkdir -p /tmp/ane_h15-$(1)
	cp -r sim /tmp/ane_h15-$(1)/sim
	cp ane_h15_main.c ane_h15_soc.c ane_h15*.h /tmp/ane_h15-$(1)/
	sed '$(2)' ane_h15_main.c > /tmp/ane_h15-$(1)/ane_h15_main.c
	grep -q '$(3)' /tmp/ane_h15-$(1)/ane_h15_main.c
	$(SIM_CC) $(SIM_CFLAGS) -I /tmp/ane_h15-$(1)/sim -I /tmp/ane_h15-$(1)/sim/inc -o /tmp/ane_h15-$(1)/sim_h15 /tmp/ane_h15-$(1)/sim/sim_harness.c $(SIM_SHA)
	@/tmp/ane_h15-$(1)/sim_h15 $(4) > /tmp/ane_h15-$(1)/run.log 2>&1; \
	s=$$?; \
	echo "--- $(1) mutant harness tail ---"; tail -4 /tmp/ane_h15-$(1)/run.log; \
	if [ $$s -eq 0 ]; then echo "SIM-MUT-$(1) BAD: mutant PASSED the harness (harness does not discriminate)"; exit 1; \
	else echo "SIM-MUT-$(1) OK: mutant FAILS the harness"; fi
endef

# pd_count set only after device_link_add: a failed link leaks its domain
sim-mutation-pd-nolinkdetach:
	$(call pd_mutant,pd-nolinkdetach,s/ane->pd_count = i + 1;/\/* NEG: pd_count set after link *\//,NEG: pd_count set after link,neg-pd-link-fail)

# any get_sync error ignored once domains are attached, not only -EACCES
sim-mutation-pd-anyerrignored:
	$(call pd_mutant,pd-anyerrignored,s/ret == -EACCES/ret < 0 \/* NEG: any error ignored *\//,NEG: any error ignored,neg-pd-rpm-err)

# a failed get_sync takes a usage count that nothing drops
sim-mutation-pd-noidleleak:
	$(call pd_mutant,pd-noidleleak,s/pm_runtime_put_noidle(&pdev->dev);/\/* NEG: usage count leaked *\//,NEG: usage count leaked,neg-pd-rpm-err)

# remove() no longer detaches the domains and deletes the links
sim-mutation-pd-noremovedetach:
	$(call pd_mutant,pd-noremovedetach,/^static void ane_h15_remove/$(COMMA)/^}/ s/ane_h15_detach_pds(ane);/\/* NEG: remove does not detach *\//,NEG: remove does not detach,pos)

# stage 2 reads without the pmgr ACTUAL=0xf gate
sim-mutation-read-noguard:
	$(call pd_mutant,read-noguard,/^static int ane_h15_stage_wrapper/$(COMMA)/^}/ s/ret = ane_h15_ps_wait(ane);/ret = 0; \/* NEG: stage 2 guard skipped *\//,NEG: stage 2 guard skipped,neg-read-ps-stuck)

# stage 2 read allowlist (two ADT windows, no CPU_CONTROL, no fatal range) skipped
sim-mutation-read-nocheck:
	$(call pd_mutant,read-nocheck,s/if (!ane_h15_read_ok(ane->soc$(COMMA) off$(COMMA) 4)) {/if (0 \/* NEG: read allowlist skipped *\/) {/,NEG: read allowlist skipped,neg-read-forbidden)

# stage 2 trusts the SoC row without comparing the live reg[0] and reg[1] resources
sim-mutation-read-nowindow:
	$(call pd_mutant,read-nowindow,s/res.start == pa \&\& resource_size(\&res) == size/1 \/* NEG: window check skipped *\//,NEG: window check skipped,neg-read-window)

# stage 2 runs without confirm_read=1
sim-mutation-read-noconfirm:
	$(call pd_mutant,read-noconfirm,s/if (!confirm_read) {/if (0 \/* NEG: confirm_read ignored *\/) {/,NEG: confirm_read ignored,pos)

# Stage 5 (confirm_run=1): the only stage with writes. neg-run = refusals + the stuck core.
sim-negative-run: sim/sim_h15
	./sim/sim_h15 neg-run

# stage 5 runs without confirm_run=1
sim-mutation-run-noconfirm:
	$(call pd_mutant,run-noconfirm,s/if (!confirm_run) {/if (0 \/* NEG: confirm_run ignored *\/) {/,NEG: confirm_run ignored,neg-run)

# stage 5 starts a core whose RVBAR is not the measured one
sim-mutation-run-norvbar:
	$(call pd_mutant,run-norvbar,s/(((u64)rv_hi << 32) | rv_lo) != s->rvbar_expect ||/0 \/* NEG: rvbar check skipped *\/ ||/,NEG: rvbar check skipped,neg-run)

# stage 5 starts a core with stale mailbox state
sim-mutation-run-nomboxcheck:
	$(call pd_mutant,run-nomboxcheck,s/!(a2i \& ANE_H15_MBOX_EMPTY) || !(i2a \& ANE_H15_MBOX_EMPTY)/0 \/* NEG: mailbox check skipped *\//,NEG: mailbox check skipped,neg-run)

# stage 5 never clears RUN again
sim-mutation-run-noclear:
	$(call pd_mutant,run-noclear,s/ctrl \& ~ANE_H15_CPU_RUN$(COMMA) "clear RUN"/ctrl$(COMMA) "clear RUN"/,ctrl$(COMMA) "clear RUN",pos)

# stage 5 reports a core that never stopped as stopped (remove() would then unmap under it)
sim-mutation-run-nostopcheck:
	$(call pd_mutant,run-nostopcheck,s/if (!stopped) {/if (0 \/* NEG: stuck core accepted *\/) {/,NEG: stuck core accepted,neg-run)

# stage 5 pops mailbox messages with 32-bit reads (loses the upper halves)
sim-mutation-run-pop32:
	$(call pd_mutant,run-pop32,s/\*v = readq_relaxed(ane->engine + off);/*v = readl_relaxed(ane->engine + off); \/* NEG: 32-bit pop *\//,NEG: 32-bit pop,pos)

# Stage 4 (confirm_read=1): the read-only DART scan; neg-dart also holds the run stage's DART gate.
sim-negative-dart: sim/sim_h15
	./sim/sim_h15 neg-dart

# stage 5 starts the core although no DART stream is enabled (the M6 silence)
sim-mutation-run-nodartgate:
	$(call pd_mutant,run-nodartgate,s/if (!live \&\& !run_unmapped) {/if (0 \/* NEG: DART gate skipped *\/) {/,NEG: DART gate skipped,neg-dart)

# a stream counts as live without a valid TTBR
sim-mutation-dart-nottbr:
	$(call pd_mutant,dart-nottbr,s/(tcr \& 1) \&\& (ttbr \& 1) \&\& on/(tcr \& 1) \&\& on \/* NEG: ttbr valid ignored *\//,NEG: ttbr valid ignored,pos)

# stage 4 reads the DART without confirm_read=1
sim-mutation-dart-noconfirm:
	$(call pd_mutant,dart-noconfirm,/^static int ane_h15_stage_dart/$(COMMA)/^}/ s/if (!confirm_read) {/if (0 \/* NEG: confirm_read ignored in dart *\/) {/,NEG: confirm_read ignored in dart,neg-dart)

# a core that will not stop leaves the module unpinned (rmmod then frees the code under it)
sim-mutation-run-nopin:
	$(call pd_mutant,run-nopin,s/__module_get(THIS_MODULE);/\/* NEG: module not pinned *\//,NEG: module not pinned,neg-run)

# Stage 5 map_fw=1: refusals, the undo after each failed step, the stuck core keeping its mapping.
sim-negative-map: sim/sim_h15
	./sim/sim_h15 neg-map

# the map is never undone (domain and mappings leak after a clean stop or a failed step)
sim-mutation-map-noundo:
	$(call pd_mutant,map-noundo,s/if (!ane->domain || ane->map_retained)/if (1 \/* NEG: unmap is a no-op *\/)/,NEG: unmap is a no-op,pos)

# a firmware segment inside system RAM is mapped into the ANE
sim-mutation-map-noramcheck:
	$(call pd_mutant,map-noramcheck,s/!= REGION_DISJOINT/== 99 \/* NEG: RAM check skipped *\//,NEG: RAM check skipped,neg-map)

# TEXT is mapped writable
sim-mutation-map-textrw:
	$(call pd_mutant,map-textrw,s/IOMMU_READ | (i ? IOMMU_WRITE : 0)/IOMMU_READ | IOMMU_WRITE \/* NEG: TEXT writable *\//,NEG: TEXT writable,pos)

# overlapping segments accepted
sim-mutation-map-nooverlap:
	$(call pd_mutant,map-nooverlap,s/pa\[0\] < pa\[1\] + size\[1\] \&\& /0 \&\& /,0 \&\& pa\[1\] <,neg-map)

# map_fw attaches and maps while the core may be running
sim-mutation-map-nostoppedcheck:
	$(call pd_mutant,map-nostoppedcheck,s/return !(st \& ANE_H15_ST_RUNNING) \&\& (st \& ANE_H15_ST_STOPPED);/return true; \/* NEG: stopped check skipped *\//,NEG: stopped check skipped,neg-map)

# map_fw runs on a node without iommus
sim-mutation-map-noiommus:
	$(call pd_mutant,map-noiommus,s/if (!device_iommu_mapped(ane->dev)) {/if (0 \/* NEG: iommus not required *\/) {/,NEG: iommus not required,neg-map)

# Stage 6 (confirm_rtkit=1): the RTKit session over the wrapper mailbox. neg-rtkit = refusals, odd firmware, stuck core.
sim-negative-rtkit: sim/sim_h15
	./sim/sim_h15 neg-rtkit

# stage 6 runs without confirm_rtkit=1
sim-mutation-rtk-noconfirm:
	$(call pd_mutant,rtk-noconfirm,s/if (stg >= 6 \&\& !confirm_rtkit) {/if (0 \/* NEG: confirm_rtkit ignored *\/) {/,NEG: confirm_rtkit ignored,neg-rtkit)

# HELLO is answered whatever versions the firmware offers
sim-mutation-rtk-nover:
	$(call pd_mutant,rtk-nover,s/if (minv > ANE_H15_RTK_VERSION || maxv < ANE_H15_RTK_VERSION) {/if (0 \/* NEG: version check skipped *\/) {/,NEG: version check skipped,neg-rtkit)

# no cap on the management messages sent
sim-mutation-rtk-nocap:
	$(call pd_mutant,rtk-nocap,s/if (k->sent >= ANE_H15_RTK_MAXSEND) {/if (0 \/* NEG: send cap removed *\/) {/,NEG: send cap removed,neg-rtkit)

# the send registers are written while A2I reads FULL
sim-mutation-rtk-nofull:
	$(call pd_mutant,rtk-nofull,s/if (a2i \& ANE_H15_MBOX_FULL) {/if (0 \/* NEG: FULL ignored *\/) {/,NEG: FULL ignored,neg-rtkit)

# the shutdown handshake is skipped
sim-mutation-rtk-noshutdown:
	$(call pd_mutant,rtk-noshutdown,s/if (!k->ap_on)/if (1 \/* NEG: shutdown skipped *\/)/,NEG: shutdown skipped,pos)

# messages of other endpoints are treated as management messages (the crash log request gets answered)
sim-mutation-rtk-anyep:
	$(call pd_mutant,rtk-anyep,s/if (m1 \& 0xff)/if (0 \/* NEG: any endpoint answered *\/)/,NEG: any endpoint answered,pos)

# Stage 7 (confirm_chan=1): the SCRATCH handshake and the channel step. neg-chan = refusals, odd firmware, stuck core.
sim-negative-chan: sim/sim_h15
	./sim/sim_h15 neg-chan

# stage 7 runs without confirm_chan=1
sim-mutation-chan-noconfirm:
	$(call pd_mutant,chan-noconfirm,s/if (stg == 7 \&\& !confirm_chan) {/if (0 \/* NEG: confirm_chan ignored *\/) {/,NEG: confirm_chan ignored,neg-chan)

# the handshake goes on although the firmware does not show READY
sim-mutation-chan-noready:
	$(call pd_mutant,chan-noready,s/if (w\[7\] != ANE_H15_HS_READY) {/if (0 \/* NEG: READY not required *\/) {/,NEG: READY not required,neg-chan)

# the request band of SCRATCH0 is not checked
sim-mutation-chan-nolimit:
	$(call pd_mutant,chan-nolimit,s/if (w\[0\] >= ANE_H15_HS_REQ_LIMIT) {/if (0 \/* NEG: request limit skipped *\/) {/,NEG: request limit skipped,neg-chan)

# the firmware-supplied surface sizes are not bounded before the allocation
sim-mutation-chan-nosizes:
	$(call pd_mutant,chan-nosizes,s/if (ipc_size > ANE_H15_HS_IPC_MAX || heap_size > ANE_H15_HS_HEAP_MAX) {/if (0 \/* NEG: size limits skipped *\/) {/,NEG: size limits skipped,neg-chan)

# two rings of the table may overlap
sim-mutation-chan-noringoverlap:
	$(call pd_mutant,chan-noringoverlap,s/if (lo < rings\[j\].iova + (u64)rings\[j\].slots \* ANE_H15_CH_SLOT \&\& hi > rings\[j\].iova) {/if (0 \/* NEG: ring overlap ignored *\/) {/,NEG: ring overlap ignored,neg-chan)

# the host-to-firmware slots are not made host owned before READY
sim-mutation-chan-noownership:
	$(call pd_mutant,chan-noownership,s/if (rings\[i\].type)/if (1 \/* NEG: ownership skipped *\/)/,NEG: ownership skipped,pos)

sim-negative-nopsguard:
	rm -rf /tmp/ane_h15-nopsguard && mkdir -p /tmp/ane_h15-nopsguard
	cp -r sim /tmp/ane_h15-nopsguard/sim
	cp ane_h15_main.c ane_h15_soc.c ane_h15*.h /tmp/ane_h15-nopsguard/
	sed 's/ret = ane_h15_ps_wait(ane);/ret = 0; \/* NEG: PS guard skipped *\//' ane_h15_main.c > /tmp/ane_h15-nopsguard/ane_h15_main.c
	grep -q 'NEG: PS guard skipped' /tmp/ane_h15-nopsguard/ane_h15_main.c
	$(SIM_CC) $(SIM_CFLAGS) -I /tmp/ane_h15-nopsguard/sim -I /tmp/ane_h15-nopsguard/sim/inc -o /tmp/ane_h15-nopsguard/sim_h15 /tmp/ane_h15-nopsguard/sim/sim_harness.c $(SIM_SHA)
	@/tmp/ane_h15-nopsguard/sim_h15 neg-ps-stuck > /tmp/ane_h15-nopsguard/run.log 2>&1; \
	s=$$?; \
	echo "--- nopsguard negative-control tail ---"; tail -6 /tmp/ane_h15-nopsguard/run.log; \
	if [ $$s -eq 0 ]; then echo "SIM-NEGNOPSGUARD: guard-skipped build PASSED the harness (BAD: harness does not discriminate)"; exit 1; \
	else echo "SIM-NEGNOPSGUARD OK: guard-skipped build FAILS the harness (guard class caught)"; fi

# Mutation control for the window-release fix: the unmap calls deleted
# from the real module MUST fail the harness (pos mode carries the
# balance checks). Exit 0 iff the harness catches the leak.
sim-mutation-nounmap:
	rm -rf /tmp/ane_h15-nounmap && mkdir -p /tmp/ane_h15-nounmap
	cp -r sim /tmp/ane_h15-nounmap/sim
	cp ane_h15_main.c ane_h15_soc.c ane_h15*.h /tmp/ane_h15-nounmap/
	sed 's/ane_h15_unmap_windows(ane);/\/* NEG: window unmap deleted *\//' ane_h15_main.c > /tmp/ane_h15-nounmap/ane_h15_main.c
	grep -q 'NEG: window unmap deleted' /tmp/ane_h15-nounmap/ane_h15_main.c
	$(SIM_CC) $(SIM_CFLAGS) -I /tmp/ane_h15-nounmap/sim -I /tmp/ane_h15-nounmap/sim/inc -o /tmp/ane_h15-nounmap/sim_h15 /tmp/ane_h15-nounmap/sim/sim_harness.c $(SIM_SHA)
	@/tmp/ane_h15-nounmap/sim_h15 pos > /tmp/ane_h15-nounmap/run.log 2>&1; \
	s=$$?; \
	echo "--- nounmap mutant harness tail ---"; tail -4 /tmp/ane_h15-nounmap/run.log; \
	if [ $$s -eq 0 ]; then echo "SIM-MUT-NOUNMAP BAD: unmap-deleted build PASSED the harness (harness does not discriminate)"; exit 1; \
	else echo "SIM-MUT-NOUNMAP OK: unmap-deleted build FAILS the harness (leak class caught)"; fi

# Mutation control for P7: the filled-table fall-through refusal
# deleted (return -ENOSYS -> return 0) MUST fail the filled probe.
sim-mutation-nofillrefusal: sim-probe-filled
	rm -rf /tmp/ane_h15-nofillref && mkdir -p /tmp/ane_h15-nofillref
	cp -r sim /tmp/ane_h15-nofillref/sim
	cp ane_h15_main.c ane_h15_soc.c ane_h15*.h /tmp/ane_h15-nofillref/
	sed 's/return -ENOSYS;/return 0; \/* NEG: filled-table fall-through deleted *\//' ane_h15_main.c > /tmp/ane_h15-nofillref/ane_h15_main.c
	grep -q 'NEG: filled-table fall-through deleted' /tmp/ane_h15-nofillref/ane_h15_main.c
	$(SIM_CC) $(SIM_CFLAGS) -DANE15_SIM_FILL_FACTS -I /tmp/ane_h15-nofillref/sim -I /tmp/ane_h15-nofillref/sim/inc -o /tmp/ane_h15-nofillref/sim_h15_filled /tmp/ane_h15-nofillref/sim/sim_harness.c $(SIM_SHA)
	@H15_FW_INPUT_DIR="$(H15_FW_INPUT_DIR)" /tmp/ane_h15-nofillref/sim_h15_filled pos-filled > /tmp/ane_h15-nofillref/run.log 2>&1; \
	s=$$?; \
	echo "--- nofillrefusal mutant harness tail ---"; tail -4 /tmp/ane_h15-nofillref/run.log; \
	if [ $$s -eq 0 ]; then echo "SIM-MUT-NOFILLREF BAD: fall-through-deleted build PASSED P7 (harness does not discriminate)"; exit 1; \
	else echo "SIM-MUT-NOFILLREF OK: fall-through-deleted build FAILS P7 (gate class caught)"; fi

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f ane_h15_adt_test test_domain_window test_rtk_startup test_t8122_start sim/sim_h15 sim/sim_h15_filled

.PHONY: default modules check sim-test sim-negative-ps-stuck \
	sim-negative-wrong-stage sim-negative-truncated-dt \
	sim-negative-rpm-fail sim-negative-nopsguard \
	sim-probe-filled sim-mutation-nounmap \
	sim-negative-pd-attach-fail sim-negative-pd-link-fail \
	sim-negative-pd-rpm-err sim-negative-pd-eacces-alone \
	sim-mutation-pd-nolinkdetach sim-mutation-pd-anyerrignored \
	sim-mutation-pd-noremovedetach sim-mutation-pd-noidleleak \
	sim-negative-read-ps-stuck sim-negative-read-forbidden sim-negative-read-window \
	sim-mutation-read-nowindow \
	sim-mutation-read-noguard sim-mutation-read-nocheck \
	sim-mutation-read-noconfirm sim-mutation-nofillrefusal \
	sim-negative-run sim-mutation-run-noconfirm sim-mutation-run-norvbar \
	sim-mutation-run-nomboxcheck sim-mutation-run-noclear \
	sim-mutation-run-nostopcheck sim-mutation-run-pop32 \
	sim-negative-dart sim-mutation-run-nodartgate sim-mutation-dart-nottbr \
	sim-mutation-dart-noconfirm sim-mutation-run-nopin sim-negative-map sim-mutation-map-noundo \
	sim-mutation-map-noramcheck sim-mutation-map-textrw sim-mutation-map-nooverlap \
	sim-mutation-map-noiommus sim-mutation-map-nostoppedcheck sim-negative-rtkit sim-mutation-rtk-noconfirm sim-mutation-rtk-nover \
	sim-mutation-rtk-nocap sim-mutation-rtk-nofull sim-mutation-rtk-noshutdown sim-mutation-rtk-anyep sim-negative-chan sim-mutation-chan-noconfirm \
	sim-mutation-chan-noready sim-mutation-chan-nolimit sim-mutation-chan-nosizes \
	sim-mutation-chan-noringoverlap sim-mutation-chan-noownership clean
endif
