/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h18: OPT-IN EXPERIMENTAL bring-up module for the H18 ANE0 of the T8152 (Mac mini M6, J873g).
 *
 * Stages dt, psd, power and boot ran on a Mac mini M6 (see README-bringup.md). The options alias_mbox, read_dart and rtkit_hello have not.
 * The firmware is preloaded by iBoot (the ADT /arm-io/ane segment-ranges name where) and iBoot programs IORVBAR. This module never writes
 * IORVBAR. Stage boot sets the core's RUN bit once, watches the status and the Postoffice outboxes for a short time, then clears RUN.
 * Only with rtkit_hello=1 does it write a mailbox: the RTKit management replies, to the TX half of the alias-bank channel.
 *
 * Stages (`stage` parameter):
 *   dt    : check the device tree windows against the SoC row and log them. No mapping, no MMIO.
 *   psd   : read-only. Map the two PSD slot windows (30 and 31) and read VALID and, when set, STATUS.
 *           The proven PSD probe does exactly these two reads. No write.
 *   power : needs confirm_power=1. Request PSD state `psd_state` (default 0, run) on the first `psd_slots`
 *           slots (default 1: slot 30 only), wait for each acknowledge as pmgr-psd.c does, then read
 *           CPU_STATUS, ASC_STATUS and IORVBAR (CPU_CONTROL with read_ctrl=1, the mailbox status words with read_mbox=1), then put the
 *           slots back to state 6 unless keep_on=1. Every write and read address is logged BEFORE the access.
 *
 *   boot  : needs confirm_power=1 and confirm_boot=1. Like power, then: refuse unless the core is stopped, CPU_CONTROL.RUN is clear and
 *           IORVBAR is exactly the value iBoot leaves (ane_h18_soc.c). Then write CPU_CONTROL |= RUN, watch CPU_STATUS and the status words
 *           of the three candidate Postoffice channels for boot_wait_ms, read at most 8 outbox messages (raw words are logged), write
 *           CPU_CONTROL &= ~RUN and wait for STOPPED. If the core does not stop, the PSD slots stay on and a reboot is required.
 *           Options: alias_mbox=1 (status of the alias-bank channel before RUN and on every poll), read_dart=1 (twelve read-only DART words
 *           before RUN, needs both slots), rtkit_hello=1 (answer the RTKit HELLO and endpoint map, needs alias_mbox=1).
 *
 * Nothing autoloads: there is no MODULE_DEVICE_TABLE, and probe returns -EPERM unless optin=t8152.
 *   insmod ane_h18.ko optin=t8152 stage=psd
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/pm_runtime.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/pm_domain.h>

#include <crypto/sha2.h>

#include "ane_h18.h"
#include "ane_h18_dt.h"
#include "ane_h18_fw_pins.h"
#include "ane_h18_fw_layout.h"

static char *optin;
module_param(optin, charp, 0444);
MODULE_PARM_DESC(optin, "Opt-in key: must be t8152 or t8132 or probe returns -EPERM.");

static char *stage = "dt";
module_param(stage, charp, 0444);
MODULE_PARM_DESC(stage, "dt (default, no MMIO), psd (read the PSD slots), power (request PSD state, needs confirm_power=1), boot, or reads (t8132 only: read-only register probe, needs allow_reads=1).");

static bool confirm_power;
module_param(confirm_power, bool, 0444);
MODULE_PARM_DESC(confirm_power, "Required for stage=power. Off by default.");

static unsigned int psd_state;
module_param(psd_state, uint, 0444);
MODULE_PARM_DESC(psd_state, "Stage=power: PSD state to request, 0 (run) to 5. Default 0.");

static unsigned int psd_slots = 1;
module_param(psd_slots, uint, 0444);
MODULE_PARM_DESC(psd_slots, "Stage=power: how many PSD slots to request, in order (30 then 31). Default 1.");

static bool keep_on;
module_param(keep_on, bool, 0444);
MODULE_PARM_DESC(keep_on, "Stage=power: leave the slots running after the reads. Default off: request state 6 again.");

static bool read_mbox;
module_param(read_mbox, bool, 0444);
MODULE_PARM_DESC(read_mbox, "Stage=power: also read the Postoffice status word of this ANE's channel.");

static bool confirm_boot;
module_param(confirm_boot, bool, 0444);
MODULE_PARM_DESC(confirm_boot, "Required for stage=boot, together with confirm_power=1. Off by default.");

static unsigned int boot_wait_ms = 300;
module_param(boot_wait_ms, uint, 0444);
MODULE_PARM_DESC(boot_wait_ms, "Stage=boot: how long the core runs before RUN is cleared again, ms (10 to 2000). Default 300.");

static bool read_dart;
module_param(read_dart, bool, 0444);
MODULE_PARM_DESC(read_dart, "Stage=boot: before RUN, read twelve words of the ANE DART (instance 0). Needs psd_slots=2. Read only. Default off.");

static bool alias_mbox;
module_param(alias_mbox, bool, 0444);
MODULE_PARM_DESC(alias_mbox, "Stage=boot: also watch the alias-bank Postoffice channel (window + 0x10000, channel 2 * mailbox number). Default off.");

static bool read_scratch;
module_param(read_scratch, bool, 0444);
MODULE_PARM_DESC(read_scratch, "Stage=boot: read the eight SCRATCH words of the engine block (engine + 0x18a0024) before RUN, when AP power ON is acknowledged and before the shutdown. Read only. Default off.");

static bool handshake;
module_param(handshake, bool, 0444);
MODULE_PARM_DESC(handshake, "Stage=boot with map_fw=1, read_scratch=1, rtkit_start=1 and rtkit_stop=1 or 2: after AP power ON is acknowledged, allocate the surfaces the shipped system allocates, publish a 0x174 byte init block in SCRATCH0 and SCRATCH1, write the wake word to SCRATCH7 and wait for the firmware's channel table address. The firmware segments are then mapped into the default DMA domain of the ANE instead of an own domain. Default off.");

static unsigned int hs_cfg_size = 0x380000;
module_param(hs_cfg_size, uint, 0444);
MODULE_PARM_DESC(hs_cfg_size, "Stage=boot with handshake=1: the config size stored at init block +0x10 (+0x18 is 0x10000000 minus it). 0x380000 is the value the shipped system sets for ane-type 0x310 (T6021 uses 0x500000). Default 0x380000.");

static unsigned int hs_wait_ms = 1000;
module_param(hs_wait_ms, uint, 0444);
MODULE_PARM_DESC(hs_wait_ms, "Stage=boot with handshake=1: how long to wait for the firmware's table address, ms, 10 to 5000. Default 1000.");

static bool hs_chan;
module_param(hs_chan, bool, 0444);
MODULE_PARM_DESC(hs_chan, "Stage=boot with handshake=1: after the firmware published its channel table, check the table, set the host-to-firmware ring slots to host owned, write READY to SCRATCH3 and wait for the firmware to clear it (macOS does this after it created its channels). Writes ring memory and SCRATCH3. Default off.");

static bool hs_cmds;
module_param(hs_cmds, bool, 0444);
MODULE_PARM_DESC(hs_cmds, "Stage=boot with hs_chan=1: send PRINT_ENABLE (0) and CONFIG_GET on the IO ring, one 64 byte slot each, doorbell BIT(bit) at engine + 0x18a4000, and log the replies. Writes the doorbell. Default off.");

static bool hs_start;
module_param(hs_start, bool, 0444);
MODULE_PARM_DESC(hs_start, "Stage=boot with hs_cmds=1: send the commands in macOS's order, PRINT_ENABLE, START, CONFIG_GET, RESOURCE_INFO_GET, and log the RESOURCE_INFO_GET reply. START starts the firmware's services and can bring more memory requests. Default off: PRINT_ENABLE and CONFIG_GET only.");

static unsigned int dart_inst;
module_param(dart_inst, uint, 0444);
MODULE_PARM_DESC(dart_inst, "hs_prog=1 with read_dart=1: a bit mask of ANE DART instances 0 to 3 whose ENABLE_STREAMS, TCR, TTBR, exception bitmap and the records of streams 0, 10, 11, 13 and 15 are read, read only, before LOAD_PROGRAM and after the call. Instances 1 to 3 are mapped here (ADT dart-ane reg[1..3]). Default 0.");

static unsigned int hs_props;
module_param(hs_props, uint, 0444);
MODULE_PARM_DESC(hs_props, "hs_start=1: send the five CH_PROPERTY_WRITE commands (0x1f) the shipped system sends after CONFIG_GET and RESOURCE_INFO_GET: 0x10a4=1, 0x10a6=1, 0xa1=0, 0x10aa=1, 0x1209=1, in the shipped system's order. A refused write is logged and the sequence goes on. The values of 0x10a6 and 0x1209 depend on shipped-system config bytes that are not known for the T8152. hs_props=2 sends 0xa1 = 1 instead of 0: the shipped system writes 0 for 'ProcedureCall Firmware log', 1 may turn the firmware's per call log on (not tested). Default 0 (off).");

static bool hs_defset;
module_param(hs_defset, bool, 0444);
MODULE_PARM_DESC(hs_defset, "hs_props=1 or 2: send DEFAULT_SETTING_SET (0x2e, 0x18 bytes: one setting, register id 2, value 0x2710) after the property writes, as the shipped system does. Default off.");

static bool hs_mcsz;
module_param(hs_mcsz, bool, 0444);
MODULE_PARM_DESC(hs_mcsz, "hs_start=1: send MCACHE_SIZE_GET (0x26, read only) after CONFIG_GET and log the size in the reply. Default off.");

static unsigned int hs_dyn;
module_param(hs_dyn, uint, 0444);
MODULE_PARM_DESC(hs_dyn, "hs_pmu=1: send SET_DYNAMIC_POWERGATE (0x2d) right after SET_SNE_PMU_BASE2: 1 = disable dynamic power gating (byte 0), 2 = enable it (byte 1). Default 0: not sent.");

static bool hs_pmu;
module_param(hs_pmu, bool, 0444);
MODULE_PARM_DESC(hs_pmu, "hs_start=1: send SET_SNE_PMU_BASE2 (0x29) with the value 0x300700000 after START and before CONFIG_GET, as the shipped system does for ANE types 0x300 and 0x310. Default off: the firmware uses the value as a bus address, so one value only.");

static bool hs_premap;
module_param(hs_premap, bool, 0444);
MODULE_PARM_DESC(hs_premap, "hs_prog=1: send PREMAP_BUFFER (0x20d, type 2) for one inference request pool before LOAD_PROGRAM, as the shipped system does at init. The reply is logged. Default off.");

static unsigned long long premap_second;
module_param(premap_second, ullong, 0444);
MODULE_PARM_DESC(premap_second, "hs_premap=1: the second address value of the command (+0x78). 0 sends the pool size, which is a guess: the value macOS passes comes from a virtual call that is not known.");

static bool hs_prog;
module_param(hs_prog, bool, 0444);
MODULE_PARM_DESC(hs_prog, "Stage=boot with hs_start=1: load the program file prog_fw (made by ane-prog-pack from an ANEC), LOAD_PROGRAM, CREATE_PROCESS and PROCEDURE_CALL on the IO ring with the T6021 wire format, wait for the IO_T2H events, then TERMINATE_PROCESS and DESTROY_PROGRAM. The file is checked and the buffers are allocated before the first command. Default off.");

static char *prog_fw = "ane_h18_prog.bin";
module_param(prog_fw, charp, 0444);
MODULE_PARM_DESC(prog_fw, "hs_prog=1: the program file name under /lib/firmware. Default ane_h18_prog.bin.");

static bool map_fw;
module_param(map_fw, bool, 0444);
MODULE_PARM_DESC(map_fw, "Stage=boot: before RUN, attach the ANE to its DART through the iommu API and map the two preloaded firmware segments at their IOVAs. Needs power-domains and iommus in the node. Default off.");

static unsigned int rtkit_stop;
module_param(rtkit_stop, uint, 0444);
MODULE_PARM_DESC(rtkit_stop, "Stage=boot with rtkit_start=1: before RUN is cleared, run the RTKit shutdown and wait for the acknowledgements. 1: AP power state QUIESCED then IOP power state SLEEP (apple_rtkit_shutdown, restartable). 2: OFF then OFF (apple_rtkit_poweroff, cannot be restarted). Default 0: none.");

static bool rtkit_start;
module_param(rtkit_start, bool, 0444);
MODULE_PARM_DESC(rtkit_start, "Stage=boot with rtkit_hello=1: also start the system endpoints after the last endpoint map message and answer the firmware's IOP power state acknowledgement with AP power state ON. Default off.");

static bool rtkit_hello;
module_param(rtkit_hello, bool, 0444);
MODULE_PARM_DESC(rtkit_hello, "Stage=boot with alias_mbox=1: answer the firmware's RTKit management HELLO (version 12) and the endpoint map, nothing else. Default off.");

static bool read_ctrl;
module_param(read_ctrl, bool, 0444);
MODULE_PARM_DESC(read_ctrl, "Stage=power: also read CPU_CONTROL (ASC + 0x44). Read only, never written. Default off.");

static unsigned int psd_timeout_ms = 1000;
module_param(psd_timeout_ms, uint, 0444);
MODULE_PARM_DESC(psd_timeout_ms, "Stage=power: per-slot acknowledge wait, ms. Default 1000 (the ADT psd-timeout-ms).");

static bool owned_fw;
module_param(owned_fw, bool, 0444);
MODULE_PARM_DESC(owned_fw, "Stage=boot with map_fw=1: stage the exact pinned 26A428 payload from fw_path into Linux-owned coherent memory and DART-map those pages at the fixed TEXT/DATA IOVAs instead of the iBoot DRAM segments. One attempt per module instance; a short unmap or a post-RUN failure retains the image and the module pin. No program submission. Default off.");

static char *fw_path;
module_param(fw_path, charp, 0444);
MODULE_PARM_DESC(fw_path, "owned_fw=1: firmware-loader file name without a path holding the exact pinned payload (1572864 bytes, sha256-pinned in ane_h18_fw_pins.h). Required for owned_fw=1; there is no fallback to the iBoot DRAM segments.");

static unsigned long long prog_iova_bufs;
module_param(prog_iova_bufs, ullong, 0444);
MODULE_PARM_DESC(prog_iova_bufs, "hs_prog: nonzero maps the program's input and output buffers, in file order, at consecutive fixed IOVAs from this base (16 KiB aligned, below 0xe0000000), as the compile-time IOVAs of the HWX (the add program: 0x30000000). Each buffer is allocated with the DMA API as before, and an alias is mapped at the fixed address and removed before the buffer is freed. Default 0: dynamic IOVAs.");

static unsigned long long prog_iova_text;
module_param(prog_iova_text, ullong, 0444);
MODULE_PARM_DESC(prog_iova_text, "hs_prog: nonzero maps program section 3 (the task stream, the HWX __TEXT) at this fixed IOVA in a 32 KiB region, the vmsize of the HWX __TEXT (the add program: 0x3000c000). Same alias mapping and unmap as prog_iova_bufs. Default 0: dynamic.");

static unsigned int prog_text_const;
module_param(prog_text_const, uint, 0444);
MODULE_PARM_DESC(prog_text_const, "hs_prog with prog_iova_text: copy the first 0x4000 bytes of program section 2 (the HWX __TEXT __const) into the section 3 region at this byte offset, where the task stream expects its constants (the matvec program: 0x280). 8 byte aligned; the region is 32 KiB. Default 0: not copied.");

static unsigned long long prog_iova_kern;
module_param(prog_iova_kern, ullong, 0444);
MODULE_PARM_DESC(prog_iova_kern, "hs_prog: nonzero copies program section 2 behind its first 0x4000 bytes (the HWX __KERN_0, the weights) into its own surface and maps it at this fixed IOVA, 16 KiB aligned, below 0xe0000000 (the matvec program: 0x30010000). Same alias mapping and unmap as prog_iova_bufs. Default 0: not mapped.");

static unsigned int hs_prog_dump;
module_param(hs_prog_dump, uint, 0444);
static unsigned int hs_settle_ms = 10;
module_param(hs_settle_ms, uint, 0444);
MODULE_PARM_DESC(hs_settle_ms, "hs_prog=1: milliseconds the driver waits after each unload command and between program passes, so the firmware can finish tearing the previous program down. Default 10.");
MODULE_PARM_DESC(hs_prog_dump, "hs_prog=1: hexdump the first N bytes of every program buffer after the call (16 bytes per line, capped at 0x1000). The first pass only. Default 0 (off).");

static unsigned int hs_prog_calls = 1;
module_param(hs_prog_calls, uint, 0444);
MODULE_PARM_DESC(hs_prog_calls, "hs_prog=1: run the whole program sequence this many times (LOAD_PROGRAM, CREATE_PROCESS, PROCEDURE_CALL, finish wait, TERMINATE_PROCESS, DESTROY_PROGRAM each pass; 1 to 1000). Per-call latency is logged as 'call i/N finished in X ms'. Default 1.");

static unsigned int prog_out_words;
module_param(prog_out_words, uint, 0444);
MODULE_PARM_DESC(prog_out_words, "hs_prog=1: dump the first N words of every output buffer after each call as 'out buf <id> +0x<off>: <16 words>', and log 'call i/N finished in X ns' while tight-polling for the finish event. X is the host poll loop's time from the doorbell write to the first iteration that saw the event: doorbell latency, the firmware's post path and the poll granularity are in it. It is NOT a hardware time. 0 (the default) keeps the sleeping poll and the millisecond log.");

enum { ANE_H18_STAGE_DT, ANE_H18_STAGE_PSD, ANE_H18_STAGE_POWER, ANE_H18_STAGE_BOOT, ANE_H18_STAGE_READS };

static bool allow_reads;
module_param(allow_reads, bool, 0444);
MODULE_PARM_DESC(allow_reads, "stage=reads only: permit the read-only register probe (IORVBAR, SCRATCH7 candidates, doorbell words) on an incomplete row. Read only; no writes, no clocks, no power requests. Default off.");

static ulong pmgr_gate_pa;
module_param(pmgr_gate_pa, ulong, 0444);
MODULE_PARM_DESC(pmgr_gate_pa, "stage=reads: optional extra gate-state register. When set (with pmgr_gate_mask) the module reads it back before every step and refuses unless all pmgr_gate_mask bits are set. 0 (the default) checks the four ANE pmgr ps registers only. The ANE gates (ADT ids 569/664) are no_ps and have no pmgr register of their own.");

static uint pmgr_gate_mask;
module_param(pmgr_gate_mask, uint, 0444);
MODULE_PARM_DESC(pmgr_gate_mask, "stage=reads: mask of pmgr_gate_pa bits that must read as set (on) before any engine-window read is allowed. Required alongside pmgr_gate_pa.");

/* An optional second extra gate register, checked the same way before every step. */
static ulong pmgr_gate2_pa;
module_param(pmgr_gate2_pa, ulong, 0444);
MODULE_PARM_DESC(pmgr_gate2_pa, "stage=reads: optional second extra gate-state register. Needs pmgr_gate_pa. Checked like pmgr_gate_pa when nonzero.");

static uint pmgr_gate2_mask;
module_param(pmgr_gate2_mask, uint, 0444);
MODULE_PARM_DESC(pmgr_gate2_mask, "stage=reads: mask bits that must read as set in the second pmgr gate register. Required alongside pmgr_gate2_pa.");

/* The four ANE pmgr power-state registers (t6050-pmgr.dtsi): ane_sys 0x3a8, ane_mpm 0x3c0,
 * ane_cpu 0x3c8, ane_tdbase 0x3d0 - all in the pmgr1 page (0x280900000). The ADT hangs the
 * ANE power-gates under ANE_CPU and ANE_SYS (569/664 parent 223/219), so the genpd claims are
 * not the last word: all four domains must read ON before any engine-window read. In the
 * pmgr power-state register the TARGET state is bits 3:0 and the ACTUAL state is bits 7:4
 * (pmgr-pwrstate.c); the actual nibble 0xf = fully on. */
#define ANE_H18_PS_SYS		0x3a8
#define ANE_H18_PS_MPM		0x3c0
#define ANE_H18_PS_CPU		0x3c8
#define ANE_H18_PS_TDBASE	0x3d0

static ulong pmgr_ps_pa;
module_param(pmgr_ps_pa, ulong, 0444);
MODULE_PARM_DESC(pmgr_ps_pa, "stage=reads: physical address of the pmgr1 page holding the four ANE power-state registers (0x280900000 on the t6050). Required, page-aligned.");

static uint pmgr_ps_mask;
module_param(pmgr_ps_mask, uint, 0444);
MODULE_PARM_DESC(pmgr_ps_mask, "stage=reads: mask the actual-state field (bits 7:4) of the four ANE power-state registers must read as set (0xf = fully on). Required alongside pmgr_ps_pa.");

/* The stage=reads step machine: one engine-window register per write of "next". */
struct ane_h18;
static struct ane_h18 *ane_h18_rd_dev;
static unsigned int ane_h18_rd_idx;
static struct dentry *ane_h18_rd_control;
static const struct file_operations ane_h18_rd_fops;
static const struct { const char *name; u32 off; } ane_h18_rd_list[] = {
	{ "IORVBAR_LO", 0x1050000 },
	{ "IORVBAR_HI", 0x1050004 },
	{ "SCRATCH7_kext", 0x1840084 },
	{ "SCRATCH7_m6practice", 0x18a003c },
	{ "SCRATCH0_m6practice", 0x18a0024 },
	{ "DOORBELL_a", 0x1844000 },
	{ "DOORBELL_b", 0x1884000 },
	/* Aurora T6050 cold-boot cells (ane_fw_cold.c snapshot, drivers/accel/ane): the scratch file
	 * first (the same 0x188xxxx block as the doorbell above), then the CPU-run/status pair and
	 * the compute-engine enable/state words in blocks not read before: a hang, if any, lands
	 * on the last steps. DART and pwgate cells are NOT here (separate power gate unmapped). */
	{ "AUR_SCRATCH0", 0x1880028 },
	{ "AUR_SCRATCH1", 0x188002c },
	{ "AUR_SCRATCH2", 0x1880030 },
	{ "AUR_SCRATCH3", 0x1880034 },
	{ "AUR_SCRATCH4", 0x1880038 },
	{ "AUR_SCRATCH5", 0x188003c },
	{ "AUR_SCRATCH6", 0x1880040 },
	{ "AUR_SCRATCH7", 0x1880044 },
	{ "AUR_CPU_RUN", 0x1600044 },
	{ "AUR_STATUS", 0x1600048 },
	{ "AUR_CE_ENABLE", 0x18a4008 },
	{ "AUR_CE_STATE", 0x18a4010 },
	{ "AUR_CE_ERROR", 0x18a4018 },
};

struct ane_h18 {
	struct device *dev;
	const struct ane_h18_soc *soc;
	void __iomem *psd[ANE_H18_NPSD];
	void __iomem *asc;
	void __iomem *rvbar;
	void __iomem *mbox;
	void __iomem *eng;	/* stage=reads: the whole engine window, reg index 0 */
	void __iomem *gate;	/* stage=reads: the pmgr gate-state register page */
	void __iomem *gate2;	/* stage=reads: optional second pmgr gate page (the other gate family) */
	void __iomem *ps;	/* stage=reads: the pmgr1 page with the four ANE power-state registers */
	void __iomem *dart;
	void __iomem *dartx[ANE_H18_DART_INSTANCES];	/* instances 1 to 3 with dart_inst, [0] unused (ane->dart is instance 0) */
	void __iomem *scratch;
	void __iomem *doorbell;
	void __iomem *ipi_pending;
	void __iomem *ipi_ack;
	struct iommu_domain *domain;
	bool dom_own;		/* the domain was allocated and attached here (map_fw without handshake); otherwise it is the default DMA domain */
	struct ane_h18_hs_buf {
		void *va;
		dma_addr_t iova;	/* the address the firmware is given: the fixed alias when `fixed`, else the DMA API address */
		dma_addr_t dma;		/* the address dma_alloc_coherent returned: what dma_free_coherent needs */
		size_t size;
		bool fixed;		/* an alias mapping at `iova` was added with iommu_map and is removed with iommu_unmap before the free */
	} hs_pool, hs_ipc, hs_heap, hs_cmd, hs_mal[ANE_H18_MAL_MAX_BUFS], owned_fw_img;
	/* the Linux-owned image (owned_fw=1): staged unmodified pinned payload, planned pages, the two fixed segment maps */
	u64 owned_pa[ANE_H18_FW_PAGES];		/* physical address of each 16 KiB image page, read from the default domain before any attach */
	u64 map_iova[2], map_size[2];		/* the fixed TEXT/DATA IOVAs and full sizes (owned pages or iBoot segments) */
	u64 map_len[2];				/* bytes actually mapped per segment: unmap uses these, never the full size blindly */
	unsigned int map_n;			/* segments whose whole extent is currently mapped */
	bool map_retained;			/* a short unmap kept the domain and the image: no retry, no removal, reboot required (the module is pinned) */
	bool module_pinned;			/* __module_get() taken once: a retained image or surface must not outlive the module code; released only by reboot */
	bool prog_cleaned;			/* the program stage unloaded the program or power-cycled the domain: the ANE holds no program, so nothing needs a reboot */
	bool domain_down;			/* the failpath recovery powered the domain down and has not brought it back: no register of the ANE may be touched */
	unsigned int *call_t2h;	/* prog_out_words: the IO_T2H cursor of the running call, for the finish-event peek that logs nothing */
	u64 call_t0, call_t1;	/* its ns stamps: the doorbell write, and the first poll iteration that saw the finish event */
	unsigned int hs_mal_n, mal_cursor, mal_slots, mal_bit;
	size_t hs_mal_bytes;
	unsigned int ack_logs;	/* ack writes logged so far: the address is logged for the first few, then silently */
	unsigned int snap_left;	/* change snapshots still allowed in the current wait */
	int mal_err;		/* the first refusal of a firmware request: later polls return it without logging again */
	bool hs_cmd_in_pool;	/* hs_cmd points into hs_pool: never freed on its own */
	u64 *mal_ring;		/* the SHAREDMALLOC ring of the published table, NULL until the channel step has checked the table */
	char ch_names[8][0x40];
	struct ane_h18_ring_copy {
		u64 *base;
		u64 iova;
		u32 slots, bit, type;
	} ch_rings[8];
	bool kpsd;		/* the node has power-domains: the kernel's PSD driver owns the slots, this module only holds a runtime PM reference */
	int pd_count;		/* power-domains cells (of_count_phandle_with_args; negative = malformed, stage=reads refuses): >1 = the claims are attached through pd_list (devm), <=1 = the driver core's single attach */
	struct dev_pm_domain_list *pd_list;	/* the attached multi-domain claims (set when pd_count > 1) */
	bool rpm_held;
	bool vote_held;		/* stage=reads holds the genpd power claims (pm_runtime_get_sync succeeded): the only flag that lets unmap put them back */
	u32 rtk_eps[8];		/* RTKit endpoints the firmware announced, one bit per endpoint number */
	u64 rtk_txq[8];		/* management messages accepted but not yet stored in TX (no free slot at the time), in order */
	unsigned int rtk_txq_n;
	bool rtk_ap_sent;	/* AP power ON was sent */
	bool rtk_ap_on;		/* the firmware acknowledged AP power ON */
	bool rtk_app_started;
	bool rtk_shutdown;	/* the shutdown handshake is under way */
	bool rtk_ap_acked, rtk_iop_acked, rtk_iop_idle;
	u32 rtk_ap_state, rtk_iop_state;	/* the states the shutdown asked for */
	bool psd_on[ANE_H18_NPSD];
	bool fw_started;
	/* hs_prog: the validated program file and the buffers made from it, freed with the surfaces after the core stopped */
	const struct firmware *prog_blob;
	struct ane_h18_pdesc {
		unsigned int nsec, nio;
		struct { u32 id; u64 size, off; } sec[ANE_H18_PROG_MAX_SECS];
		struct { u32 id, dir; u64 size, doff, dsize; } io[ANE_H18_PROG_MAX_IO];
	} pd;
	struct ane_h18_hs_buf pg_sec[ANE_H18_PROG_MAX_SECS], pg_io[ANE_H18_PROG_MAX_IO], pg_pool;
	struct ane_h18_hs_buf pg_kern;
};

static void ane_h18_result(struct ane_h18 *ane, const char *stg, const char *verdict, const char *reason)
{
	dev_crit(ane->dev, "ane_h18 RESULT stage=%s soc=%s verdict=%s reason=%s\n", stg, ane->soc->name, verdict, reason);
}

/* stage=reads: ONE register per write of "next" to /sys/kernel/debug/ane_h18_rd. The gate
 * state is re-read and checked before every read (the extra gate registers when set);
 * the address goes to the console at dev_crit priority BEFORE the readl, so a bus hang
 * always names its register. One write in flight: the mutex serializes concurrent writers
 * and the index lives in a local between the bounds check and the read. */
DEFINE_MUTEX(ane_h18_rd_lock);

static bool ane_h18_rd_gates_ok(struct ane_h18 *ane, u64 *bad_pa, u32 *bad_state,
				const char **reason)
{
	u32 gate;

	*reason = "pmgr-gates-off";
	if (ane->gate) {
		gate = readl(ane->gate);
		if ((gate & pmgr_gate_mask) != pmgr_gate_mask) {
			*bad_pa = pmgr_gate_pa;
			*bad_state = gate;
			return false;
		}
	}
	if (pmgr_gate2_pa) {
		u32 gate2 = readl(ane->gate2);

		if ((gate2 & pmgr_gate2_mask) != pmgr_gate2_mask) {
			*bad_pa = pmgr_gate2_pa;
			*bad_state = gate2;
			return false;
		}
		dev_crit(ane->dev, "ane_h18 reads gate2 pa=%#llx state 0x%08x mask 0x%08x ok\n",
			 (unsigned long long)pmgr_gate2_pa, gate2, pmgr_gate2_mask);
	}
	if (ane->ps) {
		static const struct { u32 off; const char *name; } ps_regs[] = {
			{ ANE_H18_PS_SYS, "ane_sys" },
			{ ANE_H18_PS_MPM, "ane_mpm" },
			{ ANE_H18_PS_CPU, "ane_cpu" },
			{ ANE_H18_PS_TDBASE, "ane_tdbase" },
		};
		unsigned int i;

		*reason = "pmgr-ps-off";
		for (i = 0; i < ARRAY_SIZE(ps_regs); i++) {
			u32 ps = readl(ane->ps + ps_regs[i].off);

			/* the ACTUAL state is bits 7:4; the TARGET (3:0) may lag */
			if (((ps >> 4) & pmgr_ps_mask) != pmgr_ps_mask) {
				*bad_pa = pmgr_ps_pa + ps_regs[i].off;
				*bad_state = ps;
				dev_crit(ane->dev, "ane_h18 reads %s pmgr ps state 0x%08x at %#llx lacks mask 0x%08x\n",
					 ps_regs[i].name, ps, (unsigned long long)*bad_pa,
					 pmgr_ps_mask);
				return false;
			}
		}
		dev_crit(ane->dev, "ane_h18 reads: ane_sys/ane_mpm/ane_cpu/ane_tdbase all in state, mask 0x%08x ok\n",
			 pmgr_ps_mask);
	}
	return true;
}

static ssize_t ane_h18_rd_write(struct file *file, const char __user *buffer, size_t count, loff_t *ppos)
{
	struct ane_h18 *ane;
	char cmd[8];
	u32 v;
	unsigned int idx;
	int ret;

	ret = simple_write_to_buffer(cmd, sizeof(cmd) - 1, ppos, buffer, count);
	if (ret < 0)
		return ret;
	cmd[ret] = '\0';
	if (strcmp(strim(cmd), "next"))
		return -EINVAL;
	mutex_lock(&ane_h18_rd_lock);
	ane = ane_h18_rd_dev;
	if (!ane || !ane->eng) {
		mutex_unlock(&ane_h18_rd_lock);
		return -ENODEV;
	}
	if (ane_h18_rd_idx >= ARRAY_SIZE(ane_h18_rd_list)) {
		if (ane_h18_rd_idx == ARRAY_SIZE(ane_h18_rd_list)) {
			ane_h18_rd_idx++;
			ane_h18_result(ane, "reads", "PASS", "reads-done");
		}
		mutex_unlock(&ane_h18_rd_lock);
		return count;
	}
	{
		u64 bad_pa = 0;
		u32 bad_state = 0;
		const char *refusal = "pmgr-gates-off";

		if (!ane_h18_rd_gates_ok(ane, &bad_pa, &bad_state, &refusal)) {
			dev_crit(ane->dev, "ane_h18 reads REFUSED: pmgr state 0x%08x at %#llx lacks its mask (power/clock gates or ps registers off). Enable the gates through the pmgr first.\n",
				 bad_state, (unsigned long long)bad_pa);
			ane_h18_result(ane, "reads", "REFUSED", refusal);
			mutex_unlock(&ane_h18_rd_lock);
			return -EPERM;
		}
	}
	idx = ane_h18_rd_idx;
	dev_crit(ane->dev, "ane_h18 reads %s pa=%#llx - reading now\n",
		 ane_h18_rd_list[idx].name,
		 ane->soc->engine_pa + ane_h18_rd_list[idx].off);
	v = readl(ane->eng + ane_h18_rd_list[idx].off);
	dev_crit(ane->dev, "ane_h18 reads %s = 0x%08x\n", ane_h18_rd_list[idx].name, v);
	ane_h18_rd_idx = idx + 1;
	mutex_unlock(&ane_h18_rd_lock);
	return count;
}

static const struct file_operations ane_h18_rd_fops = {
	.owner = THIS_MODULE,
	.write = ane_h18_rd_write,
};

static int ane_h18_stage_idx(const char *name, unsigned int *out)
{
	if (!strcmp(name, "dt")) { *out = ANE_H18_STAGE_DT; return 0; }
	if (!strcmp(name, "psd")) { *out = ANE_H18_STAGE_PSD; return 0; }
	if (!strcmp(name, "power")) { *out = ANE_H18_STAGE_POWER; return 0; }
	if (!strcmp(name, "boot")) { *out = ANE_H18_STAGE_BOOT; return 0; }
	if (!strcmp(name, "reads")) { *out = ANE_H18_STAGE_READS; return 0; }
	return -EINVAL;
}

/* Find a named reg entry and require that it is exactly the window the SoC row describes. A stale or shifted
 * device tree must not make the static offsets or the PSD request writes land on another block.
 */
static int ane_h18_window(struct ane_h18 *ane, const char *name, u64 pa, u64 size, int *idx)
{
	struct device_node *np = ane->dev->of_node;
	struct resource res;
	int i = of_property_match_string(np, "reg-names", name);

	if (i < 0 || of_address_to_resource(np, i, &res)) {
		dev_err(ane->dev, "ane_h18 reg-names \"%s\" missing\n", name);
		return -ENODEV;
	}
	if (res.start != pa || resource_size(&res) != size) {
		dev_err(ane->dev, "ane_h18 window %s mismatch: reg start=%#llx size=%#llx, SoC row wants start=%#llx size=%#llx\n",
			name, (u64)res.start, (u64)resource_size(&res), pa, size);
		return -EINVAL;
	}
	*idx = i;
	return 0;
}

static u64 ane_h18_psd_pa(const struct ane_h18_soc *s, unsigned int i)
{
	return ANE_H18_PSD_BASE + ((u64)s->psd_slot[i] << 8);
}

/* Check the numeric properties the SoC row depends on. The live ADT says ane-type 784 and mailbox number 12. */
static int ane_h18_check_props(struct ane_h18 *ane)
{
	struct device_node *np = ane->dev->of_node;
	u32 type = 0, num = 0;

	if (of_property_read_u32(np, "apple,ane-type", &type) || type != ane->soc->ane_type) {
		dev_err(ane->dev, "ane_h18 apple,ane-type=%u, SoC row wants %u\n", type, ane->soc->ane_type);
		return -EINVAL;
	}
	if (of_property_read_u32(np, "msgbox-mailbox-num", &num) || num != ane->soc->mbox_num) {
		dev_err(ane->dev, "ane_h18 msgbox-mailbox-num=%u, SoC row wants %u\n", num, ane->soc->mbox_num);
		return -EINVAL;
	}
	return 0;
}

/* Resolve and check every window; indexes in idx[]: 0 asc, 1 rvbar, 2 mbox, 3.. psd. */
static int ane_h18_check_windows(struct ane_h18 *ane, int *idx)
{
	const struct ane_h18_soc *s = ane->soc;
	char nm[8];
	unsigned int i;
	int ret;

	ret = ane_h18_check_props(ane);
	if (!ret)
		ret = ane_h18_window(ane, "asc", s->asc_pa, s->asc_size, &idx[0]);
	if (!ret)
		ret = ane_h18_window(ane, "rvbar", s->rvbar_pa, s->rvbar_size, &idx[1]);
	if (!ret)
		ret = ane_h18_window(ane, "mbox", s->mbox_pa, s->mbox_size, &idx[2]);
	for (i = 0; !ret && i < ANE_H18_NPSD; i++) {
		snprintf(nm, sizeof(nm), "psd%u", i);
		ret = ane_h18_window(ane, nm, ane_h18_psd_pa(s, i), ANE_H18_PSD_WIN_SIZE, &idx[3 + i]);
	}
	return ret;
}

static void ane_h18_report_windows(struct platform_device *pdev, struct ane_h18 *ane)
{
	unsigned int i;

	for (i = 0; ; i++) {
		struct resource *r = platform_get_resource(pdev, IORESOURCE_MEM, i);

		if (!r)
			break;
		dev_info(ane->dev, "ane_h18 window %u: pa=%pa end=%pa\n", i, &r->start, &r->end);
	}
}

static void ane_h18_unmap(struct ane_h18 *ane)
{
	unsigned int i;

	for (i = 0; i < ANE_H18_NPSD; i++)
		if (ane->psd[i]) {
			iounmap(ane->psd[i]);
			ane->psd[i] = NULL;
		}
	/* teardown order: the debugfs control dies FIRST (no in-flight "next" can reach a
	 * mapped window), then the vote, then the mappings. ane_h18_rd_lock serializes
	 * against a writer that already passed the ENODEV check. */
	mutex_lock(&ane_h18_rd_lock);
	ane_h18_rd_dev = NULL;
	mutex_unlock(&ane_h18_rd_lock);
	if (ane_h18_rd_control) {
		debugfs_remove(ane_h18_rd_control);
		ane_h18_rd_control = NULL;
	}
	if (ane->vote_held) {
		/* the multi-domain claims hold through devm links: nothing to put here */
		if (ane->pd_count <= 1)
			pm_runtime_put_sync(ane->dev);	/* release the single-domain vote cast at reads arm */
		ane->vote_held = false;
	}
	if (ane->asc) { iounmap(ane->asc); ane->asc = NULL; }
	if (ane->rvbar) { iounmap(ane->rvbar); ane->rvbar = NULL; }
	if (ane->mbox) { iounmap(ane->mbox); ane->mbox = NULL; }
	if (ane->dart) { iounmap(ane->dart); ane->dart = NULL; }
	for (i = 1; i < ANE_H18_DART_INSTANCES; i++)
		if (ane->dartx[i]) { iounmap(ane->dartx[i]); ane->dartx[i] = NULL; }
	if (ane->scratch) { iounmap(ane->scratch); ane->scratch = NULL; }
	if (ane->eng) { iounmap(ane->eng); ane->eng = NULL; }
	if (ane->gate) { iounmap(ane->gate); ane->gate = NULL; }
	if (ane->gate2) { iounmap(ane->gate2); ane->gate2 = NULL; }
	if (ane->ps) { iounmap(ane->ps); ane->ps = NULL; }
	if (ane->prog_blob) { release_firmware(ane->prog_blob); ane->prog_blob = NULL; }
	if (ane->doorbell) { iounmap(ane->doorbell); ane->doorbell = NULL; }
	if (ane->ipi_pending) { iounmap(ane->ipi_pending); ane->ipi_pending = NULL; }
	if (ane->ipi_ack) { iounmap(ane->ipi_ack); ane->ipi_ack = NULL; }
}

static int ane_h18_map_psd(struct ane_h18 *ane, const int *idx)
{
	unsigned int i;

	for (i = 0; i < ANE_H18_NPSD; i++) {
		ane->psd[i] = of_iomap(ane->dev->of_node, idx[3 + i]);
		if (!ane->psd[i]) {
			dev_err(ane->dev, "ane_h18 of_iomap psd%u failed\n", i);
			return -ENXIO;
		}
	}
	return 0;
}

/* ---- stage dt ---- */
static int ane_h18_stage_dt(struct ane_h18 *ane, const int *idx)
{
	const struct ane_h18_soc *s = ane->soc;
	unsigned int i;

	(void)idx;
	dev_info(ane->dev, "ane_h18 stage=dt soc=%s ane-type=%u mailbox-num=%u\n", s->name, s->ane_type, s->mbox_num);
	dev_info(ane->dev, "ane_h18 asc pa=%#llx size=%#x rvbar pa=%#llx size=%#x mbox pa=%#llx size=%#x\n",
		 s->asc_pa, s->asc_size, s->rvbar_pa, s->rvbar_size, s->mbox_pa, s->mbox_size);
	for (i = 0; i < ANE_H18_NPSD; i++)
		dev_info(ane->dev, "ane_h18 psd%u slot=%u pa=%#llx\n", i, s->psd_slot[i], ane_h18_psd_pa(s, i));
	ane_h18_result(ane, "dt", "PASS", "dt-windows-match");
	return 0;
}

/* ---- stage psd: read only ---- */
static int ane_h18_stage_psd(struct ane_h18 *ane)
{
	unsigned int i;

	for (i = 0; i < ANE_H18_NPSD; i++) {
		u32 valid, status = 0;

		dev_crit(ane->dev, "ane_h18 read psd%u slot=%u VALID pa=%#llx\n", i, ane->soc->psd_slot[i],
			 ane_h18_psd_pa(ane->soc, i) + ANE_H18_PSD_VALID);
		valid = readl(ane->psd[i] + ANE_H18_PSD_VALID);
		if (valid & 1) {
			/* a STATUS read clears the acknowledge, so only when VALID is set (as pmgr-psd.c) */
			dev_crit(ane->dev, "ane_h18 read psd%u slot=%u STATUS pa=%#llx\n", i, ane->soc->psd_slot[i],
				 ane_h18_psd_pa(ane->soc, i) + ANE_H18_PSD_STATUS);
			status = readl(ane->psd[i] + ANE_H18_PSD_STATUS);
		}
		dev_info(ane->dev, "ane_h18 psd%u slot=%u valid=%#x status=%#x state=%s\n", i, ane->soc->psd_slot[i], valid,
			 status, (valid & 1) ? ((status & ANE_H18_PSD_STATE_MASK) == 0 ? "P0-run" : "other") : "not-valid");
	}
	ane_h18_result(ane, "psd", "PASS", "psd-read-only");
	return 0;
}

/* ---- stage power ---- */
static int ane_h18_psd_request(struct ane_h18 *ane, unsigned int i, u32 state)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(psd_timeout_ms);
	u32 valid = 0, status = 0;

	dev_crit(ane->dev, "ane_h18 write psd%u slot=%u REQUEST=%u pa=%#llx\n", i, ane->soc->psd_slot[i], state,
		 ane_h18_psd_pa(ane->soc, i) + ANE_H18_PSD_REQUEST);
	writel(state, ane->psd[i] + ANE_H18_PSD_REQUEST);
	for (;;) {
		valid = readl(ane->psd[i] + ANE_H18_PSD_VALID);
		if (valid & 1) {
			status = readl(ane->psd[i] + ANE_H18_PSD_STATUS);
			if ((status & ANE_H18_PSD_STATE_MASK) == state) {
				dev_info(ane->dev, "ane_h18 psd%u slot=%u state %u acknowledged (valid=%#x status=%#x)\n", i,
					 ane->soc->psd_slot[i], state, valid, status);
				return 0;
			}
		}
		if (time_after(jiffies, timeout))
			break;
		usleep_range(50, 200);
	}
	dev_err(ane->dev, "ane_h18 psd%u slot=%u request %u NOT acknowledged: valid=%#x status=%#x diag=%#x\n", i,
		ane->soc->psd_slot[i], state, valid, status, readl(ane->psd[i] + ANE_H18_PSD_DIAG));
	return -ETIMEDOUT;
}

/* Request state 6 on every slot this module turned on, in reverse order. Returns how many slots did not acknowledge it. */
static int ane_h18_psd_off(struct ane_h18 *ane)
{
	int i, failed = 0;

	if (ane->kpsd) {
		if (ane->rpm_held) {
			ane->rpm_held = false;
			pm_runtime_put(ane->dev);
		}
		return 0;
	}
	for (i = ANE_H18_NPSD - 1; i >= 0; i--) {
		if (!ane->psd_on[i])
			continue;
		if (ane_h18_psd_request(ane, i, ANE_H18_PSD_P6_RAIL)) {
			dev_warn(ane->dev, "ane_h18 psd%d did not return to state 6\n", i);
			failed++;
		}
		ane->psd_on[i] = false;
	}
	return failed;
}

/* Bring the slots up. Without power-domains in the node this module requests state psd_state on the first psd_slots slots and waits for each
 * acknowledge. With power-domains the kernel's PSD driver owns the slots: this module takes a runtime PM reference, which powers the domain (and fails
 * if the power-on fails), and never writes a PSD request. The slots' VALID bit cannot tell on from off there, because the kernel driver's own STATUS
 * read clears the acknowledge. */
static int ane_h18_psd_up(struct ane_h18 *ane, const char *stg)
{
	unsigned int i;
	int ret;

	if (ane->kpsd) {
		dev_crit(ane->dev, "ane_h18 runtime PM get: the kernel PSD driver powers slots 30 and 31 now\n");
		ret = pm_runtime_resume_and_get(ane->dev);
		if (ret) {
			ane_h18_result(ane, stg, "FAIL", "kernel-psd-power-on-failed");
			return ret;
		}
		ane->rpm_held = true;
		return 0;
	}
	for (i = 0; i < psd_slots; i++) {
		ret = ane_h18_psd_request(ane, i, psd_state);
		if (ret) {
			ane_h18_psd_off(ane);
			ane_h18_result(ane, stg, "FAIL", "psd-not-acknowledged");
			return ret;
		}
		ane->psd_on[i] = true;
	}
	return 0;
}

static int ane_h18_read(struct ane_h18 *ane, const char *name, void __iomem *base, u64 pa, u32 off, u32 size_win, u32 *v)
{
	if ((u64)off + 4 > size_win) {
		dev_err(ane->dev, "ane_h18 refusing read %s +%#x outside its window\n", name, off);
		return -EPERM;
	}
	dev_crit(ane->dev, "ane_h18 read %s pa=%#llx\n", name, pa + off);
	*v = readl(base + off);
	dev_info(ane->dev, "ane_h18 word=%s pa=%#llx value=%#x\n", name, pa + off, *v);
	return 0;
}

/* Candidate Postoffice channels for this ANE: mailbox number, twice the number, twice the number plus one. */
static u32 ane_h18_po_chan(const struct ane_h18_soc *s, unsigned int cand)
{
	return cand == 0 ? s->mbox_num : s->mbox_num * 2 + (cand - 1);
}

static int ane_h18_stage_power(struct ane_h18 *ane)
{
	const struct ane_h18_soc *s = ane->soc;
	u32 cpu_status = 0, asc_status = 0, cpu_ctrl = 0, rv_lo = 0, rv_hi = 0, po[ANE_H18_PO_NCAND] = { 0 };
	unsigned int i;
	int ret = 0;

	if (!confirm_power) {
		ane_h18_result(ane, "power", "REFUSED", "confirm_power=1 required");
		return -EPERM;
	}
	if (psd_state >= ANE_H18_PSD_P6_RAIL || psd_slots < 1 || psd_slots > ANE_H18_NPSD) {
		ane_h18_result(ane, "power", "REFUSED", "psd_state must be 0 to 5 and psd_slots 1 or 2");
		return -EINVAL;
	}
	ret = ane_h18_psd_up(ane, "power");
	if (ret)
		return ret;
	/* Everything below is read-only and happens only after every requested slot acknowledged. */
	ret = ane_h18_read(ane, "CPU_STATUS", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_STATUS, s->asc_size, &cpu_status);
	if (!ret)
		ret = ane_h18_read(ane, "ASC_STATUS", ane->asc, s->asc_pa, ANE_H18_ASC_STATUS, s->asc_size, &asc_status);
	if (!ret && read_ctrl)
		ret = ane_h18_read(ane, "CPU_CONTROL", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_CONTROL, s->asc_size, &cpu_ctrl);
	if (!ret)
		ret = ane_h18_read(ane, "IORVBAR_LO", ane->rvbar, s->rvbar_pa, ANE_H18_RVBAR_OFF, s->rvbar_size, &rv_lo);
	if (!ret)
		ret = ane_h18_read(ane, "IORVBAR_HI", ane->rvbar, s->rvbar_pa, ANE_H18_RVBAR_OFF + 4, s->rvbar_size, &rv_hi);
	/* The channel of this ANE in the Postoffice block is not verified: the lab notebook's T8152 macOS notes give
	 * "msgbox_using_postoffice + (msgbox_mailbox_num << 1)" (channel 25), a review reads the same expression as the
	 * packed pair that makes it channel 12, and the SMC node does not fit either. Read the status word of all
	 * three candidate channels (12, 24, 25); they are in one block, the same window, and the log decides. */
	for (i = 0; !ret && read_mbox && i < ANE_H18_PO_NCAND; i++) {
		char nm[16];

		snprintf(nm, sizeof(nm), "PO_STATUS_CH%u", ane_h18_po_chan(s, i));
		ret = ane_h18_read(ane, nm, ane->mbox, s->mbox_pa, ane_h18_po_chan(s, i) * ANE_H18_PO_STRIDE + ANE_H18_PO_STATUS,
				   s->mbox_size, &po[i]);
	}
	if (ret) {
		if (!keep_on)
			ane_h18_psd_off(ane);
		ane_h18_result(ane, "power", "FAIL", "read-refused");
		return ret;
	}
	dev_info(ane->dev, "ane_h18 power cpu_status=%#x asc_status=%#x iorvbar=%#llx psd_state=%u slots=%u keep_on=%d\n",
		 cpu_status, asc_status, ((u64)rv_hi << 32) | rv_lo, psd_state, psd_slots, keep_on);
	if (read_ctrl)
		dev_info(ane->dev, "ane_h18 cpu_control=%#x\n", cpu_ctrl);
	if (read_mbox)
		dev_info(ane->dev, "ane_h18 po_status ch%u=%#x ch%u=%#x ch%u=%#x\n", ane_h18_po_chan(s, 0), po[0], ane_h18_po_chan(s, 1),
			 po[1], ane_h18_po_chan(s, 2), po[2]);
	if (!keep_on && ane_h18_psd_off(ane)) {
		ane_h18_result(ane, "power", "FAIL", "psd-not-returned-to-state-6");
		return -EIO;
	}
	ane_h18_result(ane, "power", "PASS", "psd-acked+asc-read");
	return 0;
}

/* Read the outbox of one Postoffice channel (window offset of the channel, tag for the log) if its status says it holds a message.
 * Logs the raw 16 bytes. Returns 1 if a message was read. */
static int ane_h18_po_pop(struct ane_h18 *ane, u32 chan_off, const char *tag, u32 status)
{
	const struct ane_h18_soc *s = ane->soc;
	u32 off = chan_off + ANE_H18_PO_OUTBOX, w[4] = { 0 };
	unsigned int i;

	if ((status & 0xf) == 0)
		return 0;
	if ((u64)off + 16 > s->mbox_size)
		return 0;
	dev_crit(ane->dev, "ane_h18 read outbox %s pa=%#llx (status %#x)\n", tag, s->mbox_pa + off, status);
	for (i = 0; i < 4; i++)
		w[i] = readl(ane->mbox + off + 4 * i);
	dev_info(ane->dev, "ane_h18 msg %s msg0=%#llx msg1=%#llx\n", tag, ((u64)w[1] << 32) | w[0], ((u64)w[3] << 32) | w[2]);
	return 1;
}

#include "ane_h18_fwload.h"

/* Undo ane_h18_map_firmware(): unmap what was mapped, detach, free. Never called while the core may run. */
static void ane_h18_unmap_firmware(struct ane_h18 *ane, unsigned int nmapped)
{
	unsigned int i;

	if (!ane->domain)
		return;
	for (i = 0; i < nmapped; i++)
		iommu_unmap(ane->domain, ane->soc->fw_seg[i].iova, ane->soc->fw_seg[i].size);
	if (ane->dom_own) {
		iommu_detach_device(ane->domain, ane->dev);
		iommu_domain_free(ane->domain);
	}
	ane->domain = NULL;
}

/* Attach the ANE to its DART through the iommu API (the DART driver programs its tunables, TTBR, TCR and stream enable now, with the slots powered)
 * and map the two firmware segments the boot loader left in DRAM at their IOVAs. The mapping stays until the module is removed after a clean stop. */
static int ane_h18_map_firmware(struct ane_h18 *ane)
{
	const struct ane_h18_soc *s = ane->soc;
	struct iommu_domain *dom;
	unsigned int i;
	int ret;

	if (!device_iommu_mapped(ane->dev)) {
		dev_err(ane->dev, "ane_h18 map_fw: the node has no iommus (no DART client)\n");
		ret = -ENODEV;
		goto refused;
	}
	if (handshake) {
		/* The DMA API (the handshake surfaces) allocates in the default domain, so the segments go into that one and nothing is attached. */
		dom = iommu_get_domain_for_dev(ane->dev);
		if (!dom || !(dom->type & __IOMMU_DOMAIN_DMA_API)) {
			dev_err(ane->dev, "ane_h18 map_fw: the ANE has no default DMA domain\n");
			ret = -ENODEV;
			goto refused;
		}
		dev_crit(ane->dev, "ane_h18 map the firmware into the default DMA domain of the ANE (no attach)\n");
	} else {
		dom = iommu_paging_domain_alloc(ane->dev);
		if (IS_ERR(dom)) {
			dev_err(ane->dev, "ane_h18 map_fw: no iommu domain: %ld\n", PTR_ERR(dom));
			ret = PTR_ERR(dom);
			goto refused;
		}
		dev_crit(ane->dev, "ane_h18 attach the ANE to a new DART domain (the DART driver programs the DART now)\n");
		ret = iommu_attach_device(dom, ane->dev);
		if (ret) {
			dev_err(ane->dev, "ane_h18 map_fw: attach failed: %d\n", ret);
			iommu_domain_free(dom);
			goto refused;
		}
	}
	ane->domain = dom;
	ane->dom_own = !handshake;
	if (owned_fw) {
		/* the Linux-owned image maps at the fixed IOVAs the boot entry names: TEXT at rvbar_expect & ~1, DATA behind it */
		ane->map_iova[0] = s->rvbar_expect & ~1ull;
		ane->map_size[0] = ANE_H18_FW_TEXT_SIZE;
		ane->map_iova[1] = ane->map_iova[0] + ANE_H18_FW_TEXT_SIZE;
		ane->map_size[1] = ANE_H18_FW_IMAGE_SIZE - ANE_H18_FW_TEXT_SIZE;
		ane->map_len[0] = ane->map_len[1] = 0;
		/* ownership validation before any alias map: the image's own DMA range must not sit inside the fixed alias
		 * range, and every fixed 16 KiB page must be free of an existing translation. A refusal never unmaps the
		 * pre-existing mapping or the image backing. */
		if ((u64)ane->owned_fw_img.iova < ane->map_iova[1] + ane->map_size[1] &&
		    ane->map_iova[0] < (u64)ane->owned_fw_img.iova + ane->owned_fw_img.size) {
			dev_err(ane->dev, "ane_h18 owned fw: the image DMA range %#llx..%#llx overlaps the fixed alias range %#llx..%#llx\n",
				(u64)ane->owned_fw_img.iova, (u64)ane->owned_fw_img.iova + ane->owned_fw_img.size - 1,
				ane->map_iova[0], ane->map_iova[1] + ane->map_size[1] - 1);
			ane_h18_unmap_owned_firmware(ane);
			ane_h18_result(ane, "boot", "FAIL", "owned-map-failed");
			return -EBUSY;
		}
		for (i = 0; i < ANE_H18_FW_PAGES; i++) {
			if (iommu_iova_to_phys(dom, ane->map_iova[0] + (u64)i * SZ_16K)) {
				dev_err(ane->dev, "ane_h18 owned fw: an existing translation already holds fixed alias page %u (%#llx); it is left untouched\n",
					i, ane->map_iova[0] + (u64)i * SZ_16K);
				ane_h18_unmap_owned_firmware(ane);
				ane_h18_result(ane, "boot", "FAIL", "owned-map-failed");
				return -EBUSY;
			}
		}
	}
	for (i = 0; i < ARRAY_SIZE(s->fw_seg); i++) {
		if (owned_fw)
			dev_crit(ane->dev, "ane_h18 iommu_map iova=%#llx pa=%#llx size=%#llx %s (owned image)\n", ane->map_iova[i],
				 ane->owned_pa[(u64)i * ANE_H18_FW_TEXT_SIZE / SZ_16K], ane->map_size[i], i ? "read+write" : "read");
		if (owned_fw) {
			ret = ane_h18_map_owned_segment(ane, dom, i);
			if (!ret)
				ane->map_n = i + 1;
		} else {
			dev_crit(ane->dev, "ane_h18 iommu_map iova=%#llx pa=%#llx size=%#llx %s\n", s->fw_seg[i].iova, s->fw_seg[i].pa, s->fw_seg[i].size,
				 s->fw_seg[i].write ? "read+write" : "read");
			ret = iommu_map(dom, s->fw_seg[i].iova, s->fw_seg[i].pa, s->fw_seg[i].size,
					IOMMU_READ | (s->fw_seg[i].write ? IOMMU_WRITE : 0), GFP_KERNEL);
		}
		if (ret) {
			dev_err(ane->dev, "ane_h18 map_fw: iommu_map of segment %u failed: %d\n", i, ret);
			if (owned_fw) {
				ane_h18_unmap_owned_firmware(ane);
				ane_h18_result(ane, "boot", "FAIL", "owned-map-failed");
			} else {
				ane_h18_unmap_firmware(ane, i);
			}
			return ret;
		}
	}
	return 0;

refused:
	if (owned_fw) {
		ane_h18_unmap_owned_firmware(ane);	/* nothing is mapped and no domain is held: only the staged image goes back */
		ane_h18_result(ane, "boot", "FAIL", "owned-map-failed");
	}
	return ret;
}

/* Offsets of the ANE DART words read before RUN: config (+0x0), +0x4, version (+0x8), the stream-enable word (+0xc00), the stream configuration
 * words of streams 0, 10 and 13 (+0x1000 + 4 * stream) and the TTBR words of streams 0, 1, 10, 13 and 14 (+0x1400 + 4 * stream, one TTBR word per
 * stream as in the T8152 table of drivers/iommu/apple-dart.c that the USB DARTs of this board already run on).
 * Nothing is written. Every read is logged before it is done, so a hang names the word. */
static const u32 ane_h18_dart_off[ANE_H18_DART_NREAD] = { 0x0, 0x4, 0x8, 0xc00, 0x1000, 0x1028, 0x1034, 0x1400, 0x1404, 0x1428, 0x1434, 0x1438 };

static int ane_h18_read_dart_words(struct ane_h18 *ane, const char *pfx)
{
	const struct ane_h18_soc *s = ane->soc;
	unsigned int i;
	u32 v;
	int ret = 0;

	for (i = 0; !ret && i < ANE_H18_DART_NREAD; i++) {
		char nm[24];

		snprintf(nm, sizeof(nm), "%s_%#x", pfx, ane_h18_dart_off[i]);
		ret = ane_h18_read(ane, nm, ane->dart, s->dart_pa, ane_h18_dart_off[i], s->dart_size, &v);
	}
	return ret;
}

/* One 16 byte message moved as a pair, the way the kernel mailbox driver moves it on the T8152 counted FIFO. The userspace sim has no arm64 and moves
 * the four words one by one. */
static void ane_h18_po_load16(void __iomem *p, u32 w[4])
{
#ifdef CONFIG_ARM64
	u64 lo, hi;

	asm volatile("ldp %0, %1, [%2]" : "=&r" (lo), "=&r" (hi) : "r" (p) : "memory");
	rmb();
	w[0] = (u32)lo;
	w[1] = (u32)(lo >> 32);
	w[2] = (u32)hi;
	w[3] = (u32)(hi >> 32);
#else
	unsigned int i;

	for (i = 0; i < 4; i++)
		w[i] = readl(p + 4 * i);
#endif
}

static void ane_h18_po_store16(void __iomem *p, const u32 w[4])
{
#ifdef CONFIG_ARM64
	u64 lo = ((u64)w[1] << 32) | w[0], hi = ((u64)w[3] << 32) | w[2];

	wmb();
	asm volatile("stp %0, %1, [%2]" : : "r" (lo), "r" (hi), "r" (p) : "memory");
#else
	unsigned int i;

	for (i = 0; i < 4; i++)
		writel(w[i], p + 4 * i);
#endif
}

/* Read the eight SCRATCH words and log them under a phase tag. Read only; every address is logged before its read. */
static void ane_h18_read_scratch_words(struct ane_h18 *ane, const char *phase)
{
	const struct ane_h18_soc *s = ane->soc;
	unsigned int i;
	u32 v;

	if (!ane->scratch)
		return;
	for (i = 0; i < ANE_H18_SCRATCH_N; i++) {
		char nm[32];

		snprintf(nm, sizeof(nm), "SCRATCH%u_%s", i, phase);
		if (ane_h18_read(ane, nm, ane->scratch, s->scratch_pa, ANE_H18_SCRATCH_OFF + 4 * i, s->scratch_size, &v))
			break;
	}
}

/* Write one SCRATCH word. The address is logged before the store. */
static void ane_h18_scratch_write(struct ane_h18 *ane, unsigned int i, u32 v)
{
	dev_crit(ane->dev, "ane_h18 write SCRATCH%u=%#x pa=%#llx\n", i, v, ane->soc->scratch_pa + ANE_H18_SCRATCH_OFF + 4 * i);
	writel(v, ane->scratch + ANE_H18_SCRATCH_OFF + 4 * i);
}

/* One program surface goes back: the fixed alias is unmapped first so the device cannot reach the pages after the DMA API releases them. When the unmap
 * is short, part of the alias is still mapped, so the pages stay allocated (leaked, logged) instead of going back to the allocator under a live mapping. */
static void ane_h18_hs_buf_free(struct ane_h18 *ane, struct ane_h18_hs_buf *b)
{
	if (!b->va)
		return;
	if (b->fixed) {
		size_t un = ane->domain ? iommu_unmap(ane->domain, b->iova, b->size) : b->size;

		b->fixed = false;
		if (un != b->size) {
			dev_err(ane->dev, "ane_h18 fixed alias at %#llx unmapped %#zx of %#zx bytes: the pages are leaked, not freed\n", (u64)b->iova, un, b->size);
			b->va = NULL;
			ane_h18_pin_module(ane);
			return;
		}
	}
	dma_free_coherent(ane->dev, b->size, b->va, b->dma);
	b->va = NULL;
}

/* The program surfaces and their fixed-IOVA aliases of a finished pass. Only safe once every command answered (the failure paths leave them for ane_h18_hs_free). */
static void ane_h18_prog_surfaces_free(struct ane_h18 *ane)
{
	unsigned int i;

	for (i = 0; i < ANE_H18_PROG_MAX_SECS; i++)
		ane_h18_hs_buf_free(ane, &ane->pg_sec[i]);
	ane_h18_hs_buf_free(ane, &ane->pg_kern);
	for (i = 0; i < ANE_H18_PROG_MAX_IO; i++)
		ane_h18_hs_buf_free(ane, &ane->pg_io[i]);
	ane_h18_hs_buf_free(ane, &ane->pg_pool);
}

static void ane_h18_hs_free(struct ane_h18 *ane)
{
	struct ane_h18_hs_buf *b[4] = { &ane->hs_cmd, &ane->hs_heap, &ane->hs_ipc, &ane->hs_pool };
	unsigned int i;

	if (ane->hs_cmd_in_pool) {
		ane->hs_cmd.va = NULL;
		ane->hs_cmd_in_pool = false;
	}

	for (i = 0; i < 4; i++)
		if (b[i]->va) {
			dma_free_coherent(ane->dev, b[i]->size, b[i]->va, b[i]->iova);
			b[i]->va = NULL;
		}
	for (i = 0; i < ane->hs_mal_n; i++)
		if (ane->hs_mal[i].va) {
			dma_free_coherent(ane->dev, ane->hs_mal[i].size, ane->hs_mal[i].va, ane->hs_mal[i].iova);
			ane->hs_mal[i].va = NULL;
		}
	ane->hs_mal_n = 0;
	ane->mal_ring = NULL;
	memset(ane->ch_rings, 0, sizeof(ane->ch_rings));
	for (i = 0; i < ANE_H18_PROG_MAX_SECS; i++)
		ane_h18_hs_buf_free(ane, &ane->pg_sec[i]);
	ane_h18_hs_buf_free(ane, &ane->pg_kern);
	for (i = 0; i < ANE_H18_PROG_MAX_IO; i++)
		ane_h18_hs_buf_free(ane, &ane->pg_io[i]);
	ane_h18_hs_buf_free(ane, &ane->pg_pool);
	if (ane->prog_blob) {
		release_firmware(ane->prog_blob);
		ane->prog_blob = NULL;
	}
}

/* One zeroed surface from the DMA API, in the default domain the firmware segments are mapped in. It must not overlap a firmware segment. */
static int ane_h18_hs_alloc(struct ane_h18 *ane, struct ane_h18_hs_buf *b, size_t size, const char *name)
{
	const struct ane_h18_soc *s = ane->soc;
	unsigned int i;

	b->va = dma_alloc_coherent(ane->dev, size, &b->iova, GFP_KERNEL);
	if (!b->va) {
		dev_err(ane->dev, "ane_h18 handshake: allocation of %s (%#zx bytes) failed\n", name, size);
		return -ENOMEM;
	}
	b->dma = b->iova;
	b->fixed = false;
	b->size = size;
	dev_info(ane->dev, "ane_h18 hs surface %s size=%#zx iova=%#llx\n", name, size, (u64)b->iova);
	for (i = 0; i < ARRAY_SIZE(s->fw_seg); i++)
		if (b->iova < s->fw_seg[i].iova + s->fw_seg[i].size && b->iova + size > s->fw_seg[i].iova) {
			dev_err(ane->dev, "ane_h18 handshake: %s overlaps firmware segment %u\n", name, i);
			return -EBUSY;
		}
	return 0;
}

/* A surface at a fixed IOVA: allocated as above, then aliased. The pages stay where the DMA API put them; each 16 KiB page of the allocation is looked up in the
 * domain and mapped again at `at` + its offset, read and write. The target range must be 16 KiB aligned, below the DART's 0xe0000000 limit, clear of the firmware
 * segments and unmapped. A failure unmaps what this call mapped; the allocation itself is freed by ane_h18_hs_free. */
static int ane_h18_hs_alloc_at(struct ane_h18 *ane, struct ane_h18_hs_buf *b, size_t size, const char *name, u64 at)
{
	const struct ane_h18_soc *s = ane->soc;
	struct iommu_domain *dom = ane->domain;
	size_t off;
	unsigned int i;
	int ret;

	ret = ane_h18_hs_alloc(ane, b, size, name);
	if (ret || !at)
		return ret;
	if (!dom) {
		dev_err(ane->dev, "ane_h18 fixed IOVA for %s: no iommu domain\n", name);
		return -ENODEV;
	}
	if (((at | size) & (SZ_16K - 1)) || at >= 0xe0000000ULL || size > 0xe0000000ULL - at) {
		dev_err(ane->dev, "ane_h18 fixed IOVA %#llx size %#zx for %s is not 16 KiB aligned or is not below 0xe0000000\n", at, size, name);
		return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(s->fw_seg); i++)
		if (at < s->fw_seg[i].iova + s->fw_seg[i].size && at + size > s->fw_seg[i].iova) {
			dev_err(ane->dev, "ane_h18 fixed IOVA %#llx for %s overlaps firmware segment %u\n", at, name, i);
			return -EBUSY;
		}
	for (off = 0; off < size; off += SZ_16K)
		if (iommu_iova_to_phys(dom, at + off)) {
			dev_err(ane->dev, "ane_h18 fixed IOVA %#llx for %s is already mapped\n", at + off, name);
			return -EBUSY;
		}
	for (off = 0; off < size; off += SZ_16K) {
		u64 pa = iommu_iova_to_phys(dom, b->dma + off);

		ret = pa ? iommu_map(dom, at + off, pa, SZ_16K, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL) : -EFAULT;
		if (ret) {
			dev_err(ane->dev, "ane_h18 fixed IOVA %#llx for %s: map of page at +%#zx failed: %d\n", at, name, off, ret);
			if (off)
				iommu_unmap(dom, at, off);
			return ret;
		}
	}
	b->iova = at;
	b->fixed = true;
	dev_info(ane->dev, "ane_h18 hs surface %s aliased at fixed iova=%#llx (dma api iova %#llx)\n", name, at, (u64)b->dma);
	return 0;
}

/* The fixed IOVA of program buffer i: prog_iova_bufs plus the 16 KiB aligned sizes of the buffers before it, or 0 for a dynamic one. A base that would push the
 * range past the DART limit returns 0xe0000000, which ane_h18_hs_alloc_at refuses. */
static u64 ane_h18_io_at(const struct ane_h18_pdesc *p, unsigned int i)
{
	u64 at = prog_iova_bufs;
	unsigned int k;

	if (!at)
		return 0;
	for (k = 0; k < i; k++) {
		u64 sz = ALIGN(p->io[k].size, SZ_16K);

		if (sz > 0xe0000000ULL - at)
			return 0xe0000000ULL;
		at += sz;
	}
	return at;
}

static void ane_h18_hs_put(struct ane_h18 *ane, u8 *blk, unsigned int off, unsigned int width, u64 v)
{
	dev_info(ane->dev, "ane_h18 hs block +%#x (%u bytes) = %#llx\n", off, width, v);
	memcpy(blk + off, &v, width);	/* little endian host */
}

static const struct ane_h18_hs_buf *ane_h18_hs_find(const struct ane_h18 *ane, u64 a)
{
	const struct ane_h18_hs_buf *b[3] = { &ane->hs_pool, &ane->hs_ipc, &ane->hs_heap };
	unsigned int i;

	for (i = 0; i < 3; i++)
		if (b[i]->va && a >= b[i]->iova && a < b[i]->iova + b[i]->size)
			return b[i];
	return NULL;
}

static int ane_h18_rtkit_step(struct ane_h18 *ane, u32 chan_off, const char *tag, unsigned int *sent);

struct ane_h18_ring {
	u64 *base;		/* the ring in the IPC surface, 64 byte slots, 8 u64 each */
	u64 iova;
	u32 slots, bit, type;
	unsigned int cursor;
};

/* Look an entry of the published table up by name. The entry has to be sane: type 0 to 2, bit below 32, 1 to CH_MAX_SLOTS slots, a ring that lies inside the
 * surface that holds the table, aligned to a slot. */
static int ane_h18_chan_get(struct ane_h18 *ane, const struct ane_h18_hs_buf *tb, const u8 *tp, unsigned int i, struct ane_h18_ring *r, char *name)
{
	const u8 *e = tp + i * ANE_H18_CH_ENTRY;
	u32 type, bit;
	u64 size, ring;

	memcpy(name, e, ANE_H18_CH_NAME_LEN);
	name[ANE_H18_CH_NAME_LEN - 1] = 0;
	memcpy(&type, e + 0x40, 4);
	memcpy(&bit, e + 0x44, 4);
	memcpy(&size, e + 0x48, 8);
	memcpy(&ring, e + 0x50, 8);
	dev_info(ane->dev, "ane_h18 hs chan %u name=%s type=%u bit=%u slots=%#llx ring=%#llx\n", i, name, type, bit, size, ring);
	if (!name[0] || type > 2 || bit >= 32 || !size || size > ANE_H18_CH_MAX_SLOTS || ring < tb->iova || ring % ANE_H18_CH_SLOT ||
	    ring - tb->iova >= tb->size || size * ANE_H18_CH_SLOT > tb->size - (ring - tb->iova))
		return -EPROTO;
	r->base = (u64 *)((u8 *)tb->va + (ring - tb->iova));
	r->iova = ring;
	r->slots = size;
	r->bit = bit;
	r->type = type;
	r->cursor = 0;
	return 0;
}

static int ane_h18_chan_poll(struct ane_h18 *ane, unsigned int t, u64 *digest, u32 *last_pend);

static const u8 *ane_h18_prog_cpu(const struct ane_h18 *ane, u64 a, size_t len);

/* The published IO_T2H ring of the channel table, or NULL. */
static struct ane_h18_ring_copy *ane_h18_call_ring(struct ane_h18 *ane)
{
	struct ane_h18_ring_copy *r = NULL;
	unsigned int i;

	for (i = 0; i < ANE_H18_CH_COUNT; i++)
		if (ane->ch_rings[i].base && ane->ch_rings[i].type == 1 && !strcmp(ane->ch_names[i], "IO_T2H"))
			r = &ane->ch_rings[i];
	return r;
}

/* True when the event is the finish of our procedure: cookie at +8, state 1 at +0x1c. */
static bool ane_h18_ev_fin(const u8 *ev)
{
	u64 cookie;
	u32 state;

	memcpy(&cookie, ev + 8, 8);
	memcpy(&state, ev + 0x1c, 4);
	return cookie == ANE_H18_PROG_COOKIE && state == 1;
}

/* The latency stamp of prog_out_words: call_t0 is taken right before the doorbell write that hands PROCEDURE_CALL to the firmware. call_t1 is ktime_get_ns() of
 * the first poll iteration that read the finish event in the IO_T2H ring, from the cursor on, without logging or giving a slot back, so the console cannot delay
 * the stamp. It is NOT the hardware completion time: the firmware posted the event some unknown time before an iteration noticed it, so the latency is the call
 * as the host sees it: the doorbell latency, the firmware's run and post path, and the poll granularity. */
static void ane_h18_call_peek(struct ane_h18 *ane)
{
	struct ane_h18_ring_copy *r = ane->call_t2h && !ane->call_t1 ? ane_h18_call_ring(ane) : NULL;
	unsigned int c, n;

	if (!r)
		return;
	for (n = 0, c = *ane->call_t2h; n < r->slots; n++, c = (c + 1) % r->slots) {
		u64 hdr = READ_ONCE(r->base[(size_t)c * 8]), len;
		const u8 *ev;

		if ((hdr & 1) || hdr == 0)
			return;
		dma_rmb();
		len = READ_ONCE(r->base[(size_t)c * 8 + 1]);
		ev = len >= 0x20 && len <= 0x1000 ? ane_h18_prog_cpu(ane, hdr, len) : NULL;
		if (ev && ane_h18_ev_fin(ev)) {
			ane->call_t1 = ktime_get_ns();
			return;
		}
	}
}

/* One command on the IO ring: the command block (id at +4, optional u32 argument at +8), slot header = its DMA address, doorbell BIT(bit), then wait for the
 * firmware to give the slot back (header | 1) with a zero acknowledgement at +6. Returns the block in *blk for the reply. */
static int ane_h18_cmd_body(struct ane_h18 *ane, struct ane_h18_ring *io, u16 op, u32 len, bool arg_set, u32 arg, const u8 *body, u32 body_len, u8 **blk)
{
	u64 *slot = io->base + io->cursor * 8, hdr, dma = ane->hs_cmd.iova;
	u8 *cmd = ane->hs_cmd.va;
	unsigned int waited;
	u64 digest = 0;
	u32 last_pend = 0;
	u16 id, ack;

	ane->snap_left = ANE_H18_SNAP_PER_WAIT;

	if (!(READ_ONCE(slot[0]) & 1)) {
		dev_err(ane->dev, "ane_h18 cmd %#x: IO slot %u is not host owned (%#llx)\n", op, io->cursor, READ_ONCE(slot[0]));
		return -EBUSY;
	}
	if (len > ane->hs_cmd.size || body_len > len || (body_len && len - body_len < 8)) {
		dev_err(ane->dev, "ane_h18 cmd %#x: length %#x does not fit the block (%#zx)\n", op, len, ane->hs_cmd.size);
		return -EINVAL;
	}
	memset(cmd, 0, ane->hs_cmd.size);
	memcpy(cmd + 4, &op, 2);
	if (arg_set)
		memcpy(cmd + 8, &arg, 4);
	if (body_len)
		memcpy(cmd + len - body_len, body, body_len);
	WRITE_ONCE(slot[1], len);
	WRITE_ONCE(slot[2], len);
	dma_wmb();
	WRITE_ONCE(slot[0], dma);
	dma_wmb();
	dev_crit(ane->dev, "ane_h18 write doorbell BIT(%u) pa=%#llx for command %#x slot %u block iova %#llx\n", io->bit, ane->soc->engine_pa + ANE_H18_DOORBELL_OFF, op,
		 io->cursor, (u64)dma);
	/* call_t0 is taken here, immediately before the doorbell write itself; the log line above has already run, so the stamp carries no console time. */
	if (ane->call_t2h && op == ANE_H18_CSNE_PROCEDURE_CALL)
		ane->call_t0 = ktime_get_ns();
	writel(BIT(io->bit), ane->doorbell);
	for (waited = 0; waited < hs_wait_ms; waited++) {
		ane_h18_chan_poll(ane, waited, &digest, &last_pend);
		hdr = READ_ONCE(slot[0]);
		if (hdr & 1) {
			dma_rmb();
			memcpy(&id, cmd + 4, 2);
			memcpy(&ack, cmd + 6, 2);
			dev_info(ane->dev, "ane_h18 cmd %#x answered after %ums: header=%#llx words=%#llx,%#llx id=%#x ack=%#x\n", op, waited, hdr, READ_ONCE(slot[1]), READ_ONCE(slot[2]), id, ack);
			io->cursor = (io->cursor + 1) % io->slots;
			*blk = cmd;
			if (!(hdr == (dma | 1) && READ_ONCE(slot[1]) == len && READ_ONCE(slot[2]) == 0 && id == op))
				return -EPROTO;	/* not the answer to this command */
			return ack ? -EREMOTEIO : 0;	/* the firmware answered and refused */
		}
		if (ane->call_t2h && op == ANE_H18_CSNE_PROCEDURE_CALL) {
			u64 end = ktime_get_ns() + NSEC_PER_MSEC;	/* a timed call answers the firmware and watches for the event instead of sleeping */

			while (!(READ_ONCE(slot[0]) & 1) && ktime_get_ns() < end) {
				ane_h18_chan_poll(ane, 1, &digest, &last_pend);
				ane_h18_call_peek(ane);
				cpu_relax();
				cond_resched();	/* process context with only the debugfs mutex held; hs_wait_ms can be long */
			}
		} else {
			usleep_range(500, 1000);
		}
	}
	dev_err(ane->dev, "ane_h18 cmd %#x: no answer within %ums (header %#llx)\n", op, hs_wait_ms, READ_ONCE(slot[0]));
	return -ETIMEDOUT;
}

static int ane_h18_cmd(struct ane_h18 *ane, struct ane_h18_ring *io, u16 op, u32 len, bool arg_set, u32 arg, u8 **blk)
{
	return ane_h18_cmd_body(ane, io, op, len, arg_set, arg, NULL, 0, blk);
}

/* Answer the entry of the SHAREDMALLOC ring at the cursor, if the firmware posted one. 1 = answered, 0 = nothing, negative = refused (the firmware then waits). */
static int ane_h18_mal_service(struct ane_h18 *ane)
{
	u64 *slot, hdr, size, mode;
	struct ane_h18_hs_buf *b;
	unsigned int i;
	int ret;

	if (!ane->mal_ring)
		return 0;
	if (ane->mal_err)
		return ane->mal_err;
	slot = ane->mal_ring + ane->mal_cursor * 8;
	hdr = READ_ONCE(slot[0]);
	if (hdr & 1)
		return 0;
	dma_rmb();
	size = READ_ONCE(slot[1]);
	mode = READ_ONCE(slot[2]);
	switch (ane_h18_mal_classify(hdr, size, mode)) {
	case ANE_H18_MAL_MALLOC:
		/* the first free slot (a FREEd buffer's), else a new one */
		for (i = 0; i < ane->hs_mal_n; i++)
			if (!ane->hs_mal[i].va)
				break;
		if (size % PAGE_SIZE || size > ANE_H18_MAL_MAX_SIZE || i >= ANE_H18_MAL_MAX_BUFS || ane->hs_mal_bytes + size > ANE_H18_MAL_MAX_BYTES) {
			dev_err(ane->dev, "ane_h18 fw MALLOC of %#llx bytes refused (%u buffers, %#zx bytes so far)\n", size, ane->hs_mal_n, ane->hs_mal_bytes);
			ane->mal_err = -E2BIG;
			return ane->mal_err;
		}
		b = &ane->hs_mal[i];
		ret = ane_h18_hs_alloc(ane, b, size, "FWMALLOC");
		if (ret) {
			if (b->va) {
				dma_free_coherent(ane->dev, b->size, b->va, b->iova);
				b->va = NULL;
			}
			ane->mal_err = ret;
			return ret;
		}
		if (i == ane->hs_mal_n)
			ane->hs_mal_n++;
		ane->hs_mal_bytes += size;
		dev_info(ane->dev, "ane_h18 fw MALLOC %#llx bytes mode %llu -> iova %#llx ordinal %u\n", size, mode, (u64)b->iova, i + 1);
		WRITE_ONCE(slot[1], 0);
		WRITE_ONCE(slot[2], i + 1);
		dma_wmb();
		WRITE_ONCE(slot[0], b->iova | 1);
		break;
	case ANE_H18_MAL_FREE:
		dev_info(ane->dev, "ane_h18 fw FREE of iova %#llx\n", hdr);
		WRITE_ONCE(slot[1], 0);
		WRITE_ONCE(slot[2], 0);
		dma_wmb();
		WRITE_ONCE(slot[0], 1);
		dma_wmb();
		dev_crit(ane->dev, "ane_h18 write doorbell BIT(%u) pa=%#llx to answer the FREE\n", ane->mal_bit, ane->soc->engine_pa + ANE_H18_DOORBELL_OFF);
		writel(BIT(ane->mal_bit), ane->doorbell);
		for (i = 0; i < ane->hs_mal_n; i++)
			if (ane->hs_mal[i].va && ane->hs_mal[i].iova == hdr) {
				dma_free_coherent(ane->dev, ane->hs_mal[i].size, ane->hs_mal[i].va, ane->hs_mal[i].iova);
				ane->hs_mal_bytes -= ane->hs_mal[i].size;
				ane->hs_mal[i].va = NULL;
				break;
			}
		ane->mal_cursor = (ane->mal_cursor + 1) % ane->mal_slots;
		return 1;
	case ANE_H18_MAL_IDLE:
	case ANE_H18_MAL_ANSWERED:
		return 0;
	default:
		dev_err(ane->dev, "ane_h18 SHAREDMALLOC slot %u is not a request: %#llx %#llx %#llx\n", ane->mal_cursor, hdr, size, mode);
		ane->mal_err = -EOPNOTSUPP;
		return ane->mal_err;
	}
	dma_wmb();
	dev_crit(ane->dev, "ane_h18 write doorbell BIT(%u) pa=%#llx to answer the MALLOC\n", ane->mal_bit, ane->soc->engine_pa + ANE_H18_DOORBELL_OFF);
	writel(BIT(ane->mal_bit), ane->doorbell);
	ane->mal_cursor = (ane->mal_cursor + 1) % ane->mal_slots;
	return 1;
}

static void ane_h18_chan_snapshot(struct ane_h18 *ane, const char *when, bool full);

/* One poll of the doorbell side while the host waits: answer the SHAREDMALLOC ring, acknowledge a pending interrupt the way macOS does (the value read is
 * written back to the ack register) and, when the pending value or a firmware-to-host ring changed, print the snapshot. `t` is the poll number in ms. */
static int ane_h18_chan_poll(struct ane_h18 *ane, unsigned int t, u64 *digest, u32 *last_pend)
{
	unsigned int i, k;
	u64 d = 0;
	u32 pend = ane->ipi_pending ? readl(ane->ipi_pending) : 0;
	int served, ret = 0;

	if (pend) {
		if (ane->ack_logs++ < 8)
			dev_crit(ane->dev, "ane_h18 write IPI ack=%#x pa=%#llx\n", pend, ane->soc->engine_pa + ANE_H18_IPI_ACK_OFF);
		writel(pend, ane->ipi_ack);
	}
	do {
		served = ane_h18_mal_service(ane);
		if (served < 0)
			ret = served;
	} while (served > 0);
	if (t % 10)
		return ret;
	for (i = 0; i < ANE_H18_CH_COUNT; i++)
		if (ane->ch_rings[i].type != 0)
			for (k = 0; k < ane->ch_rings[i].slots * 8; k++)
				d = (d << 7 | d >> 57) ^ READ_ONCE(ane->ch_rings[i].base[k]);
	if (d != *digest || pend != *last_pend) {
		*digest = d;
		*last_pend = pend;
		if (ane->snap_left) {
			ane->snap_left--;
			ane_h18_chan_snapshot(ane, "ring change", false);
		}
	}
	return ret;
}

/* True when the failpath recovery left the ANE domain powered down: no MMIO read of the ANE may run while it is. The one decision, so every post-failure
 * reader asks it instead of each open-coding its own gate. */
static bool ane_h18_domain_gone(struct ane_h18 *ane)
{
	if (!ane->domain_down)
		return false;
	dev_info(ane->dev, "ane_h18: the ANE domain is down, register reads are skipped\n");
	return true;
}

/* Read only look at the rings and the IPI pending register: what the firmware posted that nobody answered. A nonzero line of a firmware-to-host ring or the
 * terminal ring is logged (at most 24 lines of 16 bytes per ring, 4 when `full` is false, the rest counted), the first two slots of every host-to-firmware ring
 * are logged, and so is the pending register. With `full` the terminal ring text is logged too: the change snapshots leave it out, six of them with the text
 * overflowed the kernel log ring on the board. Written by the firmware or by us: the log says which ring. Nothing is written. */
static void ane_h18_chan_snapshot(struct ane_h18 *ane, const char *when, bool full)
{
	char (*names)[0x40] = ane->ch_names;
	struct ane_h18_ring_copy *rings = ane->ch_rings;
	unsigned int i, k, lines, more;
	u32 pend = 0;

	if (ane_h18_domain_gone(ane))
		return;
	if (ane->ipi_pending) {
		dev_info(ane->dev, "ane_h18 chan snapshot %s: read IPI pending pa=%#llx\n", when, ane->soc->engine_pa + ANE_H18_IPI_PENDING_OFF);
		pend = readl(ane->ipi_pending);
	}
	dev_info(ane->dev, "ane_h18 chan snapshot %s: IPI pending = %#x\n", when, pend);
	for (i = 0; i < ANE_H18_CH_COUNT; i++) {
		const u64 *r = rings[i].base;

		if (rings[i].type == 0) {
			for (k = 0; k < 2 && k < rings[i].slots; k++)
				dev_info(ane->dev, "ane_h18 chan snapshot %s: %s slot %u = %#llx %#llx %#llx\n", when, names[i], k, READ_ONCE(r[k * 8]), READ_ONCE(r[k * 8 + 1]), READ_ONCE(r[k * 8 + 2]));
			continue;
		}
		if (rings[i].type == 2 && full) {
			/* the terminal ring is the firmware console: print its text, one line per nonzero 64 byte slot (the firmware's own bytes, non-printable ones as '.') */
			unsigned int shown = 0;

			for (k = 0; k < rings[i].slots; k++) {
				const u8 *e = (const u8 *)(r + k * 8);
				char txt[65];
				unsigned int j;
				bool any = false;

				for (j = 0; j < 64; j++) {
					txt[j] = e[j] >= 0x20 && e[j] < 0x7f ? e[j] : '.';
					any |= e[j] != 0;
				}
				txt[64] = 0;
				if (any && shown++ < 32)
					dev_info(ane->dev, "ane_h18 chan snapshot %s: %s text slot %u: %s\n", when, names[i], k, txt);
			}
			dev_info(ane->dev, "ane_h18 chan snapshot %s: %s has %u nonzero slots as text\n", when, names[i], shown);
		}
		for (k = 0, lines = 0, more = 0; k < rings[i].slots * 4; k++) {	/* 16 byte lines of the ring */
			u64 a = READ_ONCE(r[k * 2]), b = READ_ONCE(r[k * 2 + 1]);

			if (!(a | b))
				continue;
			if (lines++ < (full ? 24 : 4))
				dev_info(ane->dev, "ane_h18 chan snapshot %s: %s +%#x = %#llx %#llx\n", when, names[i], k * 16, a, b);
			else
				more++;
		}
		dev_info(ane->dev, "ane_h18 chan snapshot %s: %s has %u nonzero lines, %u not printed\n", when, names[i], lines, more);
	}
}


/* The program file (ane-prog-pack), little endian:
 *   +0x00 u32 magic 'ANEP', u32 version 1, u32 nsec (1 to 9), u32 nio (1 to 8), u64 file size, u64 reserved
 *   +0x20 nsec records of 0x20 bytes: u32 section id (1 to 9, unique), u32 0, u64 size, u64 offset (16 byte aligned), u64 0
 *   then  nio records of 0x20 bytes: u32 buffer id (unique), u32 direction (0 input, 1 output, 2 scratch), u64 size, u64 data offset, u64 data size (0 = zero filled)
 * Every size and offset is checked against the file and the limits before anything is allocated. Returns 0 or -EINVAL with *why set. */
static int ane_h18_prog_parse(const u8 *d, size_t n, struct ane_h18_pdesc *p, const char **why)
{
	u32 magic, version, nsec, nio, i, j;
	u64 fsize, total = 0;
	size_t pos;

	memset(p, 0, sizeof(*p));
	if (n < 0x20) {
		*why = "file shorter than its header";
		return -EINVAL;
	}
	memcpy(&magic, d, 4);
	memcpy(&version, d + 4, 4);
	memcpy(&nsec, d + 8, 4);
	memcpy(&nio, d + 12, 4);
	memcpy(&fsize, d + 16, 8);
	if (magic != ANE_H18_PROG_MAGIC || version != ANE_H18_PROG_VERSION) {
		*why = "not an ANEP version 1 file";
		return -EINVAL;
	}
	if (fsize != n) {
		*why = "the size in the header is not the file size";
		return -EINVAL;
	}
	if (!nsec || nsec > ANE_H18_PROG_MAX_SECS || !nio || nio > ANE_H18_PROG_MAX_IO) {
		*why = "section or buffer count out of range";
		return -EINVAL;
	}
	pos = 0x20 + (size_t)(nsec + nio) * 0x20;
	if (pos > n) {
		*why = "the record tables do not fit in the file";
		return -EINVAL;
	}
	p->nsec = nsec;
	p->nio = nio;
	for (i = 0; i < nsec; i++) {
		const u8 *r = d + 0x20 + (size_t)i * 0x20;
		u64 size, off;
		u32 id;

		memcpy(&id, r, 4);
		memcpy(&size, r + 8, 8);
		memcpy(&off, r + 16, 8);
		if (id < 1 || id > ANE_H18_PROG_MAX_SECS || !size || size > ANE_H18_PROG_MAX_BUF || off % 16 || off < pos || off > n || size > n - off) {
			*why = "a section record is out of range";
			return -EINVAL;
		}
		for (j = 0; j < i; j++)
			if (p->sec[j].id == id) {
				*why = "two sections with the same id";
				return -EINVAL;
			}
		p->sec[i].id = id;
		p->sec[i].size = size;
		p->sec[i].off = off;
		total += size;
	}
	for (i = 0; i < nio; i++) {
		const u8 *r = d + 0x20 + ((size_t)nsec + i) * 0x20;
		u64 size, doff, dsize;
		u32 id, dir;

		memcpy(&id, r, 4);
		memcpy(&dir, r + 4, 4);
		memcpy(&size, r + 8, 8);
		memcpy(&doff, r + 16, 8);
		memcpy(&dsize, r + 24, 8);
		if (dir > 2 || !size || size > ANE_H18_PROG_MAX_BUF || dsize > size || (dsize && (doff > n || dsize > n - doff))) {
			*why = "a buffer record is out of range";
			return -EINVAL;
		}
		for (j = 0; j < i; j++)
			if (p->io[j].id == id) {
				*why = "two buffers with the same id";
				return -EINVAL;
			}
		p->io[i].id = id;
		p->io[i].dir = dir;
		p->io[i].size = size;
		p->io[i].doff = doff;
		p->io[i].dsize = dsize;
		total += size;
	}
	if (total > ANE_H18_PROG_MAX_BYTES) {
		*why = "the buffers together are over the limit";
		return -EINVAL;
	}
	return 0;
}

/* Log the first `n` bytes of a reply or event, 16 bytes a line. */
static void ane_h18_prog_dump(struct ane_h18 *ane, const char *what, const u8 *p, unsigned int n)
{
	unsigned int k;

	for (k = 0; k < n; k += 16) {
		u32 q[4] = { 0 };

		memcpy(q, p + k, n - k < 16 ? n - k : 16);
		dev_info(ane->dev, "ane_h18 prog: %s +%#x: %08x %08x %08x %08x\n", what, k, q[0], q[1], q[2], q[3]);
	}
}

/* The CPU address of `len` bytes at a firmware address inside one of the buffers the firmware was given, or NULL. */
static const u8 *ane_h18_prog_cpu(const struct ane_h18 *ane, u64 a, size_t len)
{
	const struct ane_h18_hs_buf *b[3] = { &ane->hs_pool, &ane->hs_ipc, &ane->hs_heap };
	unsigned int i;

	for (i = 0; i < ANE_H18_MAL_MAX_BUFS && i < ane->hs_mal_n; i++)
		if (ane->hs_mal[i].va && a >= ane->hs_mal[i].iova && a - ane->hs_mal[i].iova + len <= ane->hs_mal[i].size)
			return (u8 *)ane->hs_mal[i].va + (a - ane->hs_mal[i].iova);
	for (i = 0; i < 3; i++)
		if (b[i]->va && a >= b[i]->iova && a - b[i]->iova + len <= b[i]->size)
			return (u8 *)b[i]->va + (a - b[i]->iova);
	return NULL;
}

/* The firmware console as text. A record of the terminal ring is 64 bytes: word 0 the firmware address of the text (the low two bits are flags, clear in the address: every record seen so far had 2, and the lookup masks them), word 1 the length,
 * word 3 a timestamp (seen on the board; the format is not documented anywhere). The text lies in a buffer the firmware was given, so the host can read it.
 * One log line per record, slot number first: the ring wraps, the timestamps give the order. Records that do not point into one of our buffers, or whose
 * length is over 0x100, are counted and not printed. Read only. */
static void ane_h18_term_log(struct ane_h18 *ane, const char *when)
{
	unsigned int i, k, shown = 0, skipped = 0;

	for (i = 0; i < ANE_H18_CH_COUNT; i++) {
		const u64 *r = ane->ch_rings[i].base;

		if (ane->ch_rings[i].type != 2 || !r)
			continue;
		for (k = 0; k < ane->ch_rings[i].slots; k++) {
			u64 w0 = READ_ONCE(r[k * 8]), len = READ_ONCE(r[k * 8 + 1]), ts = READ_ONCE(r[k * 8 + 3]);
			const u8 *t;
			char txt[0x101];
			unsigned int j;

			if (!w0)
				continue;
			t = len && len <= 0x100 ? ane_h18_prog_cpu(ane, w0 & ~3ULL, len) : NULL;
			if (!t) {
				skipped++;
				continue;
			}
			for (j = 0; j < len; j++) {
				u8 c = READ_ONCE(t[j]);	/* the firmware may write while we read: one load per byte */

				txt[j] = c >= 0x20 && c < 0x7f ? c : (c == '\n' ? ' ' : '.');
			}
			txt[len] = 0;
			dev_info(ane->dev, "ane_h18 fw log %s: %u ts %#llx: %s\n", when, k, ts, txt);
			if (shown < 4) {	/* the first records again as raw bytes: tells text from a format reference */
				char hex[65];
				unsigned int n = len < 32 ? len : 32;

				for (j = 0; j < n; j++) {
					hex[j * 2] = "0123456789abcdef"[READ_ONCE(t[j]) >> 4];
					hex[j * 2 + 1] = "0123456789abcdef"[READ_ONCE(t[j]) & 15];
				}
				hex[n * 2] = 0;
				dev_info(ane->dev, "ane_h18 fw log %s: %u raw %u bytes: %s\n", when, k, n, hex);
			}
			shown++;
		}
	}
	dev_info(ane->dev, "ane_h18 fw log %s: %u records printed, %u not (no buffer behind the address, or length 0 or over 0x100)\n", when, shown, skipped);
}

/* Give every firmware-owned slot of the IO_T2H ring back (header | 1, doorbell BIT(ring bit)), as the T6021 driver does, and log the event each one points to
 * (0x28 bytes: u32 sequence, u32 0x300, u64 cookie, u32 program, u32 process, u32 0, u32 state; two per call, state 0 and then 1 when the procedure finished).
 * Returns the number of events taken and sets *finished when an event of our cookie had state 1. Nothing is written unless the firmware posted a slot. */
static unsigned int ane_h18_prog_t2h(struct ane_h18 *ane, unsigned int *cursor, bool *finished)
{
	const struct ane_h18_ring_copy *r = NULL;
	unsigned int i, n = 0;

	for (i = 0; i < ANE_H18_CH_COUNT; i++)
		if (ane->ch_rings[i].base && ane->ch_rings[i].type == 1 && !strcmp(ane->ch_names[i], "IO_T2H"))
			r = &ane->ch_rings[i];
	if (!r)
		return 0;
	/* Linear cursor over the firmware's channel-table ring: 64-byte records, r->slots records,
	 * wrap at r->slots. The debug probe in the wait loop prints every record so a desync between
	 * this cursor and the firmware's publish sequence shows in the trace. */
	while (n < r->slots) {
		u64 *slot = r->base + (size_t)*cursor * 8, hdr = READ_ONCE(slot[0]), len;
		const u8 *ev;

		if ((hdr & 1) || !hdr)	/* host owned (given back, or the firmware's marker 1) or not published yet */
			break;
		dma_rmb();
		len = READ_ONCE(slot[1]);
		dev_info(ane->dev, "ane_h18 prog: IO_T2H slot %u header=%#llx len=%#llx\n", *cursor, hdr, len);
		ev = len >= 0x20 && len <= 0x1000 ? ane_h18_prog_cpu(ane, hdr, len) : NULL;
		if (ev) {
			u64 cookie;
			u32 state;

			ane_h18_prog_dump(ane, "event", ev, len < 0x40 ? len : 0x40);
			memcpy(&cookie, ev + 8, 8);
			memcpy(&state, ev + 0x1c, 4);
			if (cookie == ANE_H18_PROG_COOKIE && state == 1)
				*finished = true;
		} else {
			dev_warn(ane->dev, "ane_h18 prog: the event address %#llx is not in a buffer the firmware was given, slot given back unread\n", hdr);
		}
		WRITE_ONCE(slot[0], hdr | 1);
		dma_wmb();
		dev_crit(ane->dev, "ane_h18 write doorbell BIT(%u) pa=%#llx to give IO_T2H slot %u back\n", r->bit, ane->soc->engine_pa + ANE_H18_DOORBELL_OFF, *cursor);
		writel(BIT(r->bit), ane->doorbell);
		*cursor = (*cursor + 1) % r->slots;
		n++;
	}
	return n;
}

/* Keep answering the firmware for `ms` ms (memory requests, T2H events). */
static void ane_h18_prog_settle(struct ane_h18 *ane, unsigned int ms, unsigned int *t2h_cursor)
{
	u64 digest = 0;
	u32 last_pend = 0;
	bool fin = false;
	unsigned int k;

	ane->snap_left = ANE_H18_SNAP_PER_WAIT;
	for (k = 0; k < ms; k++) {
		usleep_range(1000, 1100);
		ane_h18_chan_poll(ane, k + 1, &digest, &last_pend);
		ane_h18_prog_t2h(ane, t2h_cursor, &fin);
	}
}

/* Read only: the exception state of the ANE DART instance 0, in the layout the Linux apple-dart gen3 handler reads: the error word and its address (0x100, 0x104,
 * 0x170, 0x174, 0x1c0), the eight words of the bitmap of streams with a pending exception (0x4000 + 4 * w, one bit per stream), and for the first
 * ANE_H18_DART_EXC_SIDS streams with a bit set their record at 0x8000 + 0x40 * sid (status, +4, address lo and hi, +0x10, AXI id and user words at +0x18 to +0x24). Needs read_dart=1
 * (the DART window is mapped). The first run of a program on the M6 ended in an unhandled DART interrupt: the kernel handler ignores the streams it does not own, so this
 * is where the stream and the address of the fault show. Nothing is written. */
static void ane_h18_dart_inst(struct ane_h18 *ane, unsigned int k, const char *when);

static void ane_h18_dart_exc(struct ane_h18 *ane, const char *when)
{
	unsigned int k;

	for (k = 0; k < ANE_H18_DART_INSTANCES; k++)
		if (dart_inst & BIT(k))
			ane_h18_dart_inst(ane, k, when);
	static const u32 fixed[] = { 0x100, 0x104, 0x170, 0x174, 0x1c0 };
	static const u32 rec[] = { 0x0, 0x4, 0x8, 0xc, 0x10, 0x18, 0x1c, 0x20, 0x24 };
	u32 pend[ANE_H18_DART_EXC_WORDS], i, w, bit, shown = 0;
	const struct ane_h18_soc *s = ane->soc;

	if (!ane->dart) {
		dev_info(ane->dev, "ane_h18 prog: DART exception state (%s) not read: read_dart=0\n", when);
		return;
	}
	dev_crit(ane->dev, "ane_h18 prog: reading the ANE DART exception state (%s), pa=%#llx..%#llx, read only\n", when, s->dart_pa + fixed[0],
		 s->dart_pa + 0x8000 + 0x40 * (u64)ANE_H18_DART_EXC_MAXSID);
	for (i = 0; i < ARRAY_SIZE(fixed); i++)
		dev_info(ane->dev, "ane_h18 prog: DART exc %s +%#x = %#x\n", when, fixed[i], readl(ane->dart + fixed[i]));
	for (w = 0; w < ANE_H18_DART_EXC_WORDS; w++) {
		pend[w] = readl(ane->dart + 0x4000 + 4 * w);
		dev_info(ane->dev, "ane_h18 prog: DART exc %s bitmap word %u (+%#x) = %#x\n", when, w, 0x4000 + 4 * w, pend[w]);
	}
	for (w = 0; w < ANE_H18_DART_EXC_WORDS && shown < ANE_H18_DART_EXC_SIDS; w++)
		for (bit = 0; bit < 32 && shown < ANE_H18_DART_EXC_SIDS; bit++) {
			u32 sid = w * 32 + bit, v[ARRAY_SIZE(rec)];

			if (!(pend[w] & BIT(bit)) || sid >= ANE_H18_DART_EXC_MAXSID)
				continue;
			for (i = 0; i < ARRAY_SIZE(rec); i++)
				v[i] = readl(ane->dart + 0x8000 + 0x40 * sid + rec[i]);
			dev_info(ane->dev, "ane_h18 prog: DART exc %s stream %u: status %#x +4 %#x addr %#llx +0x10 %#x axi id %#x/%#x user %#x/%#x\n", when, sid, v[0], v[1],
				 (u64)v[3] << 32 | v[2], v[4], v[5], v[6], v[7], v[8]);
			shown++;
		}
}

/* Read only: ENABLE_STREAMS, TCR, TTBR, the first two words of the exception bitmap and the records of streams 0, 10, 11, 13 and 15 of DART instance k (0 to 3), the
 * layout the shipped system reads per instance. +0x704 is not read: the fork's own comment says it is outside the gen3 capture list. Needs the instance mapped. */
static void ane_h18_dart_inst(struct ane_h18 *ane, unsigned int k, const char *when)
{
	static const u32 regs[] = { 0xc00, 0x1000, 0x1400, 0x4000, 0x4004 };
	static const u32 sids[] = { 0, 10, 11, 13, 15 };
	static const u32 rec[] = { 0x0, 0x4, 0x8, 0xc, 0x10 };
	void __iomem *base = k ? ane->dartx[k] : ane->dart;
	u64 pa = ane->soc->dart_pa + (u64)k * ANE_H18_DART_INST_STRIDE;
	unsigned int i, j;

	if (!base) {
		dev_info(ane->dev, "ane_h18 prog: DART instance %u (%s) not mapped, not read\n", k, when);
		return;
	}
	dev_crit(ane->dev, "ane_h18 prog: reading DART instance %u (%s), pa=%#llx +0xc00..+0x4004 and +0x8000 + 0x40 * stream for streams 0, 10, 11, 13, 15, read only\n", k, when, pa);
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		dev_info(ane->dev, "ane_h18 prog: DART %u %s +%#x = %#x\n", k, when, regs[i], readl(base + regs[i]));
	for (i = 0; i < ARRAY_SIZE(sids); i++) {
		u32 v[ARRAY_SIZE(rec)];

		for (j = 0; j < ARRAY_SIZE(rec); j++)
			v[j] = readl(base + 0x8000 + 0x40 * sids[i] + rec[j]);
		dev_info(ane->dev, "ane_h18 prog: DART %u %s stream %u: status %#x +4 %#x addr %#llx +0x10 %#x\n", k, when, sids[i], v[0], v[1], (u64)v[3] << 32 | v[2], v[4]);
	}
}

/* PREMAP_BUFFER (0x20d), as the shipped system sends it at init for the inference request pool: a 0x3490 byte command, type 2, one request of 0x3898 bytes in a surface of the
 * DART's page size. Source of the layout: macOS behaviour on build 26A434 (mac18g), recorded in the M6
 * engine DMA lab note, section 4, sha256 eac93039f9c3f62883f83c2b563dda7b47e0d3b19e2fca2bd450527f0d8d6f60.
 * The pool is count x 0x3898 bytes rounded to the DART page.
 * The layout is: +8 1, +0x10 type, +0x54 1, +0x58 1, +0x60 4, +0x68 1, +0x70 the pool address, +0x78 a second value (premap_second, or the
 * pool size: which of the two values macOS passes is not known). A refusal (acknowledgement not zero) is logged and the program stage goes on. */
static int ane_h18_premap(struct ane_h18 *ane, struct ane_h18_ring *io)
{
	struct ane_h18_hs_buf *b = &ane->pg_pool;
	u64 second, addr, one = 1;
	u32 u1 = 1, type = 2, four = 4;
	u8 *cmd, *blk;
	int ret;

	ret = ane_h18_hs_alloc(ane, b, ALIGN(ANE_H18_PREMAP_POOL, SZ_16K), "PREMAP pool");
	if (ret)
		return ret;
	cmd = kzalloc(ANE_H18_PREMAP_LEN, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;
	second = premap_second ? premap_second : b->size;
	addr = b->iova;
	memcpy(cmd + 8, &u1, 4);
	memcpy(cmd + 0x10, &type, 4);
	memcpy(cmd + 0x54, &u1, 4);
	memcpy(cmd + 0x58, &u1, 4);
	memcpy(cmd + 0x60, &four, 4);
	memcpy(cmd + 0x68, &one, 8);
	memcpy(cmd + 0x70, &addr, 8);
	memcpy(cmd + 0x78, &second, 8);
	dev_info(ane->dev, "ane_h18 prog: PREMAP_BUFFER pool iova=%#llx size=%#zx second=%#llx\n", (u64)b->iova, b->size, second);
	ret = ane_h18_cmd_body(ane, io, ANE_H18_CSNE_PREMAP_BUFFER, ANE_H18_PREMAP_LEN, false, 0, cmd + 8, ANE_H18_PREMAP_LEN - 8, &blk);
	kfree(cmd);
	ane_h18_prog_dump(ane, "PREMAP_BUFFER reply", (u8 *)ane->hs_cmd.va, 0x80);
	return ret;
}

/* Start the core and service the mailbox channels while the firmware comes up: its RTKit HELLO and the endpoint maps arrive there and are answered. Reports
 * through *running whether the core reported running and adds the messages it handled to *msgs. Returns the errno of a failed read, 0 otherwise. */
static int ane_h18_core_start(struct ane_h18 *ane, unsigned int *msgs, bool *running)
{
	const struct ane_h18_soc *s = ane->soc;
	u32 ctrl = 0, st = 0, last = 0xffffffffu, po, chan_off;
	unsigned int i, waited, sent = 0;
	int ret;

	ret = ane_h18_read(ane, "CPU_CONTROL", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_CONTROL, s->asc_size, &ctrl);
	if (ret)
		return ret;
	dev_crit(ane->dev, "ane_h18 write CPU_CONTROL=%#x pa=%#llx (RUN)\n", ctrl | ANE_H18_CPU_RUN, s->asc_pa + ANE_H18_ASC_CPU_CONTROL);
	ane->fw_started = true;
	writel(ctrl | ANE_H18_CPU_RUN, ane->asc + ANE_H18_ASC_CPU_CONTROL);
	for (waited = 0; waited < boot_wait_ms; waited += 10) {
		usleep_range(10000, 11000);
		ret = ane_h18_read(ane, "CPU_STATUS", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_STATUS, s->asc_size, &st);
		if (ret)
			return ret;
		if (st & ANE_H18_ST_RUNNING)
			*running = true;
		if (st != last) {
			dev_info(ane->dev, "ane_h18 t+%ums cpu_status=%#x\n", waited + 10, st);
			last = st;
		}
		for (i = 0; i < ANE_H18_PO_NCAND + (alias_mbox ? 1 : 0) && *msgs < ANE_H18_PO_MAXMSG; i++) {
			chan_off = i < ANE_H18_PO_NCAND ? ane_h18_po_chan(s, i) * ANE_H18_PO_STRIDE : ANE_H18_PO_ALIAS_OFF + s->mbox_num * 2 * ANE_H18_PO_STRIDE;
			char tag[16];

			if ((u64)chan_off + ANE_H18_PO_STATUS + 4 > s->mbox_size)
				continue;
			if (i < ANE_H18_PO_NCAND)
				snprintf(tag, sizeof(tag), "ch%u", ane_h18_po_chan(s, i));
			else
				snprintf(tag, sizeof(tag), "alias%u", s->mbox_num * 2);
			if (i >= ANE_H18_PO_NCAND) {
				*msgs += ane_h18_rtkit_step(ane, chan_off, tag, &sent);
				continue;
			}
			po = readl(ane->mbox + chan_off + ANE_H18_PO_STATUS);
			*msgs += ane_h18_po_pop(ane, chan_off, tag, po);
		}
	}
	return 0;
}

/* The firmware did not answer the unload: power-cycle the ANE domain through the PSD this module owns. The firmware resets with the domain, so the program and
 * the process are gone, and the core runs again so the RTKit exchange starts over without a reboot. */
static void ane_h18_failpath_recover(struct ane_h18 *ane)
{
	unsigned int msgs = 0;
	bool running = false;
	int failed;

	failed = ane_h18_psd_off(ane);
	ane->domain_down = true;
	dev_err(ane->dev, "ane_h18 prog: the firmware did not answer the unload, the ANE domain was powered down (%d slots did not acknowledge)\n", failed);
	msleep(100);
	if (ane_h18_psd_up(ane, "recover")) {
		dev_err(ane->dev, "ane_h18 prog: the ANE domain did not come back up, the program may still be resident\n");
		return;
	}
	ane->domain_down = false;
	ane->rtk_ap_on = false;
	ane->rtk_ap_acked = false;
	ane->rtk_iop_acked = false;
	ane->rtk_iop_idle = false;
	if (ane_h18_core_start(ane, &msgs, &running))
		dev_err(ane->dev, "ane_h18 prog: the core did not start again after the power cycle\n");
	else
		dev_info(ane->dev, "ane_h18 prog: the ANE domain is back up, the firmware was reset and the RTKit exchange started again\n");
	ane->prog_cleaned = true;
}

/* One program on the IO ring after START: allocate the section and buffer surfaces from the file, LOAD_PROGRAM, CREATE_PROCESS, PROCEDURE_CALL, wait for the
 * finish event, then TERMINATE_PROCESS and DESTROY_PROGRAM. Every reply is logged whole. A command the firmware refuses (acknowledgement not zero) or does not
 * answer ends the sequence; the program and the process are unloaded anyway, and a firmware that does not answer the unload is power-cycled and reset. On the
 * M6 the recovery has only been seen to work this far: the matvec program still ended core-not-stopped after the power cycle, so a stuck core is not yet
 * proven clearable without a reboot. The wire format is the T6021's, as macOS behaviour on the T8152 gives the same command ids and sizes. The H14 add
 * program runs on the T8152; the matvec program's call never finishes (known failure, cause unknown). */
static int ane_h18_prog_run(struct ane_h18 *ane, struct ane_h18_ring *io, unsigned int pass, unsigned int calls, unsigned int *t2h_cursor)
{
	const struct ane_h18_pdesc *p = &ane->pd;
	const u8 *fw = ane->prog_blob->data;
	u8 cmd[ANE_H18_PROG_BLOCK], *blk;
	unsigned int i, got = 0, waited;
	u32 prog = ~0u, proc = ~0u, w;
	u64 t0;
	bool fin = false, created = false, loaded = false;
	int ret;

	for (i = 0; i < p->nsec; i++) {
		struct ane_h18_hs_buf *b = &ane->pg_sec[i];
		char nm[24];

		snprintf(nm, sizeof(nm), "PROG sec %u", p->sec[i].id);
		ret = ane_h18_hs_alloc_at(ane, b, ALIGN(p->sec[i].size, prog_iova_text && p->sec[i].id == 3 ? 0x8000 : SZ_16K), nm,
					  prog_iova_text && p->sec[i].id == 3 ? prog_iova_text : 0);
		if (ret)
			return ret;
		memcpy(b->va, fw + p->sec[i].off, p->sec[i].size);
	}
	if (prog_text_const || prog_iova_kern) {
		const u64 cbytes = 0x4000;
		unsigned int s2 = p->nsec, s3 = p->nsec;

		for (i = 0; i < p->nsec; i++) {
			if (p->sec[i].id == 2)
				s2 = i;
			if (p->sec[i].id == 3)
				s3 = i;
		}
		if (s2 == p->nsec || p->sec[s2].size < cbytes)
			return -EINVAL;
		if (prog_text_const) {
			if (!prog_iova_text || s3 == p->nsec || (prog_text_const & 7) || prog_text_const + cbytes > 0x8000 ||
			    p->sec[s3].size > prog_text_const)
				return -EINVAL;
			memcpy((u8 *)ane->pg_sec[s3].va + prog_text_const, fw + p->sec[s2].off, cbytes);
		}
		if (prog_iova_kern && p->sec[s2].size <= cbytes) {
			dev_err(ane->dev, "ane_h18 prog: prog_iova_kern is set but section 2 (%#llx bytes) has nothing behind its first %#llx bytes\n", (u64)p->sec[s2].size, cbytes);
			return -EINVAL;
		}
		if (prog_iova_kern) {
			u64 ksz = p->sec[s2].size - cbytes;

			ret = ane_h18_hs_alloc_at(ane, &ane->pg_kern, ALIGN(ksz, SZ_16K), "PROG kern", prog_iova_kern);
			if (ret)
				return ret;
			memcpy(ane->pg_kern.va, fw + p->sec[s2].off + cbytes, ksz);
		}
	}
	for (i = 0; i < p->nio; i++) {
		struct ane_h18_hs_buf *b = &ane->pg_io[i];
		char nm[24];

		snprintf(nm, sizeof(nm), "PROG buf %u", p->io[i].id);
		ret = ane_h18_hs_alloc_at(ane, b, ALIGN(p->io[i].size, SZ_16K), nm, ane_h18_io_at(p, i));
		if (ret)
			return ret;
		if (p->io[i].dsize)
			memcpy(b->va, fw + p->io[i].doff, p->io[i].dsize);
	}
	wmb();
	ane_h18_dart_exc(ane, "before LOAD_PROGRAM");
	if (hs_premap) {
		ret = ane_h18_premap(ane, io);
		if (ret == -EREMOTEIO) {
			dev_warn(ane->dev, "ane_h18 prog: PREMAP_BUFFER was refused, going on with LOAD_PROGRAM\n");
			ret = 0;
		} else if (ret) {
			goto fail;
		}
		ane_h18_prog_settle(ane, 20, t2h_cursor);
	}

	/* LOAD_PROGRAM */
	memset(cmd, 0, sizeof(cmd));
	for (i = 0; i < p->nsec; i++) {
		u8 *r = cmd + 8 + (size_t)(p->sec[i].id - 1) * 0x30;
		u64 iova = ane->pg_sec[i].iova, size = p->sec[i].size;
		u32 one = 1, id = p->sec[i].id;

		memcpy(r, &one, 4);
		memcpy(r + 4, &id, 4);
		memcpy(r + 0x18, &iova, 8);
		memcpy(r + 0x20, &size, 8);
	}
	w = ~0u;
	memcpy(cmd + 0x1b8, &w, 4);
	ane_h18_prog_dump(ane, "LOAD_PROGRAM sent", cmd, 0x1c0);
	ret = ane_h18_cmd_body(ane, io, ANE_H18_CSNE_LOAD_PROGRAM, 0x1c0, false, 0, cmd + 8, 0x1c0 - 8, &blk);
	ane_h18_prog_dump(ane, "LOAD_PROGRAM reply", (u8 *)ane->hs_cmd.va, 0x40);
	if (ret)
		goto fail;
	memcpy(&prog, (u8 *)ane->hs_cmd.va + 0x1b8, 4);
	dev_info(ane->dev, "ane_h18 prog: LOAD_PROGRAM program id %#x\n", prog);
	if (prog == ~0u) {
		dev_err(ane->dev, "ane_h18 prog: the firmware did not write a program id\n");
		ret = -EPROTO;
		goto fail;
	}
	loaded = true;
	ane_h18_prog_settle(ane, 20, t2h_cursor);

	/* CREATE_PROCESS */
	memset(cmd, 0, sizeof(cmd));
	memcpy(cmd + 8, &prog, 4);
	w = ~0u;
	memcpy(cmd + 0xc, &w, 4);
	ret = ane_h18_cmd_body(ane, io, ANE_H18_CSNE_CREATE_PROCESS, 0x10, false, 0, cmd + 8, 8, &blk);
	ane_h18_prog_dump(ane, "CREATE_PROCESS reply", (u8 *)ane->hs_cmd.va, 0x20);
	if (ret)
		goto fail;
	memcpy(&proc, (u8 *)ane->hs_cmd.va + 0xc, 4);
	dev_info(ane->dev, "ane_h18 prog: CREATE_PROCESS process id %#x\n", proc);
	if (proc == ~0u) {
		ret = -EPROTO;
		goto fail;
	}
	created = true;
	ane_h18_prog_settle(ane, 20, t2h_cursor);

	/* PROCEDURE_CALL: one 0x30 byte element per buffer at +0x60 (flags 1, buffer id +4, type +8, iova +0x18, size +0x20) */
	memset(cmd, 0, sizeof(cmd));
	{
		u32 prio = ANE_H18_PROG_PRIORITY, cnt = p->nio;
		u64 cookie = ANE_H18_PROG_COOKIE;

		memcpy(cmd + 8, &prog, 4);
		memcpy(cmd + 0xc, &proc, 4);
		memcpy(cmd + 0x18, &prio, 4);
		memcpy(cmd + 0x20, &cookie, 8);
		memcpy(cmd + 0x28, &cnt, 4);
	}
	for (i = 0; i < p->nio; i++) {
		u8 *r = cmd + 0x60 + (size_t)i * 0x30;
		u64 iova = ane->pg_io[i].iova, size = p->io[i].size;
		u32 one = 1, id = p->io[i].id, type = p->io[i].dir == 1 ? 1 : 0;

		memcpy(r, &one, 4);
		memcpy(r + 4, &id, 4);
		memcpy(r + 8, &type, 4);
		memcpy(r + 0x18, &iova, 8);
		memcpy(r + 0x20, &size, 8);
	}
	wmb();
	ane_h18_prog_dump(ane, "PROCEDURE_CALL sent", cmd, 0x60 + p->nio * 0x30);
	t0 = ktime_to_ms(ktime_get());
	ane->call_t2h = prog_out_words ? t2h_cursor : NULL;
	ane->call_t0 = ane->call_t1 = 0;
	ret = ane_h18_cmd_body(ane, io, ANE_H18_CSNE_PROCEDURE_CALL, 0x60 + p->nio * 0x30, false, 0, cmd + 8, 0x60 + p->nio * 0x30 - 8, &blk);
	ane->call_t2h = NULL;	/* the ack wait peeked with the cursor armed; the tight loop below re-arms it, because the finish event usually arrives after the ack */
	ane_h18_prog_dump(ane, "PROCEDURE_CALL reply", (u8 *)ane->hs_cmd.va, 0x40);
	if (ret)
		goto fail;
	if (prog_out_words) {
		u64 digest = 0, t0n = ktime_get_ns();
		u32 last_pend = 0;

		/* Tight poll so the stamp carries no sleep: the cursor is re-armed above because call_peek needs it, peek stamps call_t1, and the events are drained only
		 * after the stamp, so the console cannot delay it. */
		ane->call_t2h = t2h_cursor;
		for (waited = 0; waited < hs_wait_ms && !ane->call_t1; waited = (unsigned int)((ktime_get_ns() - t0n) / NSEC_PER_MSEC)) {
			ane_h18_chan_poll(ane, 1, &digest, &last_pend);
			ane_h18_call_peek(ane);
			cpu_relax();
			cond_resched();
		}
		ane->call_t2h = NULL;
		got += ane_h18_prog_t2h(ane, t2h_cursor, &fin);
	} else {
	for (waited = 0; waited < hs_wait_ms && !fin; waited++) {
		u64 digest = 0;
		u32 last_pend = 0;

		usleep_range(1000, 1100);
		ane_h18_chan_poll(ane, waited + 1, &digest, &last_pend);
		got += ane_h18_prog_t2h(ane, t2h_cursor, &fin);
		if (waited == 99 || waited == 299 || waited == 599) {
			/* Debug check: every record of the ring (64-byte records, the table's slot count),
			 * bit0 set = host owned, 1 = the firmware's freed marker, an address with bit0
			 * clear = a published event. Compare with the host cursor to catch a desync. */
			struct ane_h18_ring_copy *r2 = NULL;
			unsigned int s;

			for (i = 0; i < ANE_H18_CH_COUNT; i++)
				if (ane->ch_rings[i].base && ane->ch_rings[i].type == 1 && !strcmp(ane->ch_names[i], "IO_T2H"))
					r2 = &ane->ch_rings[i];
			if (r2) {
				dev_info(ane->dev, "ane_h18 prog: ring probe t=%ums cursor=%u slots=%u\n", waited + 1, *t2h_cursor, r2->slots);
				for (s = 0; s < r2->slots; s++) {
					u64 h = READ_ONCE(r2->base[s * 8]);

					if (h)
						dev_info(ane->dev, "ane_h18 prog: ring probe t=%ums rec %u hdr=%#llx len=%#llx\n",
							 waited + 1, s, h, READ_ONCE(r2->base[s * 8 + 1]));
				}
			}
		}
	}
	}
	if (prog_out_words) {
		if (ane->call_t1)
			dev_info(ane->dev, "ane_h18 prog: call %u/%u finished in %llu ns\n", pass, calls, ane->call_t1 - ane->call_t0);
		else if (fin)
			dev_info(ane->dev, "ane_h18 prog: call %u/%u finished, the finish event was not seen before the events were read: no latency\n", pass, calls);
		else
			dev_info(ane->dev, "ane_h18 prog: call %u/%u gave no finish event\n", pass, calls);
	}
	dev_info(ane->dev, "ane_h18 prog: PROCEDURE_CALL %s after %u ms, %u IO_T2H events\n", fin ? "finished" : "gave no finish event", waited, got);
	dev_info(ane->dev, "ane_h18 prog: call %u/%u %s in %llu ms (send to finish event)\n", pass, calls,
		 fin ? "finished" : "did not finish", ktime_to_ms(ktime_get()) - t0);
	ane_h18_dart_exc(ane, "after the call");
	for (i = 0; i < p->nio; i++) {
		const u32 *o = ane->pg_io[i].va;
		unsigned int k, nz = 0, words = p->io[i].size / 4;

		for (k = 0; k < words; k++)
			nz += o[k] != 0;
		dev_info(ane->dev, "ane_h18 prog: buffer %u (%s, %#llx bytes): %u nonzero words, first %08x %08x %08x %08x\n", p->io[i].id, p->io[i].dir == 1 ? "output" : "input", p->io[i].size, nz,
			 words > 0 ? o[0] : 0, words > 1 ? o[1] : 0, words > 2 ? o[2] : 0, words > 3 ? o[3] : 0);
		if (pass == 1 && hs_prog_dump) {
			unsigned int n = min(hs_prog_dump, 0x1000u);

			ane_h18_prog_dump(ane, p->io[i].dir == 1 ? "output buffer" : "input buffer", (const u8 *)o, n);
		}
		if (prog_out_words && p->io[i].dir == 1) {
			unsigned int w = min(words, prog_out_words), k2;

			for (k2 = 0; k2 < w; k2 += 16) {
				char line[16 * 9 + 1];
				unsigned int m, n2 = 0;

				for (m = 0; m < 16 && k2 + m < w; m++)
					n2 += sprintf(line + n2, "%08x ", o[k2 + m]);
				line[n2 - 1] = 0;
				dev_info(ane->dev, "ane_h18 prog: out buf %u +0x%x: %s\n", p->io[i].id, k2 * 4, line);
			}
		}
	}
	if (!fin) {
		ret = -ETIMEDOUT;
		goto fail;
	}
	ret = 0;

	/* unload: process first, then the program, each followed by hs_settle_ms ms of answering */
	memset(cmd, 0, sizeof(cmd));
	memcpy(cmd + 8, &prog, 4);
	memcpy(cmd + 0xc, &proc, 4);
	i = ane_h18_cmd_body(ane, io, ANE_H18_CSNE_TERMINATE_PROCESS, 0x10, false, 0, cmd + 8, 8, &blk);
	ane_h18_prog_dump(ane, "TERMINATE_PROCESS reply", (u8 *)ane->hs_cmd.va, 0x20);
	created = false;
	if (i) {
		ret = i;
		goto fail;		/* the unload did not answer: the fail path destroys the program or power-cycles the domain */
	}
	ane_h18_prog_settle(ane, hs_settle_ms, t2h_cursor);
	goto destroy;
fail:
	/* A command or the call timed out. The program and the process may still be resident, so the unload is tried anyway: a firmware that only lost the finish
	 * event answers it and the ANE holds nothing. The recovery runs only on -ETIMEDOUT or -EBUSY (an unanswered reply wait, or the IO slot of an earlier
	 * unanswered command that is not ours to send on). A command the firmware answers is taken as done without parsing the reply, so a firmware that answers
	 * DESTROY_PROGRAM with a refusal leaves the program resident with prog_cleaned set; only an unanswered one ends in the power-cycle in
	 * ane_h18_failpath_recover, which resets the firmware and sets prog_cleaned once the domain is back and the core started again. */
	if (ret == -ETIMEDOUT || ret == -EBUSY) {	/* -EBUSY: the IO slot of an earlier unanswered command is not ours */
		int unanswered = 0, cmdret;

		if (created) {
			memset(cmd, 0, sizeof(cmd));
			memcpy(cmd + 8, &prog, 4);
			memcpy(cmd + 0xc, &proc, 4);
			cmdret = ane_h18_cmd_body(ane, io, ANE_H18_CSNE_TERMINATE_PROCESS, 0x10, false, 0, cmd + 8, 8, &blk);
			ane_h18_prog_dump(ane, "TERMINATE_PROCESS reply", (u8 *)ane->hs_cmd.va, 0x20);
			ane_h18_prog_settle(ane, hs_settle_ms, t2h_cursor);
			if (cmdret) {
				unanswered = 1;
				dev_err(ane->dev, "ane_h18 prog: TERMINATE_PROCESS was not answered (%d)\n", cmdret);
			}
			created = false;
		}
		if (loaded) {
			memset(cmd, 0, sizeof(cmd));
			memcpy(cmd + 8, &prog, 4);
			cmdret = ane_h18_cmd_body(ane, io, ANE_H18_CSNE_DESTROY_PROGRAM, 0x0c, false, 0, cmd + 8, 4, &blk);
			ane_h18_prog_dump(ane, "DESTROY_PROGRAM reply", (u8 *)ane->hs_cmd.va, 0x20);
			ane_h18_prog_settle(ane, hs_settle_ms, t2h_cursor);
			if (cmdret) {
				unanswered = 1;
				dev_err(ane->dev, "ane_h18 prog: DESTROY_PROGRAM was not answered (%d)\n", cmdret);
			}
			loaded = false;
		}
		if (unanswered)
			ane_h18_failpath_recover(ane);
		else
			ane->prog_cleaned = true;
		goto fail_nounload;
	}
	if (created) {
		memset(cmd, 0, sizeof(cmd));
		memcpy(cmd + 8, &prog, 4);
		memcpy(cmd + 0xc, &proc, 4);
		(void)ane_h18_cmd_body(ane, io, ANE_H18_CSNE_TERMINATE_PROCESS, 0x10, false, 0, cmd + 8, 8, &blk);
		ane_h18_prog_dump(ane, "TERMINATE_PROCESS reply", (u8 *)ane->hs_cmd.va, 0x20);
		ane_h18_prog_settle(ane, hs_settle_ms, t2h_cursor);
	}
destroy:
	if (loaded) {
		int r2;

		memset(cmd, 0, sizeof(cmd));
		memcpy(cmd + 8, &prog, 4);
		r2 = ane_h18_cmd_body(ane, io, ANE_H18_CSNE_DESTROY_PROGRAM, 0x0c, false, 0, cmd + 8, 4, &blk);
		ane_h18_prog_dump(ane, "DESTROY_PROGRAM reply", (u8 *)ane->hs_cmd.va, 0x20);
		ane_h18_prog_settle(ane, hs_settle_ms, t2h_cursor);
		if (r2) {
			if (!ret)
				ret = r2;
			loaded = false;
			dev_err(ane->dev, "ane_h18 prog: DESTROY_PROGRAM was not answered (%d)\n", r2);
			ane_h18_failpath_recover(ane);	/* the program may still be resident: power-cycle the domain (this sets prog_cleaned only when the recovery finished) */
			goto fail_nounload;
		}
		loaded = false;
	}
fail_nounload:
	dev_info(ane->dev, "ane_h18 prog: sequence result %d\n", ret);
	return ret == -EPROTO || ret == -EREMOTEIO || ret == -ETIMEDOUT || ret == -EBUSY ? -EIO : ret;
}

/* macOS's step after it created its channels: the host owns every H2T slot, writes READY to SCRATCH3 and waits until the firmware clears it. With hs_cmds
 * then PRINT_ENABLE and CONFIG_GET. Nothing is written before the whole table checked out. Returns 0 only when every requested step worked. */
static int ane_h18_chan_step(struct ane_h18 *ane, const struct ane_h18_hs_buf *tb, u64 table)
{
	const u8 *tp = (u8 *)tb->va + (table - tb->iova);
	struct ane_h18_ring io = { 0 }, rings[ANE_H18_CH_COUNT];
	unsigned int i, k, waited;
	bool have_io = false;
	char name[ANE_H18_CH_NAME_LEN], names[ANE_H18_CH_COUNT][ANE_H18_CH_NAME_LEN];
	u32 v = 0, s3 = ~0u;
	u8 *blk;
	int ret;

	if (tb->iova + tb->size - table < (u64)ANE_H18_CH_COUNT * ANE_H18_CH_ENTRY) {
		dev_err(ane->dev, "ane_h18 chan: the table does not fit in its surface\n");
		return -EPROTO;
	}
	for (i = 0; i < ANE_H18_CH_COUNT; i++) {
		ret = ane_h18_chan_get(ane, tb, tp, i, &rings[i], name);
		memcpy(names[i], name, sizeof(name));
		if (ret) {
			dev_err(ane->dev, "ane_h18 chan: entry %u is not a ring of the table surface, nothing written\n", i);
			return ret;
		}
		if (!strcmp(name, "IO") && rings[i].type == 0) {
			io = rings[i];
			have_io = true;
		}
	}
	/* the rings must not overlap the table entries or each other: the ownership loop below writes into every H2T ring */
	for (i = 0; i < ANE_H18_CH_COUNT; i++) {
		u64 lo = rings[i].iova, hi = lo + (u64)rings[i].slots * ANE_H18_CH_SLOT;

		if (lo < table + (u64)ANE_H18_CH_COUNT * ANE_H18_CH_ENTRY && hi > table) {
			dev_err(ane->dev, "ane_h18 chan: ring %u overlaps the table, nothing written\n", i);
			return -EPROTO;
		}
		for (k = 0; k < i; k++)
			if (lo < rings[k].iova + (u64)rings[k].slots * ANE_H18_CH_SLOT && hi > rings[k].iova) {
				dev_err(ane->dev, "ane_h18 chan: ring %u overlaps ring %u, nothing written\n", i, k);
				return -EPROTO;
			}
	}
	if (hs_cmds && (!have_io || !ane->doorbell)) {
		dev_err(ane->dev, "ane_h18 chan: no IO ring of type 0 in the table, nothing written\n");
		return -EPROTO;
	}
	for (i = 0; i < ANE_H18_CH_COUNT; i++) {
		if (rings[i].type)
			continue;
		for (k = 0; k < rings[i].slots; k++) {
			u8 *e = (u8 *)rings[i].base + k * ANE_H18_CH_SLOT;

			memset(e, 0, 24);
			e[0] = 1;
		}
	}
	for (i = 0; i < ANE_H18_CH_COUNT; i++) {
		memcpy(ane->ch_names[i], names[i], sizeof(names[i]));
		ane->ch_rings[i] = (struct ane_h18_ring_copy){ rings[i].base, rings[i].iova, rings[i].slots, rings[i].bit, rings[i].type };
		if (!strcmp(names[i], "SHAREDMALLOC") && rings[i].type == 1) {
			ane->mal_ring = rings[i].base;
			ane->mal_slots = rings[i].slots;
			ane->mal_bit = rings[i].bit;
			ane->mal_cursor = 0;
		}
	}
	if (!ane->mal_ring)
		dev_warn(ane->dev, "ane_h18 chan: no SHAREDMALLOC ring of type 1 in the table, memory requests will not be answered\n");
	wmb();
	ane_h18_scratch_write(ane, 3, ANE_H18_HS_READY);
	for (waited = 0; waited < hs_wait_ms; waited++) {
		usleep_range(1000, 1100);
		s3 = readl(ane->scratch + ANE_H18_SCRATCH_OFF + 12);
		if (!s3)
			break;
	}
	dev_info(ane->dev, "ane_h18 chan: SCRATCH3 reads %#x after %ums\n", s3, waited);
	if (s3)
		return -EIO;
	{
		u64 digest = 0;
		u32 last_pend = 0;

		ane->snap_left = ANE_H18_SNAP_PER_WAIT;
		/* let the firmware run on for 100 ms, answering what it asks for, before the first look at the rings */
		for (k = 0; k < 100; k++) {
			usleep_range(1000, 1100);
			ane_h18_chan_poll(ane, k + 1, &digest, &last_pend);
		}
	}
	ane_h18_chan_snapshot(ane, "after SCRATCH3", true);
	if (!hs_cmds)
		return 0;
	/* macOS takes every command block from the pool surface, the one that holds the init block and that the firmware was told about at block +0x58 and +0x60. */
	ane->hs_cmd.va = (u8 *)ane->hs_pool.va + ANE_H18_CMD_POOL_OFF;
	ane->hs_cmd.iova = ane->hs_pool.iova + ANE_H18_CMD_POOL_OFF;
	ane->hs_cmd.size = hs_premap ? ANE_H18_PREMAP_BLOCK : hs_prog ? ANE_H18_PROG_BLOCK : ANE_H18_CMD_BLOCK;
	ane->hs_cmd_in_pool = true;
	dev_info(ane->dev, "ane_h18 hs surface CMD inside POOL at +%#x iova=%#llx size=%#zx\n", ANE_H18_CMD_POOL_OFF, (u64)ane->hs_cmd.iova, ane->hs_cmd.size);
	{
		/* macOS's order: PRINT_ENABLE, START, CONFIG_GET, RESOURCE_INFO_GET (the last two commands only with hs_start) */
		/* prop: a CH_PROPERTY_WRITE (0x1f, 0x14 bytes: u32 0 at +8, property id at +0xc, value at +0x10), sent only with hs_props. macOS sends the first one before
		 * RESOURCE_INFO_GET and the other four after it. */
		static const struct { u16 op; u32 len; bool arg; const char *name; bool start_only; bool prop; u32 id, val; bool pmu, dyn, mcsz, defset; } seq[] = {
			{ ANE_H18_CSNE_PRINT_ENABLE, 12, true, "PRINT_ENABLE", false },
			{ ANE_H18_CSNE_START, 12, false, "START", true },
			{ ANE_H18_CSNE_SET_SNE_PMU_BASE2, 16, false, "SET_SNE_PMU_BASE2", true, false, 0, 0, true },
			{ ANE_H18_CSNE_SET_DYNAMIC_POWERGATE, 12, false, "SET_DYNAMIC_POWERGATE", true, false, 0, 0, true, true },
			{ ANE_H18_CSNE_CONFIG_GET, 16, false, "CONFIG_GET", false },
			{ ANE_H18_CSNE_MCACHE_SIZE_GET, 12, false, "MCACHE_SIZE_GET", true, false, 0, 0, false, false, true },
			{ ANE_H18_CSNE_CH_PROPERTY_WRITE, 0x14, false, "PROPERTY 0x10a4", true, true, 0x10a4, 1 },
			{ ANE_H18_CSNE_RESOURCE_INFO_GET, 100, false, "RESOURCE_INFO_GET", true },
			{ ANE_H18_CSNE_CH_PROPERTY_WRITE, 0x14, false, "PROPERTY 0x10a6", true, true, 0x10a6, 1 },
			{ ANE_H18_CSNE_CH_PROPERTY_WRITE, 0x14, false, "PROPERTY 0xa1", true, true, 0xa1, 0 },
			{ ANE_H18_CSNE_CH_PROPERTY_WRITE, 0x14, false, "PROPERTY 0x10aa", true, true, 0x10aa, 1 },
			{ ANE_H18_CSNE_CH_PROPERTY_WRITE, 0x14, false, "PROPERTY 0x1209", true, true, 0x1209, 1 },
			{ ANE_H18_CSNE_DEFAULT_SETTING_SET, 0x18, false, "DEFAULT_SETTING_SET", true, false, 0, 0, false, false, false, true },
		};

		bool pmu_ok = false;

		for (i = 0; i < ARRAY_SIZE(seq); i++) {
			char when[48];

			if (seq[i].start_only && !hs_start)
				continue;
			if (seq[i].prop && !hs_props)
				continue;
			if (seq[i].mcsz && !hs_mcsz)
				continue;
			if (seq[i].defset && (!hs_defset || !hs_props))
				continue;
			if (seq[i].pmu && !seq[i].dyn && !hs_pmu)
				continue;
			if (seq[i].dyn && (!hs_pmu || !hs_dyn))
				continue;
			if (seq[i].dyn && hs_dyn > 2) {
				dev_err(ane->dev, "ane_h18 hs_dyn must be 0, 1 or 2 (got %u)\n", hs_dyn);
				return -EINVAL;
			}
			if (seq[i].dyn && !pmu_ok) {
				dev_warn(ane->dev, "ane_h18 chan: no %s: the firmware did not take the PMU base\n", seq[i].name);
				continue;
			}
			if (seq[i].defset) {
				u32 body[4] = { 1, 0, 2, 0x2710 };	/* u64 1 (one setting), u32 register id 2, u32 value 0x2710 */

				ret = ane_h18_cmd_body(ane, &io, seq[i].op, seq[i].len, false, 0, (const u8 *)body, sizeof(body), &blk);
				if (ret == -EREMOTEIO) {
					dev_warn(ane->dev, "ane_h18 chan: the firmware refused %s (ack at +6 nonzero), going on\n", seq[i].name);
					continue;
				}
			} else if (seq[i].dyn) {
				u32 on = hs_dyn == 2;	/* the byte at +8 is the low byte of this word */

				ret = ane_h18_cmd_body(ane, &io, seq[i].op, seq[i].len, false, 0, (const u8 *)&on, sizeof(on), &blk);
				if (ret == -EREMOTEIO) {
					dev_warn(ane->dev, "ane_h18 chan: the firmware refused %s (ack at +6 nonzero), going on\n", seq[i].name);
					continue;
				}
			} else if (seq[i].pmu) {
				u64 pmu_base = ANE_H18_PMU_BASE_H18G;

				ret = ane_h18_cmd_body(ane, &io, seq[i].op, seq[i].len, false, 0, (const u8 *)&pmu_base, sizeof(pmu_base), &blk);
				if (ret == -EREMOTEIO) {
					dev_warn(ane->dev, "ane_h18 chan: the firmware refused %s (ack at +6 nonzero), going on\n", seq[i].name);
					continue;
				}
				pmu_ok = !ret;
			} else if (seq[i].prop) {
				u32 body[2] = { seq[i].id, seq[i].id == 0xa1 ? hs_props == 2 : seq[i].val };

				ret = ane_h18_cmd_body(ane, &io, seq[i].op, seq[i].len, false, 0, (const u8 *)body, sizeof(body), &blk);
				if (ret == -EREMOTEIO) {
					dev_warn(ane->dev, "ane_h18 chan: the firmware refused %s (ack at +6 nonzero), going on\n", seq[i].name);
					continue;
				}
			} else {
				ret = ane_h18_cmd(ane, &io, seq[i].op, seq[i].len, seq[i].arg, 0, &blk);
				if (ret == -EREMOTEIO && seq[i].mcsz) {	/* a diagnostic read: its refusal is the result, the run goes on */
					dev_warn(ane->dev, "ane_h18 chan: the firmware refused %s (ack at +6 nonzero), going on\n", seq[i].name);
					continue;
				}
			}
			if (ret) {
				snprintf(when, sizeof(when), "after the failed %s", seq[i].name);
				ane_h18_chan_snapshot(ane, when, true);
				ane_h18_term_log(ane, when);
				return ret == -EPROTO || ret == -EREMOTEIO || ret == -ETIMEDOUT || ret == -EBUSY ? -EIO : ret;
			}
			if (seq[i].op == ANE_H18_CSNE_MCACHE_SIZE_GET) {
				memcpy(&v, blk + 8, 4);
				dev_info(ane->dev, "ane_h18 chan: MCACHE_SIZE_GET reply u32 at +8 = %#x\n", v);
			}
			if (seq[i].op == ANE_H18_CSNE_CONFIG_GET) {
				memcpy(&v, blk + 8, 4);
				dev_info(ane->dev, "ane_h18 chan: CONFIG_GET reply u32 at +8 = %#x (macOS reads it as the tick rate)\n", v);
			}
			if (seq[i].op == ANE_H18_CSNE_RESOURCE_INFO_GET) {
				for (k = 0; k < seq[i].len; k += 16) {
					u32 q[4] = { 0 };

					memcpy(q, blk + k, seq[i].len - k < 16 ? seq[i].len - k : 16);
					dev_info(ane->dev, "ane_h18 chan: RESOURCE_INFO_GET reply +%#x: %08x %08x %08x %08x\n", k, q[0], q[1], q[2], q[3]);
				}
			}
		}
	}
	if (hs_prog) {
		unsigned int pass, calls = min(hs_prog_calls, 1000u);
		unsigned int t2h = 0;

		for (pass = 1; pass <= calls; pass++) {
			ret = ane_h18_prog_run(ane, &io, pass, calls, &t2h);
			if (ret) {
				ane_h18_chan_snapshot(ane, "after the failed program", true);
				ane_h18_term_log(ane, "after the failed program");
				return ret;
			}
			ane_h18_prog_surfaces_free(ane);
			ane_h18_prog_settle(ane, hs_settle_ms, &t2h);	/* let the firmware finish with the program before the next pass loads one */
		}
	}
	{
		u64 digest = 0;
		u32 last_pend = 0;

		ane->snap_left = ANE_H18_SNAP_PER_WAIT;
		/* a firmware FREE can land after the acknowledgement: keep answering for 20 ms */
		for (k = 0; k < 20; k++) {
			usleep_range(1000, 1100);
			ane_h18_chan_poll(ane, k + 1, &digest, &last_pend);
		}
	}
	return 0;
}


/* The shipped-system handshake after READY, on the T8152: surfaces IPC (max(page, SCRATCH1 + 1) bytes) and HEAP (SCRATCH3 bytes), the 0x174 byte
 * init block taken from a pool surface, then SCRATCH0 (low), SCRATCH1 (high), the wake word in SCRATCH7. Values macOS does not give (the config size,
 * the template word) are zero or the T6021 value, logged with the block. Returns 0 only when the firmware published a table address.
 * Needs the core running with AP power ON acknowledged. */
static int ane_h18_handshake(struct ane_h18 *ane)
{
	const struct ane_h18_soc *s = ane->soc;
	u32 chan_off = ANE_H18_PO_ALIAS_OFF + s->mbox_num * 2 * ANE_H18_PO_STRIDE;
	u32 w[ANE_H18_SCRATCH_N], cur[3], last[3] = { 0xffffffffu, 0xffffffffu, 0xffffffffu };
	unsigned int i, waited, sent = 0;
	const struct ane_h18_hs_buf *tb;
	u64 ipc_size, heap_size, pub, table = 0;
	bool answered = false;
	char nm[24], tag[16];
	u8 *blk;
	int ret;

	for (i = 0; i < ANE_H18_SCRATCH_N; i++) {
		snprintf(nm, sizeof(nm), "HS_SCRATCH%u", i);
		ret = ane_h18_read(ane, nm, ane->scratch, s->scratch_pa, ANE_H18_SCRATCH_OFF + 4 * i, s->scratch_size, &w[i]);
		if (ret)
			return ret;
	}
	if (w[7] != ANE_H18_HS_READY) {
		dev_err(ane->dev, "ane_h18 handshake: SCRATCH7=%#x, not the READY value %#x yet, the requests in SCRATCH1 and SCRATCH3 are not valid\n", w[7], ANE_H18_HS_READY);
		return -EAGAIN;
	}
	if (w[0] >= ANE_H18_HS_REQ_LIMIT) {
		dev_err(ane->dev, "ane_h18 handshake: SCRATCH0=%#x is not below %#x, not going on\n", w[0], ANE_H18_HS_REQ_LIMIT);
		return -EOPNOTSUPP;
	}
	ipc_size = ALIGN((u64)w[1] + 1 > PAGE_SIZE ? (u64)w[1] + 1 : PAGE_SIZE, PAGE_SIZE);
	heap_size = w[3] ? ALIGN(w[3] > PAGE_SIZE ? (u64)w[3] : PAGE_SIZE, PAGE_SIZE) : 0;
	if (ipc_size > ANE_H18_HS_IPC_MAX || heap_size > ANE_H18_HS_HEAP_MAX) {
		dev_err(ane->dev, "ane_h18 handshake: firmware asks for IPC %#llx and HEAP %#llx bytes, above the limits %#x and %#x\n", ipc_size, heap_size,
			ANE_H18_HS_IPC_MAX, ANE_H18_HS_HEAP_MAX);
		return -E2BIG;
	}
	ret = dma_set_mask_and_coherent(ane->dev, DMA_BIT_MASK(42));
	if (ret) {
		dev_err(ane->dev, "ane_h18 handshake: no 42 bit DMA mask: %d\n", ret);
		return ret;
	}
	ret = ane_h18_hs_alloc(ane, &ane->hs_pool, ANE_H18_HS_POOL_SIZE, "POOL");
	if (!ret)
		ret = ane_h18_hs_alloc(ane, &ane->hs_ipc, ipc_size, "IPC");
	if (!ret && heap_size)
		ret = ane_h18_hs_alloc(ane, &ane->hs_heap, heap_size, "HEAP");
	if (ret) {
		ane_h18_hs_free(ane);
		return ret;
	}
	blk = ane->hs_pool.va;
	/* The block layout of ane_t6021_boot.h, which macOS shares on the T8152. Everything not listed stays zero. */
	ane_h18_hs_put(ane, blk, 0x00, 8, s->fw_seg[0].iova);		/* firmware surface */
	ane_h18_hs_put(ane, blk, 0x08, 8, ane->hs_ipc.iova);
	ane_h18_hs_put(ane, blk, 0x10, 8, hs_cfg_size);
	ane_h18_hs_put(ane, blk, 0x18, 8, 0x10000000ULL - hs_cfg_size);
	ane_h18_hs_put(ane, blk, 0x20, 8, heap_size ? ane->hs_heap.iova : 0);
	ane_h18_hs_put(ane, blk, 0x28, 8, w[3]);
	ane_h18_hs_put(ane, blk, 0x30, 4, 0);				/* previous firmware length, first boot */
	ane_h18_hs_put(ane, blk, 0x50, 8, (u64)w[1] + 1);		/* ordinal */
	ane_h18_hs_put(ane, blk, 0x58, 8, ane->hs_pool.iova);
	ane_h18_hs_put(ane, blk, 0x60, 8, ANE_H18_HS_POOL_SIZE);	/* first qword of the pool object, as T6021 */
	ane_h18_hs_put(ane, blk, 0x68, 4, 0x40);
	ane_h18_hs_put(ane, blk, 0x6c + 0xc0, 4, 4);			/* template word +0xc0, the template is otherwise zero */
	pub = ane->hs_pool.iova;
	dev_info(ane->dev, "ane_h18 handshake: init block at iova %#llx (IPC %#llx bytes, HEAP %#llx bytes)\n", pub, ipc_size, heap_size);
	wmb();
	ane_h18_scratch_write(ane, 0, (u32)pub);
	ane_h18_scratch_write(ane, 1, (u32)(pub >> 32));
	ane_h18_scratch_write(ane, 7, ANE_H18_HS_WAKE);
	snprintf(tag, sizeof(tag), "alias%u", s->mbox_num * 2);
	for (waited = 0; waited < hs_wait_ms; waited++) {
		usleep_range(1000, 1100);
		for (i = 0; i < 3; i++)
			cur[i] = readl(ane->scratch + ANE_H18_SCRATCH_OFF + (i == 2 ? 28 : 4 * i));
		if (cur[0] != last[0] || cur[1] != last[1] || cur[2] != last[2]) {
			dev_info(ane->dev, "ane_h18 handshake t+%ums SCRATCH0=%#x SCRATCH1=%#x SCRATCH7=%#x\n", waited + 1, cur[0], cur[1], cur[2]);
			memcpy(last, cur, sizeof(last));
		}
		ane_h18_rtkit_step(ane, chan_off, tag, &sent);	/* log whatever the firmware sends now */
		table = ((u64)cur[1] << 32) | cur[0];
		if (table != pub && cur[2] == ANE_H18_HS_READY) {
			answered = true;
			break;
		}
	}
	for (i = 0; i < ANE_H18_SCRATCH_N; i++)
		w[i] = readl(ane->scratch + ANE_H18_SCRATCH_OFF + 4 * i);
	dev_info(ane->dev, "ane_h18 handshake after %ums: SCRATCH0..7 = %#x %#x %#x %#x %#x %#x %#x %#x\n", waited, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
	if (!answered) {
		dev_err(ane->dev, "ane_h18 handshake: no table address within %ums\n", hs_wait_ms);
		return -ETIMEDOUT;
	}
	tb = ane_h18_hs_find(ane, table);
	if (!tb) {
		dev_info(ane->dev, "ane_h18 handshake: the table address %#llx is outside the surfaces allocated here\n", table);
	} else {
		u64 left = tb->iova + tb->size - table, len = left < 0x800 ? left : 0x800, off;
		unsigned int skipped = 0;

		dev_info(ane->dev, "ane_h18 handshake: table address %#llx is in a surface of %#zx bytes at %#llx, first %#llx bytes follow\n", table, tb->size, (u64)tb->iova, len);
		for (off = 0; off + 16 <= len; off += 16) {
			u32 q[4];

			memcpy(q, (u8 *)tb->va + (table - tb->iova) + off, sizeof(q));
			if (!(q[0] | q[1] | q[2] | q[3])) {
				skipped++;
				continue;
			}
			dev_info(ane->dev, "ane_h18 hs table +%#llx: %08x %08x %08x %08x\n", off, q[0], q[1], q[2], q[3]);
		}
		dev_info(ane->dev, "ane_h18 hs table: %u all-zero lines of 16 bytes skipped\n", skipped);
	}
	if (hs_chan) {
		if (!tb) {
			dev_err(ane->dev, "ane_h18 chan: the table is not in a surface allocated here\n");
			return -EIO;
		}
		return ane_h18_chan_step(ane, tb, table);
	}
	return 0;
}

/* Store one RTKit management message (endpoint 0) in TX as one paired store, if a TX slot is free. */
static int ane_h18_rtkit_tx_now(struct ane_h18 *ane, u32 chan_off, const char *tag, u64 msg0)
{
	const struct ane_h18_soc *s = ane->soc;
	u32 st, r[4] = { (u32)msg0, (u32)(msg0 >> 32), 0, 0 };

	st = readl(ane->mbox + chan_off + ANE_H18_PO_STATUS);
	if (!(st & 0xf00))
		return -EBUSY;
	dev_crit(ane->dev, "ane_h18 write TX of %s pa=%#llx: msg0=%#llx msg1=0 (one paired 16 byte store)\n", tag, s->mbox_pa + chan_off + ANE_H18_PO_TX, msg0);
	ane_h18_po_store16(ane->mbox + chan_off + ANE_H18_PO_TX, r);
	return 0;
}

/* Store the queued messages in order while TX slots are free. Called at every poll, so a message that found no free slot goes out when one returns. */
static void ane_h18_rtkit_flush(struct ane_h18 *ane, u32 chan_off, const char *tag)
{
	unsigned int i;

	while (ane->rtk_txq_n && !ane_h18_rtkit_tx_now(ane, chan_off, tag, ane->rtk_txq[0])) {
		for (i = 1; i < ane->rtk_txq_n; i++)
			ane->rtk_txq[i - 1] = ane->rtk_txq[i];
		ane->rtk_txq_n--;
	}
}

/* Accept one management message: stored now when a TX slot is free and nothing is queued ahead of it, queued otherwise. The per-run cap counts accepted
 * messages. A message is refused (-EBUSY) only past the cap or when the queue is full. */
static int ane_h18_rtkit_send(struct ane_h18 *ane, u32 chan_off, const char *tag, u64 msg0, unsigned int *sent)
{
	if (*sent >= ANE_H18_PO_MAXSEND) {
		dev_warn(ane->dev, "ane_h18 rtkit: send cap reached, not sending %#llx\n", msg0);
		return -EBUSY;
	}
	if (!ane->rtk_txq_n && !ane_h18_rtkit_tx_now(ane, chan_off, tag, msg0)) {
		(*sent)++;
		return 0;
	}
	if (ane->rtk_txq_n == ARRAY_SIZE(ane->rtk_txq)) {
		dev_warn(ane->dev, "ane_h18 rtkit: TX queue full, not sending %#llx\n", msg0);
		return -EBUSY;
	}
	dev_info(ane->dev, "ane_h18 rtkit: no free TX slot, queued %#llx behind %u\n", msg0, ane->rtk_txq_n);
	ane->rtk_txq[ane->rtk_txq_n++] = msg0;
	(*sent)++;
	return 0;
}

/* System endpoints the Linux apple-rtkit driver starts after the endpoint map (crash log, syslog, debug, ioreport, oslog, tracekit). */
static bool ane_h18_rtk_system_ep(unsigned int ep)
{
	return ep == 1 || ep == 2 || ep == 3 || ep == 4 || ep == 8 || ep == 0xa;
}

/* One RTKit management exchange on a Postoffice channel at window offset chan_off. Reads the status; with a message waiting it reads RX (raw words
 * logged) and answers HELLO and EPMAP in TX. With rtkit_start it also starts the system endpoints after the last endpoint map message, and answers the
 * firmware's IOP power state acknowledgement with AP power state ON. Returns 1 if a message was read, and counts messages sent in *sent. It never
 * sends anything for an unknown message type or a message for another endpoint, and never answers a buffer request. */
static int ane_h18_rtkit_step(struct ane_h18 *ane, u32 chan_off, const char *tag, unsigned int *sent)
{
	const struct ane_h18_soc *s = ane->soc;
	u32 st, w[4] = { 0 };
	u64 msg0, msg1, reply = 0;
	unsigned int type;

	if ((u64)chan_off + ANE_H18_PO_STATUS + 4 > s->mbox_size)
		return 0;
	ane_h18_rtkit_flush(ane, chan_off, tag);
	st = readl(ane->mbox + chan_off + ANE_H18_PO_STATUS);
	if (!(st & 0xf))
		return 0;
	dev_crit(ane->dev, "ane_h18 read RX of %s pa=%#llx (status %#x)\n", tag, s->mbox_pa + chan_off + ANE_H18_PO_RX, st);
	ane_h18_po_load16(ane->mbox + chan_off + ANE_H18_PO_RX, w);
	dev_info(ane->dev, "ane_h18 msg %s rx=%#x,%#x,%#x,%#x\n", tag, w[0], w[1], w[2], w[3]);
	msg0 = ((u64)w[1] << 32) | w[0];
	msg1 = ((u64)w[3] << 32) | w[2];
	type = (msg0 >> ANE_H18_RTK_TYPE_SHIFT) & 0xff;
	dev_info(ane->dev, "ane_h18 rtkit rx type=%u ep=%#llx msg0=%#llx\n", type, msg1 & 0xff, msg0);
	if ((msg1 & 0xff) || !rtkit_hello)
		return 1;
	if (type == ANE_H18_RTK_HELLO) {
		unsigned int minv = msg0 & 0xffff, maxv = (msg0 >> 16) & 0xffff;

		if (minv > ANE_H18_RTK_VERSION || maxv < ANE_H18_RTK_VERSION) {
			dev_warn(ane->dev, "ane_h18 rtkit: firmware versions %u..%u do not include %u, no reply\n", minv, maxv, ANE_H18_RTK_VERSION);
			return 1;
		}
		reply = ((u64)ANE_H18_RTK_HELLO_REPLY << ANE_H18_RTK_TYPE_SHIFT) | ((u64)ANE_H18_RTK_VERSION << 16) | ANE_H18_RTK_VERSION;
		ane_h18_rtkit_send(ane, chan_off, tag, reply, sent);
	} else if (type == ANE_H18_RTK_EPMAP) {
		unsigned int base = (msg0 >> 32) & 7, i;
		bool last = msg0 & (1ULL << 51);

		for (i = 0; i < 32; i++)
			if (msg0 & (1ULL << i)) {
				unsigned int ep = base * 32 + i;

				dev_info(ane->dev, "ane_h18 rtkit endpoint %#x discovered\n", ep);
				if (ep < ARRAY_SIZE(ane->rtk_eps) * 32)
					ane->rtk_eps[ep / 32] |= 1u << (ep % 32);
			}
		/* base in bits 34:32, last flag bit 51 (answered with the same flag), otherwise MORE = bit 0 */
		reply = ((u64)ANE_H18_RTK_EPMAP << ANE_H18_RTK_TYPE_SHIFT) | (msg0 & (7ULL << 32)) | (last ? (1ULL << 51) : 1ULL);
		if (ane_h18_rtkit_send(ane, chan_off, tag, reply, sent))
			return 1;
		if (last && rtkit_start) {
			for (i = 0; i < ARRAY_SIZE(ane->rtk_eps) * 32; i++) {
				if (!(ane->rtk_eps[i / 32] & (1u << (i % 32))) || !ane_h18_rtk_system_ep(i))
					continue;
				/* STARTEP: endpoint in bits 39:32, flag bit 1 */
				if (ane_h18_rtkit_send(ane, chan_off, tag, ((u64)ANE_H18_RTK_STARTEP << ANE_H18_RTK_TYPE_SHIFT) | ((u64)i << 32) | (1ULL << 1), sent))
					break;
			}
		}
	} else if (type == ANE_H18_RTK_IOP_PWR_ACK) {
		dev_info(ane->dev, "ane_h18 rtkit: IOP power state acknowledged: %#llx\n", msg0 & 0xffff);
		if (ane->rtk_shutdown && (msg0 & 0xffff) == ane->rtk_iop_state)
			ane->rtk_iop_acked = true;
		else if (rtkit_start && !ane->rtk_ap_sent && (msg0 & 0xff) == ANE_H18_RTK_PWR_ON &&
			 !ane_h18_rtkit_send(ane, chan_off, tag, ((u64)ANE_H18_RTK_AP_PWR << ANE_H18_RTK_TYPE_SHIFT) | ANE_H18_RTK_PWR_ON, sent))
			ane->rtk_ap_sent = true;
	} else if (type == ANE_H18_RTK_AP_PWR) {
		dev_info(ane->dev, "ane_h18 rtkit: AP power state acknowledged: %#llx\n", msg0 & 0xffff);
		if (ane->rtk_shutdown && (msg0 & 0xffff) == ane->rtk_ap_state) {
			ane->rtk_ap_acked = true;
			/* the IOP power state follows the AP one, as apple_rtkit_shutdown and apple_rtkit_poweroff do */
			ane_h18_rtkit_send(ane, chan_off, tag, ((u64)ANE_H18_RTK_SET_IOP_PWR << ANE_H18_RTK_TYPE_SHIFT) | ane->rtk_iop_state, sent);
		} else if (rtkit_start && !ane->rtk_shutdown && (msg0 & 0xff) == ANE_H18_RTK_PWR_ON && !ane->rtk_app_started) {
			unsigned int i;

			ane->rtk_ap_on = true;
			ane_h18_read_scratch_words(ane, "apon");

			/* the coprocessor is running now: start the application endpoints it announced */
			ane->rtk_app_started = true;
			for (i = ANE_H18_RTK_APP_EP; i < ARRAY_SIZE(ane->rtk_eps) * 32; i++) {
				if (!(ane->rtk_eps[i / 32] & (1u << (i % 32))))
					continue;
				if (ane_h18_rtkit_send(ane, chan_off, tag, ((u64)ANE_H18_RTK_STARTEP << ANE_H18_RTK_TYPE_SHIFT) | ((u64)i << 32) | (1ULL << 1), sent))
					break;
			}
		}
	} else if (type == ANE_H18_RTK_IOP_IDLE) {
		dev_info(ane->dev, "ane_h18 rtkit: the firmware reports IOP idle\n");
		if (ane->rtk_shutdown)
			ane->rtk_iop_idle = true;
	}
	return 1;
}

static int ane_h18_stage_boot(struct ane_h18 *ane)
{
	const struct ane_h18_soc *s = ane->soc;
	u32 st = 0, ctrl = 0, rv_lo = 0, rv_hi = 0, last = 0xffffffffu, po;
	unsigned int i, c, waited = 0, msgs = 0, running_seen = 0, sent = 0;
	bool stopped = false;
	int ret = 0, hs_ret = 0;

	if (!confirm_power || !confirm_boot) {
		ane_h18_result(ane, "boot", "REFUSED", "confirm_power=1 and confirm_boot=1 required");
		return -EPERM;
	}
	if (owned_fw) {
		if (!map_fw) {
			ane_h18_result(ane, "boot", "REFUSED", "owned_fw=1 needs map_fw=1 (the owned pages are DART-mapped at the fixed TEXT/DATA IOVAs)");
			return -EINVAL;
		}
		if (!fw_path || !fw_path[0] || strchr(fw_path, '/') || strstr(fw_path, "..")) {
			ane_h18_result(ane, "boot", "REFUSED", "owned_fw=1 needs fw_path, a firmware-loader file name without a path, holding the exact pinned payload");
			return -EINVAL;
		}
	}
	if (psd_state >= ANE_H18_PSD_P6_RAIL || psd_slots < 1 || psd_slots > ANE_H18_NPSD || boot_wait_ms < 10 || boot_wait_ms > 2000) {
		ane_h18_result(ane, "boot", "REFUSED", "psd_state 0 to 5, psd_slots 1 or 2, boot_wait_ms 10 to 2000");
		return -EINVAL;
	}
	if (map_fw && !ane->kpsd) {
		ane_h18_result(ane, "boot", "REFUSED", "map_fw=1 needs power-domains in the node (the kernel's PSD driver powers the slots for the DART)");
		return -EINVAL;
	}
	if (rtkit_hello && !alias_mbox) {
		ane_h18_result(ane, "boot", "REFUSED", "rtkit_hello=1 needs alias_mbox=1");
		return -EINVAL;
	}
	if (rtkit_stop > 2 || (rtkit_stop && !rtkit_start)) {
		ane_h18_result(ane, "boot", "REFUSED", "rtkit_stop=1 or 2 needs rtkit_start=1");
		return -EINVAL;
	}
	if (rtkit_start && !rtkit_hello) {
		ane_h18_result(ane, "boot", "REFUSED", "rtkit_start=1 needs rtkit_hello=1");
		return -EINVAL;
	}
	if ((hs_chan && !handshake) || (hs_cmds && !hs_chan) || (hs_start && !hs_cmds) || (hs_prog && !hs_start)) {
		ane_h18_result(ane, "boot", "REFUSED", "hs_chan=1 needs handshake=1, hs_cmds=1 needs hs_chan=1, hs_start=1 needs hs_cmds=1 and hs_prog=1 needs hs_start=1");
		return -EINVAL;
	}
	if (owned_fw && hs_prog) {
		ane_h18_result(ane, "boot", "REFUSED", "owned_fw=1 is a firmware-residency trial that submits no program; drop hs_prog=1 or owned_fw=1");
		return -EINVAL;
	}
	if (hs_props > 2 || (hs_props && !hs_start)) {
		ane_h18_result(ane, "boot", "REFUSED", "hs_props is 0, 1 or 2 and needs hs_start=1");
		return -EINVAL;
	}
	if (((prog_out_words || hs_prog_calls != 1) && !hs_prog) || !hs_prog_calls || hs_prog_calls > 1000 || hs_settle_ms > 1000) {
		ane_h18_result(ane, "boot", "REFUSED", "prog_out_words needs hs_prog=1; hs_prog_calls is 1 to 1000 and needs hs_prog=1 above 1; hs_settle_ms is 0 to 1000");
		return -EINVAL;
	}
	if (hs_prog) {
		const char *why = "";

		if (!prog_fw || !prog_fw[0] || strchr(prog_fw, '/') || strstr(prog_fw, "..")) {
			ane_h18_result(ane, "boot", "REFUSED", "hs_prog=1 needs prog_fw, a file name without a path");
			return -EINVAL;
		}
		ret = request_firmware(&ane->prog_blob, prog_fw, ane->dev);
		if (ret) {
			dev_err(ane->dev, "ane_h18 prog: request_firmware(%s) failed: %d\n", prog_fw, ret);
			ane->prog_blob = NULL;
			ane_h18_result(ane, "boot", "REFUSED", "hs_prog=1: the program file could not be loaded");
			return -ENOENT;
		}
		ret = ane_h18_prog_parse(ane->prog_blob->data, ane->prog_blob->size, &ane->pd, &why);
		if (ret) {
			dev_err(ane->dev, "ane_h18 prog: %s: %s\n", prog_fw, why);
			release_firmware(ane->prog_blob);
			ane->prog_blob = NULL;
			ane_h18_result(ane, "boot", "REFUSED", "hs_prog=1: the program file is not valid");
			return -EINVAL;
		}
		dev_info(ane->dev, "ane_h18 prog: %s is valid: %u sections, %u buffers\n", prog_fw, ane->pd.nsec, ane->pd.nio);
	}
	if ((hs_premap && !hs_prog) || (dart_inst && (!hs_prog || !read_dart)) || (dart_inst & ~(BIT(ANE_H18_DART_INSTANCES) - 1))) {
		ane_h18_result(ane, "boot", "REFUSED", "hs_premap=1 needs hs_prog=1; dart_inst needs hs_prog=1 and read_dart=1 and is a mask of instances 0 to 3");
		return -EINVAL;
	}
	if (handshake && (!map_fw || !rtkit_start || !rtkit_stop || !read_scratch || hs_wait_ms < 10 || hs_wait_ms > 5000 || !hs_cfg_size || hs_cfg_size > 0x10000000)) {
		ane_h18_result(ane, "boot", "REFUSED", "handshake=1 needs map_fw=1, rtkit_start=1, rtkit_stop=1 or 2, read_scratch=1, hs_wait_ms 10 to 5000, hs_cfg_size 1 to 0x10000000");
		return -EINVAL;
	}
	if (read_dart && !ane->kpsd && psd_slots != ANE_H18_NPSD) {
		ane_h18_result(ane, "boot", "REFUSED", "read_dart=1 needs psd_slots=2 (the DART block is read only with both ANE slots on)");
		return -EINVAL;
	}
	ret = ane_h18_psd_up(ane, "boot");
	if (ret)
		return ret;
	/* Preflight, all read only: the core must be stopped with RUN clear and IORVBAR exactly as iBoot leaves it. */
	ret = ane_h18_read(ane, "CPU_STATUS", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_STATUS, s->asc_size, &st);
	if (!ret)
		ret = ane_h18_read(ane, "CPU_CONTROL", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_CONTROL, s->asc_size, &ctrl);
	if (!ret)
		ret = ane_h18_read(ane, "IORVBAR_LO", ane->rvbar, s->rvbar_pa, ANE_H18_RVBAR_OFF, s->rvbar_size, &rv_lo);
	if (!ret)
		ret = ane_h18_read(ane, "IORVBAR_HI", ane->rvbar, s->rvbar_pa, ANE_H18_RVBAR_OFF + 4, s->rvbar_size, &rv_hi);
	if (ret) {
		ane_h18_psd_off(ane);
		ane_h18_result(ane, "boot", "FAIL", "read-refused");
		return ret;
	}
	if ((st & ANE_H18_ST_RUNNING) || !(st & ANE_H18_ST_STOPPED) || (ctrl & ANE_H18_CPU_RUN) || (((u64)rv_hi << 32) | rv_lo) != s->rvbar_expect) {
		dev_err(ane->dev, "ane_h18 preflight: cpu_status=%#x cpu_control=%#x iorvbar=%#llx (want stopped, RUN clear, iorvbar %#llx)\n", st, ctrl,
			((u64)rv_hi << 32) | rv_lo, s->rvbar_expect);
		ane_h18_psd_off(ane);
		ane_h18_result(ane, "boot", "FAIL", "preflight-not-as-iboot-left-it");
		return -EBUSY;
	}
	if (owned_fw) {
		/* stage the pinned payload and plan the pages through the default domain while it still owns them; before any attach, before RUN */
		ret = ane_h18_plan_owned_map(ane);
		if (ret) {
			ane_h18_free_owned_image(ane);	/* nothing is mapped and no domain is held: the trial fails clean */
			ane_h18_psd_off(ane);
			ane_h18_result(ane, "boot", "FAIL", "owned-prepare-failed");
			return ret;
		}
	}
	if (alias_mbox) {
		u32 chan_off = ANE_H18_PO_ALIAS_OFF + s->mbox_num * 2 * ANE_H18_PO_STRIDE, ast = readl(ane->mbox + chan_off + ANE_H18_PO_STATUS);

		dev_info(ane->dev, "ane_h18 alias status before RUN pa=%#llx value=%#x\n", s->mbox_pa + chan_off + ANE_H18_PO_STATUS, ast);
	}
	if (read_dart) {
		ret = ane_h18_read_dart_words(ane, "DART0");
		if (ret) {
			if (owned_fw)
				ane_h18_free_owned_image(ane);	/* planned but not mapped: nothing holds the staged image */
			ane_h18_psd_off(ane);
			ane_h18_result(ane, "boot", "FAIL", "dart-read-refused");
			return ret;
		}
	}
	if (map_fw) {
		ret = ane_h18_map_firmware(ane);
		if (ret) {
			ane_h18_psd_off(ane);
			if (!owned_fw)
				ane_h18_result(ane, "boot", "FAIL", "fw-map-failed");
			return ret;
		}
		if (read_dart) {
			ret = ane_h18_read_dart_words(ane, "DARTPOST");
			if (ret) {
				if (owned_fw)
					ane_h18_unmap_owned_firmware(ane);
				else
					ane_h18_unmap_firmware(ane, ARRAY_SIZE(s->fw_seg));
				ane_h18_psd_off(ane);
				ane_h18_result(ane, "boot", "FAIL", "dart-read-refused");
				return ret;
			}
		}
	}
	ane_h18_read_scratch_words(ane, "pre");
	/* Start the core. From here a reboot may be needed. */
	dev_crit(ane->dev, "ane_h18 write CPU_CONTROL=%#x pa=%#llx (RUN)\n", ctrl | ANE_H18_CPU_RUN, s->asc_pa + ANE_H18_ASC_CPU_CONTROL);
	ane->fw_started = true;
	writel(ctrl | ANE_H18_CPU_RUN, ane->asc + ANE_H18_ASC_CPU_CONTROL);
	for (waited = 0; waited < boot_wait_ms; waited += 10) {
		usleep_range(10000, 11000);
		ret = ane_h18_read(ane, "CPU_STATUS", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_STATUS, s->asc_size, &st);
		if (ret)
			break;
		if (st & ANE_H18_ST_RUNNING)
			running_seen = 1;
		if (st != last) {
			dev_info(ane->dev, "ane_h18 t+%ums cpu_status=%#x\n", waited + 10, st);
			last = st;
		}
		for (c = 0; c < ANE_H18_PO_NCAND + (alias_mbox ? 1 : 0) && msgs < ANE_H18_PO_MAXMSG; c++) {
			u32 chan_off = c < ANE_H18_PO_NCAND ? ane_h18_po_chan(s, c) * ANE_H18_PO_STRIDE :
					ANE_H18_PO_ALIAS_OFF + s->mbox_num * 2 * ANE_H18_PO_STRIDE;
			char tag[16];

			if ((u64)chan_off + ANE_H18_PO_STATUS + 4 > s->mbox_size)
				continue;
			if (c < ANE_H18_PO_NCAND)
				snprintf(tag, sizeof(tag), "ch%u", ane_h18_po_chan(s, c));
			else
				snprintf(tag, sizeof(tag), "alias%u", s->mbox_num * 2);
			if (c >= ANE_H18_PO_NCAND) {
				msgs += ane_h18_rtkit_step(ane, chan_off, tag, &sent);
				continue;
			}
			po = readl(ane->mbox + chan_off + ANE_H18_PO_STATUS);
			msgs += ane_h18_po_pop(ane, chan_off, tag, po);
		}
	}
	if (!ane_h18_domain_gone(ane))
		ane_h18_read_scratch_words(ane, "end");
	if (handshake) {
		if (ane->rtk_ap_on) {
			hs_ret = ane_h18_handshake(ane);
		} else {
			dev_err(ane->dev, "ane_h18 handshake: AP power ON was not acknowledged, no handshake\n");
			hs_ret = -ENODEV;
		}
		dev_info(ane->dev, "ane_h18 handshake result %d\n", hs_ret);
	}
	if (ane->domain_down) {
		ane_h18_pin_module(ane);
		dev_emerg(ane->dev, "ane_h18 boot: the domain is still down after the unload recovery, no ANE register is touched; the surfaces stay allocated, reboot required\n");
		ane_h18_result(ane, "boot", "FAIL", "domain-down-after-recover");
		return -EIO;
	}
	if (rtkit_stop && ane->rtk_ap_on) {
		/* RTKit shutdown as apple_rtkit_shutdown does it: AP power state QUIESCED, then IOP power state SLEEP once that is acknowledged, then wait for the
		 * IOP acknowledgement (or an idle message). rtkit_stop=2 sends OFF on both sides instead (apple_rtkit_poweroff), which cannot be restarted. */
		u32 chan_off = ANE_H18_PO_ALIAS_OFF + s->mbox_num * 2 * ANE_H18_PO_STRIDE;
		char tag[16];

		snprintf(tag, sizeof(tag), "alias%u", s->mbox_num * 2);
		for (waited = 0; waited < ANE_H18_RTK_STOP_WAIT_MS && ane->rtk_txq_n; waited += 10) {
			/* messages of the boot still queued go out first */
			usleep_range(10000, 11000);
			ane_h18_rtkit_step(ane, chan_off, tag, &sent);
		}
		sent = 0;	/* the shutdown has its own send budget: the boot messages cannot starve it */
		ane->rtk_shutdown = true;
		ane->rtk_ap_state = rtkit_stop == 2 ? ANE_H18_RTK_PWR_OFF : ANE_H18_RTK_PWR_QUIESCED;
		ane->rtk_iop_state = rtkit_stop == 2 ? ANE_H18_RTK_PWR_OFF : ANE_H18_RTK_PWR_SLEEP;
		if (!ane_h18_rtkit_send(ane, chan_off, tag, ((u64)ANE_H18_RTK_AP_PWR << ANE_H18_RTK_TYPE_SHIFT) | ane->rtk_ap_state, &sent)) {
			for (waited = 0; waited < ANE_H18_RTK_STOP_WAIT_MS && !ane->rtk_iop_acked && !ane->rtk_iop_idle; waited += 10) {
				usleep_range(10000, 11000);
				ane_h18_rtkit_step(ane, chan_off, tag, &sent);
			}
		}
		dev_info(ane->dev, "ane_h18 rtkit shutdown: ap_acked=%d iop_acked=%d iop_idle=%d after %ums\n", ane->rtk_ap_acked, ane->rtk_iop_acked,
			 ane->rtk_iop_idle, waited);
	}
	/* Stop the core again. */
	ctrl = readl(ane->asc + ANE_H18_ASC_CPU_CONTROL);
	dev_crit(ane->dev, "ane_h18 write CPU_CONTROL=%#x pa=%#llx (clear RUN)\n", ctrl & ~ANE_H18_CPU_RUN, s->asc_pa + ANE_H18_ASC_CPU_CONTROL);
	writel(ctrl & ~ANE_H18_CPU_RUN, ane->asc + ANE_H18_ASC_CPU_CONTROL);
	for (i = 0; i < 20; i++) {
		usleep_range(5000, 6000);
		st = readl(ane->asc + ANE_H18_ASC_CPU_STATUS);
		if (st & ANE_H18_ST_STOPPED) {
			stopped = true;
			break;
		}
	}
	dev_info(ane->dev, "ane_h18 boot running_seen=%u messages=%u final cpu_status=%#x stopped=%d\n", running_seen, msgs, st, stopped);
	if (!stopped) {
		ane_h18_pin_module(ane);	/* a running core may still DMA into the surfaces and the image the module owns */
		dev_emerg(ane->dev, "ane_h18 the core did not stop after RUN was cleared: the PSD slots stay on, reboot required\n");
		ane_h18_result(ane, "boot", "FAIL", "core-not-stopped");
		return ret ? ret : -EBUSY;
	}
	ane->fw_started = false;
	if (!ane->map_retained) {
		ane_h18_hs_free(ane);	/* only after the core stopped: a running core may still write into these surfaces */
		if (owned_fw)
			ane_h18_unmap_owned_firmware(ane);	/* after hs_free: the fixed-IOVA aliases unmap through this domain */
		else
			ane_h18_unmap_firmware(ane, ARRAY_SIZE(s->fw_seg));
	}
	if (!keep_on)
		ane_h18_psd_off(ane);
	if (ane->map_retained)
		return -EIO;	/* the EMERG above names the retention; the module pin stays and the control write reports failure */
	if (ret) {
		ane_h18_result(ane, "boot", "FAIL", "read-refused");
		if (owned_fw) {
			ane_h18_pin_module(ane);
			if (!keep_on)
				ane_h18_psd_off(ane);
		}
		return ret;
	}
	if (rtkit_stop && !(ane->rtk_ap_on && ane->rtk_ap_acked && ane->rtk_iop_acked)) {
		ane_h18_result(ane, "boot", "FAIL", "rtkit-shutdown-incomplete");
		if (owned_fw) {
			ane_h18_pin_module(ane);
			if (!keep_on)
				ane_h18_psd_off(ane);
		}
		return -EIO;
	}
	if (handshake && hs_ret) {
		ane_h18_result(ane, "boot", "FAIL", hs_ret == -ETIMEDOUT ? "handshake-no-table-address" : (hs_ret == -EIO || hs_ret == -EPROTO) ? "handshake-channel-step-failed" : "handshake-not-done");
		if (owned_fw && !ane->prog_cleaned) {
			ane_h18_pin_module(ane);
			if (!keep_on)
				ane_h18_psd_off(ane);
			return hs_ret;
		}
		if (owned_fw)
			dev_info(ane->dev, "ane_h18 boot: the program stage left no program behind, the surfaces and the image are freed and the module can be removed\n");
		return hs_ret;
	}
	ane_h18_result(ane, "boot", "PASS", handshake ? "core-handshook-and-stopped" : running_seen ? "core-ran-and-stopped" : "core-never-reported-running");
	return 0;
}
static int ane_h18_probe(struct platform_device *pdev)
{
	struct ane_h18 *ane;
	unsigned int stage_idx, k;
	int idx[3 + ANE_H18_NPSD];
	const struct ane_h18_soc *soc_sel = &ane_t8152_soc;
	int ret;

	if (optin && !strcmp(optin, ane_t8132_soc.name))
		soc_sel = &ane_t8132_soc;
	if (!optin || strcmp(optin, soc_sel->name)) {
		dev_err(&pdev->dev, "not probing: load with optin=%s or optin=%s. EXPERIMENTAL: no hardware has run this module.\n",
			ane_t8152_soc.name, ane_t8132_soc.name);
		return -EPERM;
	}
	ret = ane_h18_stage_idx(stage, &stage_idx);
	if (ret) {
		dev_err(&pdev->dev, "unknown stage \"%s\" (dt, psd, power, boot or reads)\n", stage);
		return ret;
	}
	if (!soc_sel->row_complete && stage_idx != ANE_H18_STAGE_DT && stage_idx != ANE_H18_STAGE_READS) {
		dev_err(&pdev->dev, "ane_h18 %s: row incomplete, refusing %s: only stage=dt and stage=reads are allowed (asc/rvbar/mbox/scratch/mailbox/rvbar_expect/fw_seg need hardware verification; see ane_h18_soc.c)\n",
			soc_sel->name, stage);
		return -EINVAL;
	}
	if (stage_idx == ANE_H18_STAGE_READS && !allow_reads) {
		dev_err(&pdev->dev, "ane_h18 %s: stage=reads needs allow_reads=1 (read-only register probe: IORVBAR, SCRATCH7 candidates, doorbell words; no writes)\n",
			soc_sel->name);
		return -EPERM;
	}
	if (stage_idx == ANE_H18_STAGE_READS && soc_sel != &ane_t8132_soc) {
		dev_err(&pdev->dev, "ane_h18 %s: stage=reads is the t8132 row's probe; this SoC does not support it\n",
			soc_sel->name);
		return -EINVAL;
	}
	if (stage_idx == ANE_H18_STAGE_READS &&
	    !of_property_present(pdev->dev.of_node, "power-domains")) {
		dev_err(&pdev->dev, "ane_h18 %s: stage=reads requires power-domains (the pmgr pwrstate domains of the ANE); a bare node leaves the block gated and the first readl hangs the bus\n",
			soc_sel->name);
		return -EPERM;
	}
	ane = devm_kzalloc(&pdev->dev, sizeof(*ane), GFP_KERNEL);
	if (!ane)
		return -ENOMEM;
	ane->dev = &pdev->dev;
	ane->soc = soc_sel;
	ane->kpsd = of_property_present(pdev->dev.of_node, "power-domains");
	ane->pd_count = of_count_phandle_with_args(pdev->dev.of_node, "power-domains",
						   "#power-domain-cells");
	/* negatives stay negative: kpsd true with a malformed count refuses the reads
	 * stage below instead of arming with no domain attached */
	if (ane->kpsd) {
		dev_info(&pdev->dev, "ane_h18 the node has power-domains: the kernel powers PSD slots 30 and 31, this module requests no PSD state\n");
		ret = devm_pm_runtime_enable(&pdev->dev);
		if (ret)
			return ret;
	}
	platform_set_drvdata(pdev, ane);

	if (soc_sel->row_complete) {
		ret = ane_h18_check_windows(ane, idx);
		if (ret) {
			ane_h18_result(ane, stage, "FAIL", "dt-window-mismatch");
			return ret;
		}
	} else {
		ane_h18_report_windows(pdev, ane);
	}
	if (stage_idx == ANE_H18_STAGE_DT) {
		if (!soc_sel->row_complete) {
			/* the windows were reported above, not checked: the row has no verified
			 * window map yet, so the checked verdict would be a false PASS */
			ane_h18_result(ane, stage, "PASS", "dt-windows-reported-unchecked");
			return 0;
		}
		return ane_h18_stage_dt(ane, idx);
	}

	if (stage_idx == ANE_H18_STAGE_READS) {
		/* Read-only register probe on the engine window (reg index 0, the 32 MiB ADT
		 * window the kext uses as its RegisterControl base). The 2026-11-11 bus hang:
		 * the ANE block was power/clock-gated at read time, so a single readl hung the
		 * bus until the watchdog rebooted. Rules: the node must carry power-domains
		 * (every cell must resolve to a pmgr pwrstate provider in the live tree, so a
		 * PMP report entry or a shifted phandle refuses before any power request), the
		 * operator must pass the four ANE ps registers, and every step re-checks them
		 * (and the optional extra gate registers), logs the address at dev_crit
		 * priority BEFORE the readl, and reads exactly one register. No writes, no
		 * clocks, no PMP request from this stage. */
		struct device_node *pd_np = NULL;
		u32 ph = 0;
		int ret2;
		unsigned int pd_i;

		/* first, before any hardware or power request: verify what each power-domains
		 * phandle points at. The probe above already refused a bare node, so the node
		 * has power-domains here and pd_count is meaningful. */
		if (ane->pd_count < 0) {
			ane_h18_result(ane, stage, "FAIL", "pd-count-malformed");
			return -EINVAL;
		}
		if (!ane->pd_count) {
			ane_h18_result(ane, stage, "FAIL", "power-domains-unreadable");
			return -EINVAL;
		}
		for (pd_i = 0; pd_i < (unsigned int)ane->pd_count; pd_i++) {
			if (of_property_read_u32_index(pdev->dev.of_node, "power-domains",
						       pd_i, &ph)) {
				ane_h18_result(ane, stage, "FAIL", "power-domains-unreadable");
				return -EINVAL;
			}
			pd_np = of_find_node_by_phandle(ph);
			if (!pd_np ||
			    !(of_device_is_compatible(pd_np, "apple,t6050-pmgr-pwrstate") ||
			      of_device_is_compatible(pd_np, "apple,t8103-pmgr-pwrstate"))) {
				dev_crit(&pdev->dev, "ane_h18 %s: power-domains cell %u (phandle 0x%x) does not resolve to a pmgr pwrstate provider - re-derive the ANE phandles from the installed dtb; a PMP report entry is refused here by design\n",
					 soc_sel->name, pd_i, ph);
				ane_h18_result(ane, stage, "FAIL", "ps-domain-mismatch");
				of_node_put(pd_np);
				return -EINVAL;
			}
			of_node_put(pd_np);
		}


		if (pmgr_gate_pa && !pmgr_gate_mask)
			goto reads_fail_gates;
		if (pmgr_gate2_pa && !pmgr_gate_pa)
			goto reads_fail_gates;
		if (pmgr_gate2_pa && !pmgr_gate2_mask)
			goto reads_fail_gates;	/* a zero mask passes trivially; refuse the config */
		if (!pmgr_ps_pa || (pmgr_ps_pa & (PAGE_SIZE - 1)))
			goto reads_fail_ps;
		if (pmgr_ps_mask != 0xf)	/* the actual-state nibble must read fully on: only 0xf is the on-identity */
			goto reads_fail_ps_mask;
		ret2 = -ENXIO;
		ane->eng = of_iomap(pdev->dev.of_node, 0);
		if (!ane->eng) {
			ane_h18_result(ane, stage, "FAIL", "engine-iomap-failed");
			return -ENXIO;
		}
		/* With more than one power-domains cell the driver core attaches no
		 * domain automatically (pmdomain/core.c), so a plain
		 * pm_runtime_get_sync on the device would cast NO vote while this
		 * log claimed one. Attach the claimed domains explicitly and hold
		 * them runtime-active through the device links (DEV_LINK_ON) for as
		 * long as this probe is bound. */
		if (ane->pd_count > 1) {
			static const struct dev_pm_domain_attach_data pd_data = {
				.pd_flags = PD_FLAG_DEV_LINK_ON,
			};

			ret = devm_pm_domain_attach_list(&pdev->dev, &pd_data, &ane->pd_list);
			if (ret < 0) {
				ret2 = ret;
				ane_h18_result(ane, stage, "FAIL", "pd-attach-failed");
				goto out_unmap_eng;
			}
			if (ret != ane->pd_count) {
				dev_crit(&pdev->dev, "ane_h18 %s: attached %d of %d claimed power domains - re-derive the claimed phandles from the installed dtb\n",
					 soc_sel->name, ret, ane->pd_count);
				ane_h18_result(ane, stage, "FAIL", "pd-attach-count");
				ret2 = -EINVAL;
				goto out_unmap_eng;
			}
			ane->vote_held = true;
		} else {
			ret2 = pm_runtime_get_sync(&pdev->dev);
			if (ret2 < 0) {
				pm_runtime_put_noidle(&pdev->dev);
				ane_h18_result(ane, stage, "FAIL", "runtime-vote-failed");
				goto out_unmap_eng;
			}
			ane->vote_held = true;
		}
		dev_crit(&pdev->dev, "ane_h18 reads: genpd claims held (%s; no PMP vote)\n",
			 ane->pd_count > 1 ? "claims attached, links runtime-active" : "single power-domains provider");

		if (pmgr_gate_pa) {
			ane->gate = ioremap(pmgr_gate_pa, 0x1000);
			if (!ane->gate)
				goto out_release_gate;
		}
		if (pmgr_gate2_pa) {
			ane->gate2 = ioremap(pmgr_gate2_pa, 0x1000);
			if (!ane->gate2)
				goto out_release_gates;
		}
		ane->ps = ioremap(pmgr_ps_pa, 0x1000);
		if (!ane->ps)
			goto out_release_gate2;	/* unmap the mapped gate2, then gate, then the vote */
		ane_h18_rd_idx = 0;
		ane_h18_rd_dev = ane;
		ane_h18_rd_control = debugfs_create_file("ane_h18_rd", 0200, NULL, NULL, &ane_h18_rd_fops);
		if (IS_ERR_OR_NULL(ane_h18_rd_control)) {
			ane_h18_rd_control = NULL;
			ane_h18_result(ane, stage, "FAIL", "rd-control-failed");
			ane_h18_unmap(ane);
			return -ENOMEM;
		}
		dev_crit(&pdev->dev, "ane_h18 reads: engine mapped, the four ANE ps regs at pmgr_ps_pa=%#llx%s checked per read; write \"next\" to /sys/kernel/debug/ane_h18_rd for ONE register per step\n",
			 (unsigned long long)pmgr_ps_pa,
			 pmgr_gate_pa ? " plus the extra gate register(s)" : "");
		ane_h18_result(ane, stage, "PASS", "reads-armed-step-via-ane_h18_rd");
		return 0;

reads_fail_gates:
		ane_h18_result(ane, stage, "FAIL", "gates-unverified-pass-pmgr-gate-pa-and-mask");
		return -EINVAL;
reads_fail_ps:
		ane_h18_result(ane, stage, "FAIL", "ps-unverified-pass-pmgr-ps-pa");
		return -EINVAL;
reads_fail_ps_mask:
		ane_h18_result(ane, stage, "FAIL", "ps-mask-not-0xf");
		return -EINVAL;
out_release_gate2:
		if (ane->gate2) { iounmap(ane->gate2); ane->gate2 = NULL; }
out_release_gates:
		if (ane->gate) { iounmap(ane->gate); ane->gate = NULL; }
out_release_gate:
		if (ane->pd_count <= 1)
			pm_runtime_put_sync(&pdev->dev);
		ane->vote_held = false;
		ane_h18_result(ane, stage, "FAIL", "gate-iomap-failed");
out_unmap_eng:
		iounmap(ane->eng);
		ane->eng = NULL;
		return ret2;
	}

	ret = ane_h18_map_psd(ane, idx);
	if (ret)
		goto out;
	if (stage_idx == ANE_H18_STAGE_PSD) {
		ret = ane_h18_stage_psd(ane);
		goto out;
	}
	/* stage=power: the ASC windows are mapped here, but nothing reads them before the PSD acknowledge. */
	ane->asc = of_iomap(pdev->dev.of_node, idx[0]);
	ane->rvbar = of_iomap(pdev->dev.of_node, idx[1]);
	if (read_mbox || stage_idx == ANE_H18_STAGE_BOOT)
		ane->mbox = of_iomap(pdev->dev.of_node, idx[2]);
	if (!ane->asc || !ane->rvbar || ((read_mbox || stage_idx == ANE_H18_STAGE_BOOT) && !ane->mbox)) {
		dev_err(&pdev->dev, "ane_h18 of_iomap of an ASC window failed\n");
		ret = -ENXIO;
		goto out;
	}
	if (read_scratch && stage_idx == ANE_H18_STAGE_BOOT) {
		/* Mapping only; the words are read after both PSD slots acknowledged. */
		ane->scratch = ioremap(ane->soc->scratch_pa, ane->soc->scratch_size);
		if (!ane->scratch) {
			dev_err(&pdev->dev, "ane_h18 ioremap of the SCRATCH page failed\n");
			ret = -ENXIO;
			goto out;
		}
	}
	if (hs_chan && stage_idx == ANE_H18_STAGE_BOOT) {
		/* Mapping only; the doorbell is written only for a command, after the channel step. */
		ane->doorbell = ioremap(ane->soc->engine_pa + ANE_H18_DOORBELL_OFF, 0x1000);
		ane->ipi_pending = ioremap(ane->soc->engine_pa + ANE_H18_IPI_PENDING_OFF, 0x1000);
		ane->ipi_ack = ioremap(ane->soc->engine_pa + ANE_H18_IPI_ACK_OFF, 0x1000);
		if (!ane->doorbell || !ane->ipi_pending || !ane->ipi_ack) {
			dev_err(&pdev->dev, "ane_h18 ioremap of the doorbell page failed\n");
			ret = -ENXIO;
			goto out;
		}
	}
	if (read_dart && stage_idx == ANE_H18_STAGE_BOOT) {
		/* Mapping only; nothing reads the DART before both PSD slots acknowledged. */
		ane->dart = ioremap(ane->soc->dart_pa, ane->soc->dart_size);
		if (!ane->dart) {
			dev_err(&pdev->dev, "ane_h18 ioremap of the DART window failed\n");
			ret = -ENXIO;
			goto out;
		}
		for (k = 1; k < ANE_H18_DART_INSTANCES; k++) {
			if (!(dart_inst & BIT(k)))
				continue;
			ane->dartx[k] = ioremap(ane->soc->dart_pa + (u64)k * ANE_H18_DART_INST_STRIDE, ane->soc->dart_size);
			if (!ane->dartx[k]) {
				dev_err(&pdev->dev, "ane_h18 ioremap of the DART instance %u window failed\n", k);
				ret = -ENXIO;
				goto out;
			}
		}
	}
	ret = stage_idx == ANE_H18_STAGE_BOOT ? ane_h18_stage_boot(ane) : ane_h18_stage_power(ane);
out:
	/* a failed probe never reaches remove(); a stage that passed keeps its mappings until remove.
	 * If the core may still be running, keep the mappings (the module cannot be removed after a failed bind anyway). */
	if (ret && !ane->fw_started)
		ane_h18_unmap(ane);
	return ret;
}

static void ane_h18_remove(struct platform_device *pdev)
{
	struct ane_h18 *ane = platform_get_drvdata(pdev);

	if (ane) {
		if (ane->fw_started) {
			dev_emerg(&pdev->dev, "ane_h18 the core may still run: reboot required\n");
			return;
		}
		if (keep_on)
			dev_warn(&pdev->dev, "ane_h18 keep_on=1: PSD slots left running\n");
		ane_h18_unmap(ane);
	}
}

static const struct of_device_id ane_h18_of_match[] = {
	{ .compatible = "apple,t8152-ane" },
	{ .compatible = "apple,t8132-ane" },
	{ }
};

static struct platform_driver ane_h18_driver = {
	.driver = {
		.name = "ane_h18",
		.suppress_bind_attrs = true,
		.of_match_table = ane_h18_of_match,
	},
	.probe = ane_h18_probe,
	.remove = ane_h18_remove,
};
module_driver(ane_h18_driver, ane_h18_dt_register, ane_h18_dt_unregister, optin, stage);

MODULE_AUTHOR("Joshua Warren");
MODULE_DESCRIPTION("OPT-IN EXPERIMENTAL Apple H18 ANE0 bring-up for the T8152 (dt, psd, power; no firmware path) and the T8132 diagnostic row (dt, reads)");
MODULE_LICENSE("Dual MIT/GPL");
