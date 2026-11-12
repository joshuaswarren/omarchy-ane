# SPDX-License-Identifier: GPL-2.0-only OR MIT
# OPT-IN EXPERIMENTAL module for the H18 ANE0 of the T8152 (Mac mini M6). Not part of "make all", not autoloaded.
# Build against the M6 kernel tree on a build host (not the lab CT), by hand:
#   make KERNELDIR=<kernel tree or its out dir> ARCH=arm64 LLVM=1 modules
# The userspace sim needs only a C compiler:
#   make sim-test                    positive controls
#   make sim-negative-psd-never      sim-negative-psd-wrong   sim-negative-dt   sim-negative-args
#   make sim-mutation-noconfirm      sim-mutation-nowindow    sim-mutation-noundo   sim-mutation-noack   sim-mutation-nogate
#   make sim-all                     everything above
ifneq ($(KERNELRELEASE),)
	obj-m := ane_h18.o
	ane_h18-objs := ane_h18_main.o ane_h18_soc.o ane_h18_dt.o
	ccflags-y += -I$(src)
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

SIM_CC ?= cc
SIM_CFLAGS ?= -g -O0 -Wall -Wextra -Werror=return-type -std=gnu11 -pthread

default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

sim/sim_h18: sim/sim_harness.c sim/shim.h ane_h18_main.c ane_h18_soc.c ane_h18.h ane_h18_dt.h
	$(SIM_CC) $(SIM_CFLAGS) -I sim -I sim/inc -o $@ sim/sim_harness.c

sim/sim_dt_pmp: sim/test_dt_pmp.c ane_h18_dt.c ane_h18_dt.h ane_h18_dt_t8132_data.h
	$(SIM_CC) $(SIM_CFLAGS) -I sim -o $@ sim/test_dt_pmp.c

sim-dt-pmp: sim/sim_dt_pmp
	./sim/sim_dt_pmp

sim/sim_dt_lifecycle: sim/test_dt_lifecycle.c ane_h18_dt.c ane_h18_dt.h ane_h18_dt_t8132_data.h
	$(SIM_CC) $(SIM_CFLAGS) -I sim -o $@ sim/test_dt_lifecycle.c

sim-dt-lifecycle: sim/sim_dt_lifecycle
	@timeout 30s ./sim/sim_dt_lifecycle

sim-test: sim/sim_h18
	./sim/sim_h18 pos

sim-negative-psd-never: sim/sim_h18
	./sim/sim_h18 neg-psd-never

sim-negative-psd-wrong: sim/sim_h18
	./sim/sim_h18 neg-psd-wrong

sim-negative-dt: sim/sim_h18
	./sim/sim_h18 neg-dt

sim-negative-args: sim/sim_h18
	./sim/sim_h18 neg-args

sim-negative-boot: sim/sim_h18
	./sim/sim_h18 neg-boot

sim-positive-reads: sim/sim_h18
	./sim/sim_h18 pos-reads

sim-negative-reads: sim/sim_h18
	./sim/sim_h18 neg-reads

# stage=reads: the pwrstate identity of the power-domains cells is not checked (a shifted base dtb would power a foreign domain)
sim-mutation-nopwrstateid:
	$(call mutant,nopwrstateid,s/of_device_is_compatible(pd_np$(COMMA) "apple,t6050-pmgr-pwrstate") ||/1 \/* NEG: pwrstate identity not checked *\/ ||/,NEG: pwrstate identity not checked,neg-reads)

# the power-domains phandle targets are never resolved
sim-mutation-novotecheck:
	$(call mutant,novotecheck,s/pd_np = of_find_node_by_phandle(ph);/pd_np = NULL; \/* NEG: target never resolved *\//,NEG: target never resolved,neg-reads)

# the multi-domain claims are skipped and a device get_sync fakes the vote
sim-mutation-nopdattach:
	$(call mutant,nopdattach,s/if (ane->pd_count > 1) {/if (0) { \/* NEG: claims never attached *\//,NEG: claims never attached,pos-reads)

# Mutation controls: build a copy of the real module with one sed edit and REQUIRE the named harness mode to fail it.
# $(call mutant,<name>,<sed script>,<marker>,<harness mode>)
COMMA := ,
define mutant
	rm -rf /tmp/ane_h18-$(1) && mkdir -p /tmp/ane_h18-$(1)
	cp -r sim /tmp/ane_h18-$(1)/sim
	cp ane_h18_main.c ane_h18_soc.c ane_h18.h ane_h18_dt.h /tmp/ane_h18-$(1)/
	sed '$(2)' ane_h18_main.c > /tmp/ane_h18-$(1)/ane_h18_main.c
	grep -q '$(3)' /tmp/ane_h18-$(1)/ane_h18_main.c
	$(SIM_CC) $(SIM_CFLAGS) -I /tmp/ane_h18-$(1)/sim -I /tmp/ane_h18-$(1)/sim/inc -o /tmp/ane_h18-$(1)/sim_h18 /tmp/ane_h18-$(1)/sim/sim_harness.c
	@/tmp/ane_h18-$(1)/sim_h18 $(4) > /tmp/ane_h18-$(1)/run.log 2>&1; \
	s=$$?; \
	echo "--- $(1) mutant harness tail ---"; tail -3 /tmp/ane_h18-$(1)/run.log; \
	if [ $$s -eq 0 ]; then echo "SIM-MUT-$(1) BAD: mutant PASSED the harness (the harness does not discriminate)"; exit 1; \
	else echo "SIM-MUT-$(1) OK: mutant FAILS the harness"; fi
endef

# stage=power runs without confirm_power=1
sim-mutation-noconfirm:
	$(call mutant,noconfirm,s/if (!confirm_power) {/if (0 \/* NEG: confirm_power ignored *\/) {/,NEG: confirm_power ignored,pos)

# the window check against the SoC row is skipped
sim-mutation-nowindow:
	$(call mutant,nowindow,s/if (res.start != pa || resource_size(\&res) != size) {/if (0 \/* NEG: window check skipped *\/) {/,NEG: window check skipped,neg-dt)

# a failed request does not put the acknowledged slots back to state 6
sim-mutation-noundo:
	$(call mutant,noundo,/^static int ane_h18_psd_up/$(COMMA)/^}/ s/ane_h18_psd_off(ane);/\/* NEG: no undo *\//,NEG: no undo,neg-psd-never)

# the acknowledged state is not compared with the requested one
sim-mutation-noack:
	$(call mutant,noack,s/if ((status \& ANE_H18_PSD_STATE_MASK) == state) {/if (1 \/* NEG: state not compared *\/) {/,NEG: state not compared,neg-psd-wrong)

# the request returns right after the write, without waiting for the acknowledge
sim-mutation-nogate:
	$(call mutant,nogate,s/writel(state$(COMMA) ane->psd\[i\] + ANE_H18_PSD_REQUEST);/writel(state$(COMMA) ane->psd[i] + ANE_H18_PSD_REQUEST); return 0; \/* NEG: no wait *\//,NEG: no wait,neg-psd-never)

# stage=boot runs without confirm_boot=1
sim-mutation-noconfirmboot:
	$(call mutant,noconfirmboot,s/if (!confirm_power || !confirm_boot) {/if (!confirm_power) {/,if (!confirm_power) {,neg-boot)

# stage=boot starts the core without comparing IORVBAR with the value iBoot leaves
NOPRE_SED := s/!= s->rvbar_expect) {/!= s->rvbar_expect \&\& 0 \/* NEG: IORVBAR not compared *\/) {/
sim-mutation-nopreflight:
	$(call mutant,nopreflight,$(NOPRE_SED),NEG: IORVBAR not compared,neg-boot)

# stage=boot never clears RUN again
sim-mutation-nohalt:
	$(call mutant,nohalt,s/writel(ctrl \& ~ANE_H18_CPU_RUN, ane->asc + ANE_H18_ASC_CPU_CONTROL);/\/* NEG: RUN never cleared *\//,NEG: RUN never cleared,pos)

# the RTKit reply is written without looking at the inbox credits
sim-mutation-nocredit:
	$(call mutant,nocredit,s/if (!(st \& 0xf00))/if (0 \/* NEG: credit ignored *\/)/,NEG: credit ignored,pos)

# the reply is written to RX, the half the firmware's message came from
sim-mutation-wronghalf:
	$(call mutant,wronghalf,s/ane_h18_po_store16(ane->mbox + chan_off + ANE_H18_PO_TX$(COMMA) r);/ane_h18_po_store16(ane->mbox + chan_off + ANE_H18_PO_RX$(COMMA) r); \/* NEG: wrong half *\//,NEG: wrong half,pos)

# a HELLO whose version range excludes 12 is answered anyway
sim-mutation-nover:
	$(call mutant,nover,s/if (minv > ANE_H18_RTK_VERSION || maxv < ANE_H18_RTK_VERSION) {/if (0 \/* NEG: version not checked *\/) {/,NEG: version not checked,pos)

# read_dart is allowed with one PSD slot
sim-mutation-dartgate:
	$(call mutant,dartgate,s/if (read_dart \&\& !ane->kpsd \&\& psd_slots != ANE_H18_NPSD) {/if (0 \/* NEG: DART gate off *\/) {/,NEG: DART gate off,neg-boot)

# rtkit_hello is allowed without alias_mbox
sim-mutation-hellogate:
	$(call mutant,hellogate,s/if (rtkit_hello \&\& !alias_mbox) {/if (0 \/* NEG: hello gate off *\/) {/,NEG: hello gate off,neg-boot)

# the firmware is mapped without the kernel-powered node (map_fw allowed with the module's own PSD requests)
sim-mutation-mapgate:
	$(call mutant,mapgate,s/if (map_fw \&\& !ane->kpsd) {/if (0 \/* NEG: map gate off *\/) {/,NEG: map gate off,neg-boot)

# the RUN write comes before the firmware map (the maps move behind the RUN comment)
sim-mutation-nofail:
	$(call mutant,nofail,s/ane_h18_result(ane$(COMMA) "boot"$(COMMA) "FAIL"$(COMMA) "fw-map-failed");/\/* NEG: map failure ignored *\/ ret = 0;/,NEG: map failure ignored,neg-boot)

# a failed second map leaves the first mapped, the domain attached
sim-mutation-noundomap:
	$(call mutant,noundomap,s/for (i = 0; i < nmapped; i++)/for (i = 0; i < 0 \&\& nmapped; i++) \/* NEG: no unmap *\//,NEG: no unmap,neg-boot)

# the runtime PM reference is never released after a clean stop
sim-mutation-norpmput:
	$(call mutant,norpmput,s/pm_runtime_put(ane->dev);/\/* NEG: no put *\//,NEG: no put,pos)

# the shutdown handshake is skipped
sim-mutation-nostop:
	$(call mutant,nostop,s/if (rtkit_stop \&\& ane->rtk_ap_on) {/if (0 \/* NEG: no shutdown *\/) {/,NEG: no shutdown,pos)

# AP power ON is sent for any IOP power acknowledgement, whatever its state
sim-mutation-noackstate:
	$(call mutant,noackstate,s/!ane->rtk_ap_sent \&\& (msg0 \& 0xff) == ANE_H18_RTK_PWR_ON/!ane->rtk_ap_sent \&\& 1 \/* NEG: ack state ignored *\//,NEG: ack state ignored,pos)

# a state 6 request that is not acknowledged does not fail the stage
sim-mutation-nostate6:
	$(call mutant,nostate6,s/if (!keep_on \&\& ane_h18_psd_off(ane)) {/if (!keep_on \&\& (ane_h18_psd_off(ane)$(COMMA) 0)) { \/* NEG: state 6 ack ignored *\//,NEG: state 6 ack ignored,neg-psd-never)

# the stage reports PASS although the shutdown was not acknowledged
sim-mutation-noshutfail:
	$(call mutant,noshutfail,s/if (rtkit_stop \&\& !(ane->rtk_ap_on \&\& ane->rtk_ap_acked \&\& ane->rtk_iop_acked)) {/if (0 \/* NEG: shutdown result ignored *\/) {/,NEG: shutdown result ignored,pos)

# a message that finds no free TX slot is dropped instead of queued
sim-mutation-noqueue:
	$(call mutant,noqueue,s/ane->rtk_txq\[ane->rtk_txq_n++\] = msg0;/(void)msg0; \/* NEG: message dropped *\//,NEG: message dropped,pos)

# the wake word is never written
sim-mutation-hsnowake:
	$(call mutant,hsnowake,s/ane_h18_scratch_write(ane, 7, ANE_H18_HS_WAKE);/(void)0; \/* NEG: no wake *\//,NEG: no wake,pos)

# the high half of the block address is not published
sim-mutation-hsnohigh:
	$(call mutant,hsnohigh,s/ane_h18_scratch_write(ane, 1, (u32)(pub >> 32));/ane_h18_scratch_write(ane, 1, 0); \/* NEG: high half lost *\//,NEG: high half lost,pos)

# the surfaces are never freed
sim-mutation-hsnofree:
	$(call mutant,hsnofree,s/^\tane_h18_hs_free(ane);\t\/\* only after.*/\t\/* NEG: surfaces leaked *\//,NEG: surfaces leaked,pos)

# the answer is judged by READY alone, which is there before the wake
sim-mutation-hsbys7:
	$(call mutant,hsbys7,s/if (table != pub \&\& cur\[2\] == ANE_H18_HS_READY) {/if (cur[2] == ANE_H18_HS_READY) { \/* NEG: judged by S7 *\//,NEG: judged by S7,pos)

# SCRATCH0 at 0x21 or above does not stop the handshake
sim-mutation-hsnoreqgate:
	$(call mutant,hsnoreqgate,s/if (w\[0\] >= ANE_H18_HS_REQ_LIMIT) {/if (0 \/* NEG: request gate off *\/) {/,NEG: request gate off,pos)

# a huge firmware request is allocated
sim-mutation-hsnolimit:
	$(call mutant,hsnolimit,s/if (ipc_size > ANE_H18_HS_IPC_MAX || heap_size > ANE_H18_HS_HEAP_MAX) {/if (0 \/* NEG: size limits off *\/) {/,NEG: size limits off,pos)

# the segments go into a new domain instead of the default one
sim-mutation-hsowndom:
	$(call mutant,hsowndom,s/dom = iommu_get_domain_for_dev(ane->dev);/dom = iommu_paging_domain_alloc(ane->dev); \/* NEG: own domain *\//,NEG: own domain,pos)

# the DMA mask is too small for the DART aperture
sim-mutation-hsmask:
	$(call mutant,hsmask,s/DMA_BIT_MASK(42)/DMA_BIT_MASK(32) \/* NEG: 32 bit mask *\//,NEG: 32 bit mask,pos)

# the H2T slots are not handed to the host before the first command
sim-mutation-chnoinit:
	$(call mutant,chnoinit,s/e\[0\] = 1;/e[0] = 0; \/* NEG: slots left firmware owned *\//,NEG: slots left firmware owned,pos)

# READY is never written to SCRATCH3
sim-mutation-chnos3:
	$(call mutant,chnos3,s/ane_h18_scratch_write(ane, 3, ANE_H18_HS_READY);/(void)0; \/* NEG: no SCRATCH3 *\//,NEG: no SCRATCH3,pos)

# a ring outside the table surface is accepted
CHNOVALID_SED := s/ring - tb->iova >= tb->size || size \* ANE_H18_CH_SLOT > tb->size - (ring - tb->iova)/0 \/* NEG: ring bound off *\//
sim-mutation-chnovalid:
	$(call mutant,chnovalid,$(CHNOVALID_SED),NEG: ring bound off,pos)

# the ring bound is the old form that wraps past 2^64
CHNOWRAP_SED := s/ring - tb->iova >= tb->size || size \* ANE_H18_CH_SLOT > tb->size - (ring - tb->iova)/ring + size * ANE_H18_CH_SLOT > tb->iova + tb->size \/* NEG: wrapping bound *\//
sim-mutation-chnowrap:
	$(call mutant,chnowrap,$(CHNOWRAP_SED),NEG: wrapping bound,pos)

# a SCRATCH3 that stays READY is accepted
sim-mutation-chs3skip:
	$(call mutant,chs3skip,s/^\tif (s3)$$/\tif (0 \/* NEG: S3 not checked *\/)/,NEG: S3 not checked,pos)

# a failing acknowledgement is accepted
sim-mutation-chnoack:
	$(call mutant,chnoack,s/return ack ? -EREMOTEIO : 0;/return 0; \/* NEG: ack ignored *\//,NEG: ack ignored,pos)

# the doorbell is never rung
sim-mutation-chnobell:
	$(call mutant,chnobell,s/writel(BIT(io->bit), ane->doorbell);/(void)0; \/* NEG: no doorbell *\//,NEG: no doorbell,pos)

# a ring that overlaps the table entries or another ring is accepted
sim-mutation-chnoverlap:
	$(call mutant,chnoverlap,s/if (lo < table + (u64)ANE_H18_CH_COUNT \* ANE_H18_CH_ENTRY \&\& hi > table) {/if (0 \/* NEG: overlap check off *\/) {/,NEG: overlap check off,pos)

# the snapshot after SCRATCH3 is not taken
sim-mutation-chnosnap:
	$(call mutant,chnosnap,s/ane_h18_chan_snapshot(ane, \"after SCRATCH3\", true);/(void)0; \/* NEG: no snapshot *\//,NEG: no snapshot,pos)

# the snapshot prints all-zero lines of the firmware rings
sim-mutation-chsnapw:
	$(call mutant,chsnapw,s/if (!(a | b))/if (0 \/* NEG: zero lines printed *\/)/,NEG: zero lines printed,pos)

# the SHAREDMALLOC ring is never served
sim-mutation-chnoserve:
	$(call mutant,chnoserve,s/if (!ane->mal_ring)/if (1 \/* NEG: ring not served *\/)/,NEG: ring not served,pos)

# a firmware FREE is not answered
sim-mutation-chnofreeans:
	$(call mutant,chnofreeans,s/WRITE_ONCE(slot\[0\], 1);/(void)0; \/* NEG: FREE not answered *\//,NEG: FREE not answered,pos)

# a pending IPI is never acknowledged
sim-mutation-chnoipiack:
	$(call mutant,chnoipiack,s/writel(pend, ane->ipi_ack);/(void)0; \/* NEG: no ack *\//,NEG: no ack,pos)

# the terminal ring text is not printed
sim-mutation-chnotext:
	$(call mutant,chnotext,s/if (any \&\& shown++ < 32)/if (0 \/* NEG: no terminal text *\/)/,NEG: no terminal text,pos)

# the program file is accepted without its checks
sim-mutation-pgnoparse:
	$(call mutant,pgnoparse,s/ret = ane_h18_prog_parse/ret = 0 \&\& \/* NEG: parse skipped *\/ ane_h18_prog_parse/,NEG: parse skipped,pos)

# a section may run past the end of the file
sim-mutation-pgnobounds:
	$(call mutant,pgnobounds,s/ size > n - off/ 0 \/* NEG: no section end check *\//,NEG: no section end check,pos)

# hs_prog without hs_start is not refused
sim-mutation-pgnostartgate:
	$(call mutant,pgnostartgate,s/ || (hs_prog \&\& !hs_start)/ \/* NEG: no hs_start gate for hs_prog *\//,NEG: no hs_start gate for hs_prog,pos)

# the command block stays 0x100 bytes with hs_prog
sim-mutation-pgblock:
	$(call mutant,pgblock,s/hs_prog ? ANE_H18_PROG_BLOCK : ANE_H18_CMD_BLOCK/ANE_H18_CMD_BLOCK \/* NEG: small block *\//,NEG: small block,pos)

# the call cookie is not at +0x20
sim-mutation-pgcookie:
	$(call mutant,pgcookie,s/memcpy(cmd + 0x20, \&cookie, 8);/memcpy(cmd + 0x28, \&cookie, 8); \/* NEG: cookie at +0x28 *\//,NEG: cookie at +0x28,pos)

# the IOVA of a section or buffer is not at +0x18 of its record
sim-mutation-pgiovaoff:
	$(call mutant,pgiovaoff,s/memcpy(r + 0x18, \&iova, 8);/memcpy(r + 0x10, \&iova, 8); \/* NEG: iova at +0x10 *\//,NEG: iova at +0x10,pos)

# a section record sits at the slot of its index, not of its id
sim-mutation-pgslot:
	$(call mutant,pgslot,s/(size_t)(p->sec\[i\].id - 1) \* 0x30/(size_t)i * 0x30 \/* NEG: slot = index *\//,NEG: slot = index,pos)

# every buffer is announced as an input
sim-mutation-pgouttype:
	$(call mutant,pgouttype,s/type = p->io\[i\].dir == 1 ? 1 : 0;/type = 0; \/* NEG: every buffer is type 0 *\//,NEG: every buffer is type 0,pos)

# the section bytes are not copied into their surfaces
sim-mutation-pgnosecdata:
	$(call mutant,pgnosecdata,s/memcpy(b->va, fw + p->sec\[i\].off, p->sec\[i\].size);/(void)fw; \/* NEG: no section bytes *\//,NEG: no section bytes,pos)

# the input data of the file is not copied into its buffers
sim-mutation-pgnoindata:
	$(call mutant,pgnoindata,s/memcpy(b->va, fw + p->io\[i\].doff, p->io\[i\].dsize);/(void)fw; \/* NEG: no input data *\//,NEG: no input data,pos)

# an IO_T2H slot is not given back
sim-mutation-pgnogiveback:
	$(call mutant,pgnogiveback,s/WRITE_ONCE(slot\[0\], hdr | 1);/(void)hdr; \/* NEG: T2H slot not given back *\//,NEG: T2H slot not given back,pos)

# the IO_T2H doorbell is not rung
sim-mutation-pgnodoorbell:
	$(call mutant,pgnodoorbell,s/writel(BIT(r->bit), ane->doorbell);/(void)r; \/* NEG: no T2H doorbell *\//,NEG: no T2H doorbell,pos)

# a call that never finished is followed by an unload anyway
sim-mutation-pgunloadtimeout:
	$(call mutant,pgunloadtimeout,s/if (ret == -ETIMEDOUT || ret == -EBUSY)/if (0 \/* NEG: unload after a timeout *\/)/,NEG: unload after a timeout,pos)

# a never written IO_T2H slot counts as an event
sim-mutation-pgemptyslot:
	$(call mutant,pgemptyslot,s/if ((hdr \& 1) || !hdr)/if (hdr \& 1 \/* NEG: empty slot is an event *\/)/,NEG: empty slot is an event,pos)

# a slot with a length and no address yet counts as published
sim-mutation-pgearlylen:
	$(call mutant,pgearlylen,s/ || !hdr/ || \/* NEG: length counts as published *\/ !(hdr | READ_ONCE(slot[1]))/,NEG: length counts as published,pos)

# no DART exception dump after the call
sim-mutation-pgnodartpost:
	$(call mutant,pgnodartpost,s/ane_h18_dart_exc(ane, \"after the call\");/(void)0; \/* NEG: no DART dump after the call *\//,NEG: no DART dump after the call,pos)

# the DART dump prints more than eight stream records
sim-mutation-pgdartcap:
	$(call mutant,pgdartcap,s/shown < ANE_H18_DART_EXC_SIDS; bit++/shown < ANE_H18_DART_EXC_SIDS + 8; bit++ \/* NEG: no cap *\//,NEG: no cap,pos)

# PREMAP_BUFFER is never sent
sim-mutation-pgnopremap:
	$(call mutant,pgnopremap,s/if (hs_premap) {/if (0 \/* NEG: no premap *\/) {/,NEG: no premap,pos)

# the PREMAP_BUFFER type is not at +0x10
sim-mutation-pgpremaptype:
	$(call mutant,pgpremaptype,s/memcpy(cmd + 0x10, \&type, 4);/memcpy(cmd + 0x14, \&type, 4); \/* NEG: type at +0x14 *\//,NEG: type at +0x14,pos)

# the DART instances 1 to 3 are not read
sim-mutation-pgnodartinst:
	$(call mutant,pgnodartinst,s/if (dart_inst \& BIT(k))/if (0 \/* NEG: no instance dump *\/)/,NEG: no instance dump,pos)

# the stream records of an instance are read four bytes off
sim-mutation-pgdartrec:
	$(call mutant,pgdartrec,s/0x8000 + 0x40 \* sids\[i\] + rec\[j\]/0x8000 + 0x40 * sids[i] + rec[j] + 4 \/* NEG: records at +4 *\//,NEG: records at +4,pos)

# a malformed PREMAP_BUFFER answer counts as a refusal
sim-mutation-pgpremapmal:
	$(call mutant,pgpremapmal,s/return -EPROTO;\t\/\* not the answer to this command \*\//return -EREMOTEIO;/,return -EREMOTEIO;,pos)

# a mask bit above instance 3 is accepted
sim-mutation-pgnomaskcheck:
	$(call mutant,pgnomaskcheck,s/ || (dart_inst \& ~(BIT(ANE_H18_DART_INSTANCES) - 1))/ \/* NEG: no mask check *\//,NEG: no mask check,pos)

# a change snapshot prints as many lines as an explicit one
sim-mutation-chsnaplim:
	$(call mutant,chsnaplim,s/(full ? 24 : 4)/24 \/* NEG: no change cap *\//,NEG: no change cap,pos)

# a change snapshot prints the terminal text
sim-mutation-chsnaptxt:
	$(call mutant,chsnaptxt,s/rings\[i\].type == 2 \&\& full/rings[i].type == 2 \&\& 1 \/* NEG: no full flag *\//,NEG: no full flag,pos)

# change snapshots are not capped per wait
sim-mutation-chnocap:
	$(call mutant,chnocap,s/if (ane->snap_left) {/if (1 \/* NEG: no cap *\/) {/,NEG: no cap,pos)

# the ack is written even when nothing is pending
sim-mutation-chackalways:
	$(call mutant,chackalways,s/	if (pend) {/	if (1 \/* NEG: ack always *\/) {/,NEG: ack always,pos)

# START and RESOURCE_INFO_GET are sent without hs_start
sim-mutation-chnostartgate:
	$(call mutant,chnostartgate,s/if (seq\[i\].start_only \&\& !hs_start)/if (0 \/* NEG: start commands always *\/)/,NEG: start commands always,pos)

sim-all: sim-test sim-positive-reads sim-negative-reads sim-dt-pmp sim-dt-lifecycle sim-negative-psd-never sim-negative-psd-wrong sim-negative-dt sim-negative-args sim-negative-boot \
	sim-mutation-nopwrstateid sim-mutation-novotecheck sim-mutation-nopdattach sim-mutation-noconfirm sim-mutation-nowindow sim-mutation-noundo sim-mutation-noack sim-mutation-nogate \
	sim-mutation-noconfirmboot sim-mutation-nopreflight sim-mutation-nohalt \
	sim-mutation-nocredit sim-mutation-wronghalf sim-mutation-nover sim-mutation-dartgate sim-mutation-hellogate \
	sim-mutation-mapgate sim-mutation-nofail sim-mutation-noundomap sim-mutation-norpmput \
	sim-mutation-nostop sim-mutation-noackstate \
	sim-mutation-nostate6 \
	sim-mutation-noshutfail \
	sim-mutation-noqueue \
	sim-mutation-hsnowake \
	sim-mutation-hsnohigh \
	sim-mutation-hsnofree \
	sim-mutation-hsbys7 \
	sim-mutation-hsnoreqgate \
	sim-mutation-hsnolimit \
	sim-mutation-hsowndom \
	sim-mutation-hsmask \
	sim-mutation-chnoinit \
	sim-mutation-chnos3 \
	sim-mutation-chnovalid \
	sim-mutation-chs3skip \
	sim-mutation-chnoack \
	sim-mutation-chnobell \
	sim-mutation-chnoverlap \
	sim-mutation-chnowrap \
	sim-mutation-chnosnap \
	sim-mutation-chsnaplim \
	sim-mutation-pgnopremap \
	sim-mutation-pgpremapmal \
	sim-mutation-pgpremaptype \
	sim-mutation-pgnodartinst \
	sim-mutation-pgdartrec \
	sim-mutation-pgnomaskcheck \
	sim-mutation-pgnodartpost \
	sim-mutation-pgdartcap \
	sim-mutation-pgnoparse \
	sim-mutation-pgnobounds \
	sim-mutation-pgnostartgate \
	sim-mutation-pgblock \
	sim-mutation-pgcookie \
	sim-mutation-pgiovaoff \
	sim-mutation-pgslot \
	sim-mutation-pgouttype \
	sim-mutation-pgnosecdata \
	sim-mutation-pgnoindata \
	sim-mutation-pgnogiveback \
	sim-mutation-pgnodoorbell \
	sim-mutation-pgunloadtimeout \
	sim-mutation-pgemptyslot \
	sim-mutation-pgearlylen \
	sim-mutation-chsnaptxt \
	sim-mutation-chsnapw \
	sim-mutation-chnoserve \
	sim-mutation-chnofreeans \
	sim-mutation-chnoipiack \
	sim-mutation-chnotext \
	sim-mutation-chnocap \
	sim-mutation-chackalways \
	sim-mutation-chnostartgate

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f sim/sim_h18 sim/sim_dt_pmp sim/sim_dt_lifecycle

.PHONY: sim-dt-pmp sim-dt-lifecycle sim-positive-reads sim-negative-reads sim-mutation-nopwrstateid sim-mutation-novotecheck sim-mutation-nopdattach sim-mutation-pgpremapmal sim-mutation-pgnopremap sim-mutation-pgpremaptype sim-mutation-pgnodartinst sim-mutation-pgdartrec sim-mutation-pgnomaskcheck sim-mutation-pgnodartpost sim-mutation-pgdartcap sim-mutation-pgearlylen sim-mutation-pgnoparse sim-mutation-pgnobounds sim-mutation-pgnostartgate sim-mutation-pgblock sim-mutation-pgcookie sim-mutation-pgiovaoff sim-mutation-pgslot sim-mutation-pgouttype sim-mutation-pgnosecdata sim-mutation-pgnoindata sim-mutation-pgnogiveback sim-mutation-pgnodoorbell sim-mutation-pgunloadtimeout sim-mutation-pgemptyslot sim-mutation-chsnaplim sim-mutation-chsnaptxt sim-mutation-chnostartgate sim-mutation-chnocap sim-mutation-chackalways sim-mutation-chnotext sim-mutation-chnoserve sim-mutation-chnofreeans sim-mutation-chnoipiack sim-mutation-chnosnap sim-mutation-chsnapw sim-mutation-chnowrap sim-mutation-chnoverlap sim-mutation-chnoinit sim-mutation-chnos3 sim-mutation-chnovalid sim-mutation-chs3skip sim-mutation-chnoack sim-mutation-chnobell sim-mutation-hsnowake sim-mutation-hsnohigh sim-mutation-hsnofree sim-mutation-hsbys7 sim-mutation-hsnoreqgate sim-mutation-hsnolimit sim-mutation-hsowndom sim-mutation-hsmask sim-mutation-noqueue sim-mutation-noshutfail sim-mutation-nostate6 sim-mutation-nostop sim-mutation-noackstate sim-mutation-mapgate sim-mutation-nofail sim-mutation-noundomap sim-mutation-norpmput sim-mutation-nocredit sim-mutation-wronghalf sim-mutation-nover sim-mutation-dartgate sim-mutation-hellogate sim-negative-boot sim-mutation-noconfirmboot sim-mutation-nopreflight sim-mutation-nohalt default modules sim-test sim-negative-psd-never sim-negative-psd-wrong sim-negative-dt sim-negative-args \
	sim-mutation-noconfirm sim-mutation-nowindow sim-mutation-noundo sim-mutation-noack sim-mutation-nogate sim-all clean
endif
