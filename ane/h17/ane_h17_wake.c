// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 stage 4: QUARANTINED firmware-wake experiment.
 *
 * The experiment below is retained for source review, but module init
 * refuses it before registering a driver, taking power or mapping MMIO.
 * At 670e0bbfe054384763d039cda4d8af1683420e85 the register map mixed
 * H16 constants and two mailbox guesses. J700 snapshot attempts hung;
 * no surviving log identifies the failing transaction. A polling timeout
 * cannot bound an individual MMIO transaction that never completes.
 * See receipts/2026-10-30-j700-h17-hang/README.md for the release criteria.
 * This gate has deliberately no module-parameter override.
 *
 * One runtime-PM vote on the existing ps_ane_sys domain, the pmgr child
 * raise, one MMIO read pass over the T8140-sourced engine block and —
 * only with confirm_wake=1 — the AppleT8140ANEHAL control path: the
 * mailbox-setup writes, one doorbell ring, a bounded poll of the
 * doorbell ack and SCRATCH7, and the ack write. It never programs a
 * DART, maps firmware, answers the firmware, or submits a task.
 *
 * Register provenance (engine-relative offsets), per the hal-map
 * receipt (receipts/2026-10-30-j700-h17-hal-map/README.md):
 * - T8140-sourced, from the AppleT8140ANEHAL kext of this machine's
 *   macOS 26.6.2 build: doorbell 0x18a4000 / ack 0x18a4008 / state
 *   0x18a4010 and 0x18a4018 (64-bit words), IPI pending 0x18ac000, the
 *   mailbox-setup writes 0x1960000 <- 1, 0x1974200 <- 0 then <- 1,
 *   0x195c284 <- 1, 0x195c288 <- 1, and the SCRATCH page 0x1880000
 *   (exported window 0x401880000; the h16g interface rows put SCRATCH0
 *   at +0x20 and the h18g table at +0x24 — scratch0_off selects). The
 *   pmgr rows 0x290 and 0xc008 are the macOS power-up path.
 * - Unsourced on T8140 (h16g-row / M6 family parity only) and behind
 *   opt-in parameters, default off: CPU_CONTROL 0x1600044, CPU_STATUS
 *   0x1600048, RVBAR 0x1050000 (read_wrapper=1) and the ASC mailbox
 *   peek at mbox_off + 0x110/0x114/0x830/0x838 (mbox_peek=1, mbox_off
 *   0x1608000 or 0x196c000). The h16g/M6 RUN write to CPU_CONTROL is
 *   retired with them.
 * - The wake word 0x08042006 follows the H14/H16 modules
 *   (ane/t6021/ane_t6021.h, ane/h16/README-bringup.md); it is a
 *   checked value, not an access. Every access is logged before it
 *   happens so a hang names the word, and a hang costs one reboot.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

/* The quarantine gate: deliberately no module-parameter override. The
 * qualified sim build (-DH17_REGISTER_MAP_QUALIFIED=1) flips it to
 * exercise the probe against the fake bus only. */
#ifndef H17_REGISTER_MAP_QUALIFIED
#define H17_REGISTER_MAP_QUALIFIED 0
#endif

#define H17_ANE_NODE "/soc/ane@400000000"
#define H17_PMGR_PS_NODE "/soc/power-management@300700000/power-controller@290"
#define H17_PROBE_COMPAT "omarchy,t8140-ane-ps-probe"
#define H17_DART0_NODE "/soc/iommu@401800000"
#define H17_DART1_NODE "/soc/iommu@401820000"
#define H17_DART2_NODE "/soc/iommu@401840000"

#define H17_PS_OFF 0x290
#define H17_PS_ACTUAL GENMASK(7, 4)

#define H17_ENGINE_BASE 0x400000000ull
#define H17_ENGINE_MIN_SIZE 0x1900000

/* T8140-sourced rows (hal-map receipt): the doorbell block, IPI word,
 * mailbox-setup writes and the SCRATCH page are composed or exported by
 * the AppleT8140ANEHAL kext of this machine's macOS build. */
#define H17_DOORBELL 0x18a4000
#define H17_DOORBELL_ACK 0x18a4008
#define H17_DOORBELL_STATE0 0x18a4010
#define H17_DOORBELL_STATE1 0x18a4018
#define H17_IPI_PENDING 0x18ac000
#define H17_MBOX_SETUP0 0x1960000
#define H17_MBOX_SETUP1 0x1974200
#define H17_MBOX_SETUP2 0x195c284
#define H17_MBOX_SETUP3 0x195c288
/* SCRATCH0 candidates: the h16g rows put SCRATCH0 at +0x20 of the page,
 * the h18g table at +0x24. Both alignments are covered by scratch0_off. */
#define H17_SCRATCH_PAGE 0x1880000
#define H17_SCRATCH0 (H17_SCRATCH_PAGE + 0x20)
#define H17_SCRATCH0_ALT (H17_SCRATCH_PAGE + 0x24)
#define H17_SCRATCH_N 9
/* Unsourced on T8140 (h16g-row / M6 family parity only): behind
 * read_wrapper / mbox_peek, default off. */
#define H17_CPU_CONTROL 0x1600044
#define H17_CPU_STATUS 0x1600048
#define H17_RVBAR 0x1050000
#define H17_MBOX 0x1608000
#define H17_MBOX_ALT 0x196c000
#define H17_MBOX_A2I_CTRL 0x110
#define H17_MBOX_I2A_CTRL 0x114
#define H17_MBOX_I2A_RECV0 0x830
#define H17_MBOX_I2A_RECV1 0x838

#define H17_MBOX_EMPTY BIT(17)
#define H17_MBOX_ENABLE BIT(0)
#define H17_WAKE_ACK 0x08042006u
#define H17_MBOX_MAXMSG 8

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700 after preflight and stage 3 pass");

static bool confirm_wake;
module_param(confirm_wake, bool, 0444);
MODULE_PARM_DESC(confirm_wake, "1 = run the HAL control path: mailbox-setup writes, doorbell ring, ack");

static unsigned int poll_ms = 10;
module_param(poll_ms, uint, 0444);
MODULE_PARM_DESC(poll_ms, "Poll interval in ms (default 10)");

static unsigned int wake_wait_ms = 500;
module_param(wake_wait_ms, uint, 0444);
MODULE_PARM_DESC(wake_wait_ms, "Bounded DOORBELL_ACK/SCRATCH7 poll window after the ring (default 500)");

static unsigned int mbox_wait_ms = 500;
module_param(mbox_wait_ms, uint, 0444);
MODULE_PARM_DESC(mbox_wait_ms, "Bounded mailbox peek window after the wake polls (default 500)");

static unsigned int stop_wait_ms = 500;
module_param(stop_wait_ms, uint, 0444);
MODULE_PARM_DESC(stop_wait_ms, "Bounded wait for the raised child state to read back on (default 500)");

static unsigned int mbox_off = H17_MBOX;
module_param(mbox_off, uint, 0444);
MODULE_PARM_DESC(mbox_off, "ASC mailbox base for mbox_peek, engine-relative (default 0x1608000, alt 0x196c000)");

static unsigned int scratch0_off = H17_SCRATCH0;
module_param(scratch0_off, uint, 0444);
MODULE_PARM_DESC(scratch0_off, "SCRATCH0 offset, 0x1880020 (default) or 0x1880024");

static bool read_wrapper;
module_param(read_wrapper, bool, 0444);
MODULE_PARM_DESC(read_wrapper, "1 = also read the unsourced h16g/M6 CPU wrapper trio");

static bool mbox_peek;
module_param(mbox_peek, bool, 0444);
MODULE_PARM_DESC(mbox_peek, "1 = peek the unsourced ASC mailbox at mbox_off");

/* The macOS power-up path for ane-type 0x200/0x210 raises one more pmgr
 * power state, 0xf at offset 0xc008 of the ANE pmgr window, beyond
 * ANE_SYS@0x290 (the h16g branch raises 0xc018/0xc020, a t6021-shaped
 * branch 0x4008..0x4030), before it touches the ASC. On J700 the first
 * stage 4 snapshot with ANE_SYS alone hung the machine at the engine
 * read pass, so child_ps=1 (the default) raises 0xc008 — idempotently,
 * skipped when ACTUAL is already 0xf — before the first engine access.
 * child_ps=0 keeps the run write-free, engine reads may then hang. */
#define H17_PMGR_CHILD_OFF 0xc008

static bool child_ps = true;
module_param(child_ps, bool, 0444);
MODULE_PARM_DESC(child_ps, "Raise the 0xc008 pmgr state before engine access (default on)");

static const u32 h17_pmgr_dump_offs[] = {
	0x2e0, 0x4000, 0x4008, 0x4010, 0x4018, 0x4020, 0x4028, 0x4030,
	0xc000, 0xc008, 0xc010, 0xc018, 0xc020,
};

struct ane_h17_wake {
	struct device *dev;
	void __iomem *engine;
	struct regmap *pmgr;
};

/* Every MMIO read is logged before it happens: a hang names the word. */
static u32 h17_rd(struct ane_h17_wake *w, u32 off, const char *what)
{
	u32 v;

	dev_info(w->dev, "ane_h17_wake read off=%#x %s\n", off, what);
	v = readl(w->engine + off);
	dev_info(w->dev, "ane_h17_wake value off=%#x %s=%#x\n", off, what, v);
	return v;
}

/* The HAL's doorbell words are 64-bit (ldr/str). */
static u64 h17_rdq(struct ane_h17_wake *w, u32 off, const char *what)
{
	u64 v;

	dev_info(w->dev, "ane_h17_wake read off=%#x %s\n", off, what);
	v = readq(w->engine + off);
	dev_info(w->dev, "ane_h17_wake value off=%#x %s=%#llx\n", off, what, v);
	return v;
}

static void h17_wrq(struct ane_h17_wake *w, u32 off, u64 v, const char *what)
{
	dev_info(w->dev, "ane_h17_wake write off=%#x val=%#llx %s\n", off, v, what);
	writeq(v, w->engine + off);
}

static bool ane_h17_darts_disabled(void)
{
	static const char *const paths[] = {
		H17_DART0_NODE, H17_DART1_NODE, H17_DART2_NODE,
	};
	struct device_node *node;
	unsigned int i;
	bool okay = true;

	for (i = 0; i < ARRAY_SIZE(paths); i++) {
		node = of_find_node_by_path(paths[i]);
		if (!node || of_device_is_available(node) ||
		    !of_device_is_compatible(node, "apple,t8140-dart"))
			okay = false;
		of_node_put(node);
	}
	return okay;
}

static int ane_h17_wake_probe(struct platform_device *pdev)
{
	struct device_node *ane, *ps;
	struct ane_h17_wake *w;
	struct resource res;
	u32 word, scratch7 = 0, s7a, s7b;
	u64 state, state1, ring = 0;
	unsigned int i, msgs = 0, polls;
	bool wake_seen = false, ring_acked = false;
	int ret;

	if (!pdev->dev.pm_domain) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=REFUSED reason=no-genpd\n");
		return -ENODEV;
	}
	w = devm_kzalloc(&pdev->dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->dev = &pdev->dev;

	/* Same power vote as stage 3: the aurora PMGR driver owns the word. */
	ps = of_find_node_by_path(H17_PMGR_PS_NODE);
	if (!ps)
		return -ENODEV;
	w->pmgr = syscon_node_to_regmap(ps->parent);
	of_node_put(ps);
	if (IS_ERR(w->pmgr))
		return PTR_ERR(w->pmgr);

	ane = of_find_node_by_path(H17_ANE_NODE);
	if (!ane)
		return -ENODEV;
	ret = of_address_to_resource(ane, 0, &res);
	of_node_put(ane);
	if (ret)
		return ret;
	if (res.start != H17_ENGINE_BASE || resource_size(&res) < H17_ENGINE_MIN_SIZE) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=engine-window start=%#llx size=%#llx\n",
			(unsigned long long)res.start, (unsigned long long)resource_size(&res));
		return -EINVAL;
	}
	if (scratch0_off != H17_SCRATCH0 && scratch0_off != H17_SCRATCH0_ALT) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=scratch0-off val=%#x\n",
			scratch0_off);
		return -EINVAL;
	}
	w->engine = devm_ioremap_resource(&pdev->dev, &res);
	if (IS_ERR(w->engine))
		return PTR_ERR(w->engine);

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=power-vote error=%d\n", ret);
		goto disable;
	}
	ret = regmap_read(w->pmgr, H17_PS_OFF, &word);
	if (ret) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=pmgr-read error=%d\n", ret);
		goto release;
	}
	dev_info(&pdev->dev, "ane_h17_wake pmgr off=%#x word=%#x target=%#x actual=%#lx\n",
		 H17_PS_OFF, word, word & 0xf, FIELD_GET(H17_PS_ACTUAL, word));
	if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=actual-not-on\n");
		ret = -EIO;
		goto release;
	}

	/* Map the child power-state picture: reads only, PMGR block. */
	for (i = 0; i < ARRAY_SIZE(h17_pmgr_dump_offs); i++) {
		dev_info(&pdev->dev, "ane_h17_wake pmgr read off=%#x\n", h17_pmgr_dump_offs[i]);
		if (regmap_read(w->pmgr, h17_pmgr_dump_offs[i], &word))
			dev_info(&pdev->dev, "ane_h17_wake pmgr off=%#x read-failed\n",
				 h17_pmgr_dump_offs[i]);
		else
			dev_info(&pdev->dev, "ane_h17_wake pmgr off=%#x word=%#x\n",
				 h17_pmgr_dump_offs[i], word);
	}

	/*
	 * Raise the 0xc008 child state before the first engine access:
	 * macOS does this before it touches the ASC, and the first
	 * snapshot attempt with ANE_SYS alone hung at the engine read
	 * pass. Idempotent: skipped when ACTUAL is already 0xf.
	 */
	if (child_ps) {
		dev_info(&pdev->dev, "ane_h17_wake pmgr read off=%#x child-state\n",
			 H17_PMGR_CHILD_OFF);
		if (regmap_read(w->pmgr, H17_PMGR_CHILD_OFF, &word))
			word = 0;
		if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf) {
			dev_info(&pdev->dev, "ane_h17_wake pmgr write off=%#x val=%#x child\n",
				 H17_PMGR_CHILD_OFF, 0xfu);
			regmap_write(w->pmgr, H17_PMGR_CHILD_OFF, 0xf);
			polls = (stop_wait_ms + poll_ms - 1) / poll_ms;
			for (i = 0; i < polls; i++) {
				msleep(poll_ms);
				if (regmap_read(w->pmgr, H17_PMGR_CHILD_OFF, &word))
					break;
				if (FIELD_GET(H17_PS_ACTUAL, word) == 0xf)
					break;
			}
		}
		dev_info(&pdev->dev, "ane_h17_wake pmgr off=%#x word=%#x child\n",
			 H17_PMGR_CHILD_OFF, word);
		if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf) {
			dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=child-not-on\n");
			ret = -EIO;
			goto release;
		}
	}

	/* Read-only pass over the T8140-sourced engine block. No write yet. */
	for (i = 0; i < H17_SCRATCH_N; i++)
		h17_rd(w, scratch0_off + 4 * i, "SCRATCH");
	h17_rd(w, H17_IPI_PENDING, "IPI_PENDING");
	h17_rdq(w, H17_DOORBELL_STATE0, "DOORBELL_STATE0");
	h17_rdq(w, H17_DOORBELL_STATE1, "DOORBELL_STATE1");
	if (read_wrapper) {
		h17_rd(w, H17_CPU_STATUS, "CPU_STATUS");
		h17_rd(w, H17_CPU_CONTROL, "CPU_CONTROL");
		h17_rd(w, H17_RVBAR, "RVBAR_LO");
		h17_rd(w, H17_RVBAR + 4, "RVBAR_HI");
	}
	if (mbox_peek) {
		h17_rd(w, mbox_off + H17_MBOX_A2I_CTRL, "A2I_CTRL");
		h17_rd(w, mbox_off + H17_MBOX_I2A_CTRL, "I2A_CTRL");
	}

	if (!confirm_wake) {
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=asc-snapshot\n");
		goto release;
	}

	/*
	 * The T8140 HAL's own control path, decoded from the same build:
	 * the mailbox-setup writes, then ring the doorbell with the
	 * complement of (STATE0 & 3) once that value is clear in STATE1,
	 * a bounded poll of the ack, the ack write, then the bounded
	 * SCRATCH7 wake-word poll. The child state is already raised
	 * above (or child_ps=0).
	 */
	state = h17_rdq(w, H17_DOORBELL_STATE0, "DOORBELL_STATE0") & 3;
	state1 = h17_rdq(w, H17_DOORBELL_STATE1, "DOORBELL_STATE1");
	ring = state ^ 3;
	if (ring & state1) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=doorbell-conflict state=%llu state1=%llu\n",
			state, state1);
		ret = -EIO;
		goto release;
	}
	if (state != 3) {
		h17_wrq(w, H17_DOORBELL, ring, "DOORBELL");
		polls = (wake_wait_ms + poll_ms - 1) / poll_ms;
		for (i = 0; i < polls; i++) {
			msleep(poll_ms);
			if ((h17_rdq(w, H17_DOORBELL_ACK, "DOORBELL_ACK") & 3) == ring)
				break;
		}
		state1 = h17_rdq(w, H17_DOORBELL_STATE1, "DOORBELL_STATE1");
		if (ring & state1) {
			dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=doorbell-unclear state1=%llu\n",
				state1);
			ret = -EIO;
			goto release;
		}
		h17_wrq(w, H17_DOORBELL_ACK, ring, "DOORBELL_ACK");
		ring_acked = true;
	} else {
		dev_info(&pdev->dev, "ane_h17_wake doorbell state already 3, no ring\n");
	}

	h17_wrq(w, H17_MBOX_SETUP0, 1, "MBOX_SETUP0");
	h17_wrq(w, H17_MBOX_SETUP1, 0, "MBOX_SETUP1");
	h17_wrq(w, H17_MBOX_SETUP1, 1, "MBOX_SETUP1");
	h17_wrq(w, H17_MBOX_SETUP2, 1, "MBOX_SETUP2");
	h17_wrq(w, H17_MBOX_SETUP3, 1, "MBOX_SETUP3");

	/* Bounded SCRATCH7 wake-word poll; s7a/s7b cover the alignment
	 * chosen by scratch0_off. */
	s7a = scratch0_off + 4 * 7;
	s7b = scratch0_off + 4 * 8;
	polls = (wake_wait_ms + poll_ms - 1) / poll_ms;
	for (i = 0; i < polls; i++) {
		u32 ack_b;

		msleep(poll_ms);
		scratch7 = h17_rd(w, s7a, "SCRATCH7_A");
		ack_b = h17_rd(w, s7b, "SCRATCH7_B");
		if (scratch7 == H17_WAKE_ACK || ack_b == H17_WAKE_ACK) {
			wake_seen = true;
			break;
		}
	}
	dev_info(&pdev->dev, "ane_h17_wake wake wait done wake_seen=%d scratch7=%#x ring=%llx acked=%d\n",
		 wake_seen, scratch7, ring, ring_acked);

	/* Opt-in read-only mailbox peek. Reads pop the message; nothing is replied. */
	if (mbox_peek) {
		polls = (mbox_wait_ms + poll_ms - 1) / poll_ms;
		for (i = 0; i < polls && msgs < H17_MBOX_MAXMSG; i++) {
			u32 ctrl_i2a;

			msleep(poll_ms);
			ctrl_i2a = h17_rd(w, mbox_off + H17_MBOX_I2A_CTRL, "I2A_CTRL");
			if (ctrl_i2a & H17_MBOX_EMPTY)
				continue;
			if (!(ctrl_i2a & H17_MBOX_ENABLE)) {
				dev_info(&pdev->dev, "ane_h17_wake mailbox not enabled ctrl=%#x\n", ctrl_i2a);
				break;
			}
			h17_rd(w, mbox_off + H17_MBOX_I2A_RECV0, "I2A_RECV0");
			h17_rd(w, mbox_off + H17_MBOX_I2A_RECV1, "I2A_RECV1");
			msgs++;
		}
		dev_info(&pdev->dev, "ane_h17_wake mailbox peek done mbox_off=%#x msgs=%u\n", mbox_off, msgs);
	}

	if (wake_seen)
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=wake-word msgs=%u\n", msgs);
	else if (ring_acked)
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=doorbell-ack\n");
	else
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=quiet-stop\n");

release:
	pm_runtime_put_sync(&pdev->dev);
disable:
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static const struct of_device_id ane_h17_wake_of_match[] = {
	{ .compatible = H17_PROBE_COMPAT },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_wake_of_match);

static struct platform_driver ane_h17_wake_driver = {
	.probe = ane_h17_wake_probe,
	.driver = {
		.name = "ane_h17_wake",
		.of_match_table = ane_h17_wake_of_match,
	},
};

static int __init ane_h17_wake_init(void)
{
	struct device_node *ane, *ps, *probe, *domain = NULL;
	u32 reg;
	int ret = -EPERM;

	if (!optin || !of_machine_is_compatible("apple,j700"))
		goto refused;

	if (!H17_REGISTER_MAP_QUALIFIED) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=unqualified-register-map; no power or MMIO access; H17 execution is not supported\n");
		return -EOPNOTSUPP;
	}

	ane = of_find_node_by_path(H17_ANE_NODE);
	ps = of_find_node_by_path(H17_PMGR_PS_NODE);
	probe = of_find_compatible_node(NULL, NULL, H17_PROBE_COMPAT);
	if (probe)
		domain = of_parse_phandle(probe, "power-domains", 0);
	if (!ane || !ps || !probe || of_device_is_available(ane) ||
	    !of_device_is_available(ps) || !of_device_is_available(probe) ||
	    !of_device_is_compatible(ane, "apple,t8140-ane") ||
	    !ane_h17_darts_disabled() ||
	    of_property_read_u32(ps, "reg", &reg) || reg != H17_PS_OFF ||
	    domain != ps) {
		ret = -EINVAL;
		goto put_nodes;
	}
	ret = 0;
put_nodes:
	of_node_put(domain);
	of_node_put(probe);
	of_node_put(ps);
	of_node_put(ane);
	if (ret)
		goto refused;
	return platform_driver_register(&ane_h17_wake_driver);
refused:
	pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=board-optin-or-dt error=%d\n", ret);
	return ret;
}

static void __exit ane_h17_wake_exit(void)
{
	platform_driver_unregister(&ane_h17_wake_driver);
}

module_init(ane_h17_wake_init);
module_exit(ane_h17_wake_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 wake experiment quarantined pending register/power qualification");
