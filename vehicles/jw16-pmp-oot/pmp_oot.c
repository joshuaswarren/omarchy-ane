// SPDX-License-Identifier: GPL-2.0
/*
 * pmp_oot.c - Apple PMP host-driven PM protocol, out-of-tree module.
 *
 * Runs on the STOCK 7.1.13-3-2-ARCH kernel: the DT pmp node compatible is
 * changed to "apple,t6000-pmp-oot" so the built-in apple_pmp (Rust) does not
 * bind, and this module does what the in-tree driver does (bring-up, syscall
 * serving) PLUS the ApplePMPv2 host-driven PM protocol recovered offline in
 * entries/Jw16PmpProto: class-0 Startup->Configure handshake, PM PING +
 * SET-DVFS-STATES, PTD SOC-DEV-PS-REQ arming, map113 DVFS_ON probe.
 *
 * A bug here is an oops / failed rmmod at worst - never a dead boot.
 *
 * Staging (module param pmp_stage, cumulative):
 *   0 = A: in-tree behaviour only (class-0/2 messages logged, not answered)
 *   1 = B: + answer a firmware Startup with Configure (64 KiB coherent shmem)
 *   3 = D: + PTD SOC-DEV-PS-REQ arm (ANE bits) + PTD row dump
 * PM-class engagement ladder (Jw16PmpOot2; the firmware never sends Startup
 * under Linux, so the stages act WITHOUT waiting for one - one insmod runs
 * C1..C4 sequentially, NO rmmod until close-out):
 *   C1 (pmp_c1): +pmp_c1_delay_ms after probe send Configure
 *       0x100000000000 | shmem_dva; retry every pmp_c1_retry_ms up to
 *       pmp_c1_retries times without a Configure_Ack.
 *   C2 (pmp_c2): then PM PING cmd 0 (expect ack 0x201).
 *   C3 (pmp_c3): REFUSED - the CISP_CMD_PMP_CTRL_SET scratch value layout
 *       (0x28e3d0868) is not pinnable offline (Jw16PmpOot2 entry); no guesses.
 *   C4 (pmp_c4): then cmd-6 SET-DVFS-STATES x16 (nub dvfs-domain table).
 *   C5 is stage-script side (proven pmp_dvfs.ko + granted ladder), gated on an
 *       ack being seen; pmp_map113=1 keeps only the raw write-1 discriminator.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/math64.h>
#include <linux/unaligned.h>
#include <linux/soc/apple/rtkit.h>

#define PMP_MMIO_SIZE		0x80000
#define ASC_MMIO_SIZE		0x4000
#define BOOTARGS_OFFSET		0x22c
#define BOOTARGS_SIZE		0x230
#define CPU_CONTROL		0x44
#define CPU_RUN			BIT(4)

#define PMP_ENDPOINT		0x20
#define PMP_CTRL_ENDPOINT	0x21

/* ep-0x20 message word: opc byte at bits 51:48 ((msg>>48)&0xff). */
#define OPC_SHIFT		48
#define OPC_ACK_MASK		0x1ULL
#define OPC_GET_IOVA_TABLE	0x10ULL
#define OPC_MALLOC		0x12ULL
#define OPC_FREE		0x14ULL
#define OPC_SET_BUF		0x30ULL
#define OPC_REGISTER_IOREG	0x32ULL
#define OPC_SET_IOREG		0x34ULL
#define OPC_STARTUP		0x00ULL
#define OPC_CONFIGURE_ACK	0x02ULL
#define OPC_PM			0x20ULL	/* any PM cmd < 16 (bit 53 set) */
#define OPC_PM_CMD16		0x21ULL	/* PM cmd 16 */
#define MALLOC_SIZE_MASK	0xFFFFFF
#define MSG_IOVA_MASK		0xFFFFFFFFFFFFULL
#define SET_IOREG_INDEX_MASK	0xFFFF
#define PIO_VM_BASE		0xc0000000ULL
#define PIO_GRANULARITY		0x1000000ULL
#define SHMEM_SIZE		0x10000
#define SHMEM_MAPS_OFFSET	0xe000

/* PM class: msg = BIT(53) | cmd<<44 | (arg1&0xf)<<40 | (arg2&0xff)<<32 |
 * (arg3&0xffff)<<16 | (arg4&0xffff)  (ApplePMP::_sendPMCommand). */
#define PM_MSG(cmd, a1, a2, a3, a4) \
	(BIT(53) | ((u64)(cmd) << 44) | (((u64)(a1) & 0xf) << 40) | \
	 (((u64)(a2) & 0xff) << 32) | (((u64)(a3) & 0xffff) << 16) | \
	 ((u64)(a4) & 0xffff))
#define PM_CMD(m)	(((m) >> 44) & 0x1f)

#define REPORT_SRAM_BASE	0x28e3c0000ULL
#define REPORT_SRAM_SIZE	0x20000
#define PTD_TABLE_BASE		0x10000
/* ptd-range nub row offsets (apple,tunable-ptd-range, verified). */
#define PTD_ROW_PMP_STATUS	0x1
#define PTD_ROW_DVFS_STATE	0x8
#define PTD_ROW_PS_REQ		0xf8
#define PTD_ROW_PS_ACK		0x100
#define PTD_ROW_SOC_DEV_DVFS	0x108
#define PTD_ADDR(row)		(PTD_TABLE_BASE + (row) * 8)

#define MAP113_BASE		0x400004000ULL
#define MAP113_SIZE		0x4000
#define MAP113_DVFS_CMD		0xa00
#define MAP113_DVFS_ON		0x2000

#define DVFS_DOMAIN_MAX		16
#define MAX_ALLOCS		64
#define MAX_IOREG_ENTRIES	512

static unsigned int pmp_stage = 0;
module_param(pmp_stage, uint, 0444);
MODULE_PARM_DESC(pmp_stage, "A=0 B=1 C=2 D=3 (cumulative protocol stages)");

static bool pmp_map113;
module_param(pmp_map113, bool, 0444);
MODULE_PARM_DESC(pmp_map113, "attempt map113 DVFS_ON write-1 (stage D probe)");

static bool pmp_c1 = true;
module_param(pmp_c1, bool, 0444);
MODULE_PARM_DESC(pmp_c1, "C1: proactive class-0 Configure without a Startup");

static bool pmp_c2 = true;
module_param(pmp_c2, bool, 0444);
MODULE_PARM_DESC(pmp_c2, "C2: PM PING cmd 0 after C1 (expect ack 0x201)");

static bool pmp_c3;
module_param(pmp_c3, bool, 0444);
MODULE_PARM_DESC(pmp_c3, "C3: refused - ISP scratch layout unpinnable offline");

static bool pmp_c4 = true;
module_param(pmp_c4, bool, 0444);
MODULE_PARM_DESC(pmp_c4, "C4: cmd-6 SET-DVFS-STATES for all nub domains");

static unsigned int pmp_c1_delay_ms = 3000;
module_param(pmp_c1_delay_ms, uint, 0444);
MODULE_PARM_DESC(pmp_c1_delay_ms, "delay after probe before the first C1 send");

static unsigned int pmp_c1_retry_ms = 1000;
module_param(pmp_c1_retry_ms, uint, 0444);
MODULE_PARM_DESC(pmp_c1_retry_ms, "C1 retry spacing without a Configure_Ack");

static unsigned int pmp_c1_retries = 5;
module_param(pmp_c1_retries, uint, 0444);
MODULE_PARM_DESC(pmp_c1_retries, "C1 retry count without a Configure_Ack");

static u64 pmp_ane_bits = BIT(10) | BIT(31);	/* ANE_SYS id1=11, ANE1_SYS id1=32 */
module_param(pmp_ane_bits, ullong, 0444);
MODULE_PARM_DESC(pmp_ane_bits, "SOC-DEV-PS-REQ bitmask to arm");

struct pmp_alloc {
	u64 addr;
	void *vaddr;
	size_t size;
};

struct pmp_dev {
	struct device *dev;
	struct platform_device *pdev;
	void __iomem *pmp;
	void __iomem *asc;
	struct apple_rtkit *rtk;

	struct mutex lock;

	/* syscall serving state (mirrors pmp.rs) */
	struct pmp_alloc allocs[MAX_ALLOCS];
	int n_allocs;
	u64 value_buf_addr;
	u32 ioreg_entries[MAX_IOREG_ENTRIES];
	int n_ioreg;

	/* PIO/IOVA table */
	struct {
		u64 host, pio, size;
	} pio[16];
	int n_pio;
	dma_addr_t iova_table_dma;
	void *iova_table;

	/* Configure shmem */
	dma_addr_t shmem_dma;
	void *shmem;
	u64 config_word;

	/* PM-class engagement ladder (C1..C4) */
	bool c1_sent, c2_sent, c4_sent, config_acked, ping_acked, ladder_done;
	unsigned int c1_retries;
	struct delayed_work ladder_work;

	bool ptd_armed;
	struct delayed_work ptd_work;

	spinlock_t report_lock;	/* guards map113/PTD cycles vs remove */
	bool dead;
};

static int pmp_send(struct pmp_dev *p, u8 ep, u64 msg)
{
	int ret = apple_rtkit_send_message(p->rtk, ep, msg, NULL, false);
	if (ret)
		dev_err(p->dev, "send ep %#x msg %#llx: %d\n", ep, msg, ret);
	return ret;
}

/* ---- bootargs patch (mirrors pmp.rs patch_bootargs) ---- */
static u32 fourcc(const char s[5])
{
	return (u32)(u8)s[3] | ((u32)(u8)s[2] << 8) |
	       ((u32)(u8)s[1] << 16) | ((u32)(u8)s[0] << 24);
}

struct bootargs_patch {
	u32 key;
	u32 val;
};

static void patch_bootargs(struct pmp_dev *p, const struct bootargs_patch *patches, int np)
{
	u32 offset = readl(p->pmp + BOOTARGS_OFFSET);
	u32 size = readl(p->pmp + BOOTARGS_SIZE);
	u8 *buf;
	u32 idx = 0;
	int i;

	if (!size || size > SZ_1M) {
		dev_err(p->dev, "bad bootargs size %u\n", size);
		return;
	}
	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return;
	memcpy_fromio(buf, p->pmp + offset, size);
	while (idx + 8 <= size) {
		u32 key = get_unaligned_le32(&buf[idx]);
		u32 esz = get_unaligned_le32(&buf[idx + 4]);
		u64 v;

		idx += 8;
		if (esz > size - idx)
			break;
		for (i = 0; i < np; i++) {
			if (patches[i].key != key)
				continue;
			v = cpu_to_le64((u64)patches[i].val);
			memcpy(&buf[idx], &v, min_t(u32, esz, 8));
			break;
		}
		idx += esz;
	}
	memcpy_toio(p->pmp + offset, buf, size);
	kfree(buf);
	dev_info(p->dev, "bootargs patched (%u bytes at %#x)\n", size, offset);
}

/* ---- syscall serving (mirrors pmp.rs) ---- */
static struct pmp_alloc *find_alloc(struct pmp_dev *p, u64 addr)
{
	int i;

	for (i = p->n_allocs - 1; i >= 0; i--)
		if (p->allocs[i].addr == addr)
			return &p->allocs[i];
	return NULL;
}

static u64 handle_get_iova_table(struct pmp_dev *p)
{
	struct property *prop;
	const __be32 *cells;
	size_t len;
	int i;

	mutex_lock(&p->lock);
	if (p->iova_table) {
		mutex_unlock(&p->lock);
		dev_err(p->dev, "Asked for iova table with existing buffer\n");
		return (u64)-EIO;
	}
	prop = of_find_property(p->dev->of_node, "apple,pio-ranges", NULL);
	if (!prop || prop->length < 16) {
		mutex_unlock(&p->lock);
		dev_info(p->dev, "no pio-ranges; empty table\n");
		return (OPC_GET_IOVA_TABLE | OPC_ACK_MASK) << OPC_SHIFT;
	}
	len = prop->length;
	cells = prop->value;
	p->n_pio = 0;
	for (i = 0; i + 4 <= len / 4; i += 4) {
		p->pio[p->n_pio].host = of_read_number(&cells[i], 2);
		p->pio[p->n_pio].size = of_read_number(&cells[i + 2], 2);
		p->n_pio++;
		if (p->n_pio >= ARRAY_SIZE(p->pio))
			break;
	}
	p->iova_table = dma_alloc_coherent(p->dev, 170 * 24,
					   &p->iova_table_dma, GFP_KERNEL);
	if (!p->iova_table) {
		p->n_pio = 0;
		mutex_unlock(&p->lock);
		return (u64)-ENOMEM;
	}
	{
		struct iommu_domain *dom = iommu_get_domain_for_dev(p->dev);
		u64 pio = PIO_VM_BASE;
		__le64 *t = p->iova_table;

		if (!dom) {
			dma_free_coherent(p->dev, 170 * 24, p->iova_table,
					  p->iova_table_dma);
			p->iova_table = NULL;
			p->n_pio = 0;
			mutex_unlock(&p->lock);
			return (u64)-ENODEV;
		}
		for (i = 0; i < p->n_pio; i++) {
			int err = iommu_map(dom, pio, p->pio[i].host,
					    p->pio[i].size,
					    IOMMU_READ | IOMMU_WRITE |
					    IOMMU_MMIO, GFP_KERNEL);
			if (err) {
				dev_err(p->dev, "iommu_map %#llx: %d\n",
					p->pio[i].host, err);
				break;
			}
			t[i * 3] = cpu_to_le64(p->pio[i].host);
			t[i * 3 + 1] = cpu_to_le64(pio);
			t[i * 3 + 2] = cpu_to_le64(p->pio[i].size);
			pio += PIO_GRANULARITY;
		}
		dev_info(p->dev, "iova table: %d/%d ranges mapped\n",
			 i, p->n_pio);
	}
	mutex_unlock(&p->lock);
	return (OPC_GET_IOVA_TABLE | OPC_ACK_MASK) << OPC_SHIFT |
	       (p->iova_table_dma & MSG_IOVA_MASK);
}

static u64 handle_malloc(struct pmp_dev *p, u64 size)
{
	void *v;
	dma_addr_t dma;
	u64 addr;
	u64 ret;

	if (!size || size > SZ_16M)
		return (u64)-EINVAL;
	v = dma_alloc_coherent(p->dev, size, &dma, GFP_KERNEL);
	if (!v)
		return (u64)-ENOMEM;
	addr = dma;
	mutex_lock(&p->lock);
	if (p->n_allocs >= MAX_ALLOCS) {
		mutex_unlock(&p->lock);
		dma_free_coherent(p->dev, size, v, dma);
		return (u64)-ENOMEM;
	}
	p->allocs[p->n_allocs++] = (struct pmp_alloc){ addr, v, size };
	mutex_unlock(&p->lock);
	ret = (OPC_MALLOC | OPC_ACK_MASK) << OPC_SHIFT | addr;
	return ret;
}

static u64 handle_free(struct pmp_dev *p, u64 addr)
{
	mutex_lock(&p->lock);
	{
		struct pmp_alloc *a = find_alloc(p, addr);
		void *v;
		size_t sz;
		dma_addr_t dma;

		if (!a) {
			mutex_unlock(&p->lock);
			dev_err(p->dev, "free of unknown alloc %#llx\n", addr);
			return (u64)-EIO;
		}
		v = a->vaddr;
		sz = a->size;
		dma = a->addr;
		*a = p->allocs[--p->n_allocs];
		dma_free_coherent(p->dev, sz, v, dma);
	}
	mutex_unlock(&p->lock);
	return (OPC_FREE | OPC_ACK_MASK) << OPC_SHIFT;
}

static u64 handle_set_buf(struct pmp_dev *p, u64 addr)
{
	mutex_lock(&p->lock);
	{
		struct pmp_alloc *a = find_alloc(p, addr);
		u64 ptr;

		if (!a || a->size < sizeof(u64)) {
			mutex_unlock(&p->lock);
			dev_err(p->dev, "set_buf: bad buffer %#llx\n", addr);
			return (u64)-EIO;
		}
		memcpy(&ptr, a->vaddr, sizeof(u64));
		p->value_buf_addr = ptr;
	}
	mutex_unlock(&p->lock);
	return (OPC_SET_BUF | OPC_ACK_MASK) << OPC_SHIFT;
}

static u64 handle_register_ioreg(struct pmp_dev *p, u64 addr)
{
	char name[0x31];
	char tunable[0x31 + 16];
	struct pmp_alloc *a, *vb;
	const u8 *data;
	int dlen;
	u32 sz;
	u64 ret;

	mutex_lock(&p->lock);
	a = find_alloc(p, addr);
	if (!a || a->size < 0x44) {
		mutex_unlock(&p->lock);
		dev_err(p->dev, "register_ioreg: bad buffer %#llx\n", addr);
		return (u64)-EIO;
	}
	sz = get_unaligned_le32((u8 *)a->vaddr + 0x40);
	if (sz == 0) {
		memcpy(name, a->vaddr, 0x30);
		name[0x30] = 0;
		if (!p->value_buf_addr) {
			mutex_unlock(&p->lock);
			dev_err(p->dev, "register_ioreg: no value buf\n");
			return (u64)-EIO;
		}
		vb = find_alloc(p, p->value_buf_addr);
		if (!vb) {
			mutex_unlock(&p->lock);
			dev_err(p->dev, "register_ioreg: no value buf alloc\n");
			return (u64)-EIO;
		}
		snprintf(tunable, sizeof(tunable), "apple,tunable-%s", name);
		data = of_get_property(p->dev->of_node, tunable, &dlen);
		if (data && dlen > 0 && dlen <= vb->size) {
			memcpy(vb->vaddr, data, dlen);
			sz = dlen;
		} else {
			dev_info(p->dev, "unknown property %s\n", tunable);
		}
	}
	if (p->n_ioreg >= MAX_IOREG_ENTRIES) {
		mutex_unlock(&p->lock);
		return (u64)-ENOMEM;
	}
	p->ioreg_entries[p->n_ioreg++] = sz;
	/* NOTE: 1-based index in the reply, 0-based in set_ioreg - this is
	 * exactly what the proven in-tree driver does (bug-compatible). */
	ret = (OPC_REGISTER_IOREG | OPC_ACK_MASK) << OPC_SHIFT |
	      ((u64)p->n_ioreg << 32) | sz;
	mutex_unlock(&p->lock);
	return ret;
}

static u64 handle_set_ioreg(struct pmp_dev *p, u64 index)
{
	u32 len;
	u64 ret;

	mutex_lock(&p->lock);
	if (index >= p->n_ioreg) {
		mutex_unlock(&p->lock);
		return (u64)-EIO;
	}
	len = p->ioreg_entries[index];
	ret = (OPC_SET_IOREG | OPC_ACK_MASK) << OPC_SHIFT | len;
	mutex_unlock(&p->lock);
	return ret;
}

/* ---- class 0 Configure handshake ---- */
static u64 configure_build(struct pmp_dev *p, const char *why)
{
	__le64 *maps;
	int i;
	void *shmem;
	dma_addr_t dma;

	mutex_lock(&p->lock);
	if (p->shmem) {
		mutex_unlock(&p->lock);
		dev_err(p->dev, "Configure with existing shmem\n");
		return (u64)-EIO;
	}
	shmem = dma_alloc_coherent(p->dev, SHMEM_SIZE, &dma, GFP_KERNEL);
	if (!shmem) {
		mutex_unlock(&p->lock);
		return (u64)-ENOMEM;
	}
	memset(shmem, 0, SHMEM_SIZE);
	if (p->iova_table) {
		maps = shmem + SHMEM_MAPS_OFFSET;
		for (i = 0; i < p->n_pio && (i + 1) * 24 <= SHMEM_SIZE -
			    SHMEM_MAPS_OFFSET; i++) {
			__le64 e[3] = {
				cpu_to_le64(p->pio[i].host),
				cpu_to_le64(p->pio[i].pio),
				cpu_to_le64(p->pio[i].size),
			};
			memcpy(&maps[i * 3], e, sizeof(e));
		}
	}
	p->shmem = shmem;
	p->shmem_dma = dma;
	p->config_word = ((u64)OPC_STARTUP + 1) << OPC_SHIFT |
			 (dma & MSG_IOVA_MASK);
	mutex_unlock(&p->lock);
	dev_info(p->dev, "%s -> Configure shmem dva %#llx\n", why, (u64)dma);
	return p->config_word;
}

static u64 handle_startup(struct pmp_dev *p)
{
	return configure_build(p, "PMP Startup");
}

static void send_dvfs_states(struct pmp_dev *p);

/* ---- PM-class engagement ladder (C1..C4, no Startup needed) ---- */
static void ladder_work_fn(struct work_struct *work)
{
	struct pmp_dev *p = container_of(work, struct pmp_dev, ladder_work.work);
	u64 ts;

	if (p->ladder_done)
		return;

	if (!p->c1_sent) {
		p->c1_sent = true;
		p->c1_retries = pmp_c1_retries;
		if (pmp_c1) {
			u64 word = configure_build(p, "C1: proactive (no Startup)");

			if ((s64)word >= 0 && !pmp_send(p, PMP_ENDPOINT, word))
				dev_info(p->dev,
					 "C1: Configure %#llx sent; %u retries every %u ms until ack\n",
					 word, p->c1_retries,
					 pmp_c1_retry_ms);
		}
		schedule_delayed_work(&p->ladder_work,
				      msecs_to_jiffies(pmp_c1_retry_ms));
		return;
	}
	if (!p->config_acked) {
		if (p->c1_retries) {
			p->c1_retries--;
			dev_info(p->dev, "C1: no Configure_Ack, retry %u/%u\n",
				 pmp_c1_retries - p->c1_retries,
				 pmp_c1_retries);
			pmp_send(p, PMP_ENDPOINT, p->config_word);
			schedule_delayed_work(&p->ladder_work,
					      msecs_to_jiffies(pmp_c1_retry_ms));
			return;
		}
		dev_info(p->dev, "C1: retry window exhausted\n");
	}
	if (!p->c2_sent) {
		p->c2_sent = true;
		if (pmp_c2) {
			ts = div_u64(ktime_get_ns(), NSEC_PER_MSEC) & 0xffffffff;

			if (!pmp_send(p, PMP_ENDPOINT,
				      PM_MSG(0, 0, 0, ts >> 16, ts & 0xffff)))
				dev_info(p->dev,
					 "C2: PM PING sent (expect ack 0x201), ts %#llx\n",
					 ts);
		}
		schedule_delayed_work(&p->ladder_work, msecs_to_jiffies(1000));
		return;
	}
	/* C4: after C2 (ping acked or the 1 s observation window) */
	if (pmp_c4 && !p->c4_sent) {
		p->c4_sent = true;
		send_dvfs_states(p);
	}
	p->ladder_done = true;
	dev_info(p->dev,
		 "C1..C4 ladder complete: config_acked=%d ping_acked=%d cmd6_sent=%d\n",
		 p->config_acked, p->ping_acked, p->c4_sent);
}

/* ---- PM class: C4 cmd-6 SET-DVFS-STATES (nub dvfs-domain table) ---- */
static void send_dvfs_states(struct pmp_dev *p)
{
	const u8 *dom;
	int dlen, i, n;

	dom = of_get_property(p->dev->of_node, "apple,tunable-dvfs-domain",
			      &dlen);
	if (!dom || dlen < 28) {
		dev_err(p->dev, "no dvfs-domain nub property\n");
		return;
	}
	n = dlen / 28;
	for (i = 0; i < n && i < DVFS_DOMAIN_MAX; i++) {
		u32 d = get_unaligned_le32(dom + i * 28);
		u32 mn = get_unaligned_le32(dom + i * 28 + 4);
		u32 enc = get_unaligned_le32(dom + i * 28 + 8);
		u64 msg = PM_MSG(6, d ? d - 1 : 0, 0, mn, enc);

		if (pmp_send(p, PMP_ENDPOINT, msg))
			return;
	}
	dev_info(p->dev, "PMP PM: SET-DVFS-STATES sent for %d domains\n", n);
}

/* ---- stage D: PTD arm + rows + map113 ---- */
static void __iomem *report_base;
static void __iomem *map113_base;

static void ptd_show_rows(struct pmp_dev *p)
{
	if (!report_base)
		return;
	dev_info(p->dev,
		 "PTD rows: PMP-STATUS=%#llx DVFS-STATE=%#llx PS-REQ=%#llx PS-ACK=%#llx SOC-DEV-DVFS=%#llx\n",
		 readq(report_base + PTD_ADDR(PTD_ROW_PMP_STATUS)),
		 readq(report_base + PTD_ADDR(PTD_ROW_DVFS_STATE)),
		 readq(report_base + PTD_ADDR(PTD_ROW_PS_REQ)),
		 readq(report_base + PTD_ADDR(PTD_ROW_PS_ACK)),
		 readq(report_base + PTD_ADDR(PTD_ROW_SOC_DEV_DVFS)));
}

static void ptd_work_fn(struct work_struct *work)
{
	struct pmp_dev *p = container_of(work, struct pmp_dev, ptd_work.work);
	u64 cur;

	if (!report_base) {
		report_base = ioremap(REPORT_SRAM_BASE, REPORT_SRAM_SIZE);
		if (!report_base) {
			dev_err(p->dev, "ioremap report SRAM failed\n");
			return;
		}
	}
	cur = readq(report_base + PTD_ADDR(PTD_ROW_PS_REQ));
	writeq(cur | pmp_ane_bits, report_base + PTD_ADDR(PTD_ROW_PS_REQ));
	p->ptd_armed = true;
	dev_info(p->dev, "PTD PS-REQ %#llx -> %#llx (readback %#llx)\n",
		 cur, cur | pmp_ane_bits,
		 readq(report_base + PTD_ADDR(PTD_ROW_PS_REQ)));
	ptd_show_rows(p);

	if (!pmp_map113)
		return;
	if (!map113_base) {
		map113_base = ioremap(MAP113_BASE, MAP113_SIZE);
		if (!map113_base) {
			dev_err(p->dev, "ioremap map113 failed\n");
			return;
		}
	}
	dev_info(p->dev, "map113 DVFS_ON read %#llx\n",
		 readq(map113_base + MAP113_DVFS_ON));
	writeq(1, map113_base + MAP113_DVFS_ON);
	dev_info(p->dev, "map113 DVFS_ON write-1 readback %#llx (%s)\n",
		 readq(map113_base + MAP113_DVFS_ON),
		 readq(map113_base + MAP113_DVFS_ON) == 1 ? "ACCEPTED" :
		 "refused");
}

/* ---- RTKit callbacks ---- */
static void pmp_recv_message(void *cookie, u8 ep, u64 msg)
{
	struct pmp_dev *p = cookie;
	u64 opc = (msg >> OPC_SHIFT) & 0xff;
	u64 reply;
	int ret;

	switch (opc) {
	case OPC_GET_IOVA_TABLE:
		reply = handle_get_iova_table(p);
		break;
	case OPC_MALLOC:
		reply = handle_malloc(p, msg & MALLOC_SIZE_MASK);
		break;
	case OPC_FREE:
		reply = handle_free(p, msg & MSG_IOVA_MASK);
		break;
	case OPC_SET_BUF:
		reply = handle_set_buf(p, msg & MSG_IOVA_MASK);
		break;
	case OPC_REGISTER_IOREG:
		reply = handle_register_ioreg(p, msg & MSG_IOVA_MASK);
		break;
	case OPC_SET_IOREG:
		reply = handle_set_ioreg(p, msg & SET_IOREG_INDEX_MASK);
		break;
	case OPC_STARTUP:
		if (pmp_stage >= 1) {
			u64 reply = handle_startup(p);

			/* the answer IS the first Configure: seed the ladder */
			if ((s64)reply >= 0 && !p->c1_sent) {
				p->c1_sent = true;
				p->c1_retries = pmp_c1_retries;
			}
		} else {
			dev_info(p->dev,
				 "PMP Startup (class0) seen, stage A: not answering\n");
		}
		break;
	case OPC_CONFIGURE_ACK:
		p->config_acked = true;
		dev_info(p->dev,
			 "C1: Configure_Ack: fw memory base %#llx raw %#llx\n",
			 (msg & 0x3ffffff) << 12, msg);
		return;
	case OPC_PM:
	case OPC_PM_CMD16:
		dev_info(p->dev,
			 "PMP PM: ep %#x cmd %llu raw %#llx args %#x %#x %#x %#x\n",
			 ep, PM_CMD(msg), msg,
			 (unsigned)((msg >> 40) & 0xf),
			 (unsigned)((msg >> 32) & 0xff),
			 (unsigned)((msg >> 16) & 0xffff),
			 (unsigned)(msg & 0xffff));
		if (PM_CMD(msg) == 1) {
			p->ping_acked = true;
			dev_info(p->dev, "C2: PING acked (0x201) raw %#llx\n",
				 msg);
		}
		return;
	default:
		dev_info(p->dev,
			 "Got unknown message: ep %#x class %u raw %#llx\n",
			 ep, (unsigned)((msg >> 52) & 0xf), msg);
		return;
	}

	if ((s64)reply == -EIO || (s64)reply == -ENOMEM ||
	    (s64)reply == -EINVAL || (s64)reply == -ENODEV) {
		dev_err(p->dev, "Failed to handle rtkit message %#llx: %lld\n",
			msg, (s64)reply);
		return;
	}
	ret = pmp_send(p, ep, reply);
	if (ret)
		dev_err(p->dev, "Failed to send reply for %#llx: %d\n", msg,
			ret);
}

static void pmp_crashed(void *cookie, const void *crashlog, size_t size)
{
	struct pmp_dev *p = cookie;

	dev_err(p->dev, "PMP firmware crashed (crashlog %zu bytes)\n", size);
}

static const struct apple_rtkit_ops pmp_rtkit_ops = {
	.recv_message = pmp_recv_message,
	.crashed = pmp_crashed,
};

/* ---- probe / remove ---- */
static int pmp_oot_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pmp_dev *p;
	u32 board_id, dvid, dcap;
	bool have_dcap;
	int ret;

	if (!dev->of_node)
		return -ENODEV;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->pdev = pdev;
	mutex_init(&p->lock);
	spin_lock_init(&p->report_lock);
	INIT_DELAYED_WORK(&p->ptd_work, ptd_work_fn);
	INIT_DELAYED_WORK(&p->ladder_work, ladder_work_fn);
	platform_set_drvdata(pdev, p);

	p->pmp = devm_platform_ioremap_resource_byname(pdev, "pmp");
	if (IS_ERR(p->pmp))
		return dev_err_probe(dev, PTR_ERR(p->pmp), "iomap pmp\n");
	p->asc = devm_platform_ioremap_resource_byname(pdev, "asc");
	if (IS_ERR(p->asc))
		return dev_err_probe(dev, PTR_ERR(p->asc), "iomap asc\n");

	if (of_property_read_u32(dev->of_node, "apple,board-id", &board_id) ||
	    of_property_read_u32(dev->of_node, "apple,dram-vendor-id", &dvid)) {
		dev_err(dev, "missing board-id/dram-vendor-id\n");
		return -EINVAL;
	}
	have_dcap = !of_property_read_u32(dev->of_node, "apple,dram-capacity",
					  &dcap);
	if (have_dcap) {
		const struct bootargs_patch p4[] = {
			{ fourcc("BDID"), board_id },
			{ fourcc("DCAP"), dcap },
			{ fourcc("DVID"), dvid },
		};
		patch_bootargs(p, p4, 3);
	} else {
		const struct bootargs_patch p3[] = {
			{ fourcc("BDID"), board_id },
			{ fourcc("DVID"), dvid },
		};
		patch_bootargs(p, p3, 2);
	}

	p->rtk = apple_rtkit_init(dev, p, NULL, 0, &pmp_rtkit_ops);
	if (IS_ERR(p->rtk))
		return dev_err_probe(dev, PTR_ERR(p->rtk), "rtkit init\n");

	/* start the PMP CPU (asc+0x44 RUN) */
	{
		u32 val = readl(p->asc + CPU_CONTROL);

		writel(val | CPU_RUN, p->asc + CPU_CONTROL);
	}

	ret = apple_rtkit_wake(p->rtk);
	if (ret)
		goto err_free;
	ret = apple_rtkit_start_ep(p->rtk, PMP_ENDPOINT);
	if (ret)
		goto err_free;
	if (pmp_stage >= 1) {
		ret = apple_rtkit_start_ep(p->rtk, PMP_CTRL_ENDPOINT);
		if (ret)
			dev_info(dev, "pmp_ctrl endpoint %#x not started: %d\n",
				 PMP_CTRL_ENDPOINT, ret);
	}
	dev_info(dev, "apple_pmp_oot: probe done (stage %u)\n", pmp_stage);
	if (pmp_c3)
		dev_info(dev,
			 "C3 refused: ISP scratch 0x28e3d0868 value layout unpinnable offline (Jw16PmpOot2); not writing\n");
	if (pmp_c1 || pmp_c2 || pmp_c4)
		schedule_delayed_work(&p->ladder_work,
				      msecs_to_jiffies(pmp_c1_delay_ms));
	if (pmp_stage >= 3)
		schedule_delayed_work(&p->ptd_work,
				      msecs_to_jiffies(3000));
	return 0;

err_free:
	apple_rtkit_free(p->rtk);
	return ret;
}

static void pmp_oot_remove(struct platform_device *pdev)
{
	struct pmp_dev *p = platform_get_drvdata(pdev);
	int i;

	cancel_delayed_work_sync(&p->ladder_work);
	cancel_delayed_work_sync(&p->ptd_work);
	if (p->rtk) {
		apple_rtkit_shutdown(p->rtk);
		apple_rtkit_free(p->rtk);
	}
	mutex_lock(&p->lock);
	for (i = 0; i < p->n_allocs; i++)
		dma_free_coherent(p->dev, p->allocs[i].size, p->allocs[i].vaddr,
				  p->allocs[i].addr);
	p->n_allocs = 0;
	if (p->shmem) {
		dma_free_coherent(p->dev, SHMEM_SIZE, p->shmem, p->shmem_dma);
		p->shmem = NULL;
	}
	if (p->iova_table) {
		dma_free_coherent(p->dev, 170 * 24, p->iova_table,
				  p->iova_table_dma);
		p->iova_table = NULL;
	}
	mutex_unlock(&p->lock);
	dev_info(&pdev->dev, "apple_pmp_oot: removed\n");
}

static const struct of_device_id pmp_oot_of_match[] = {
	{ .compatible = "apple,t6000-pmp-oot" },
	{ }
};
MODULE_DEVICE_TABLE(of, pmp_oot_of_match);

static struct platform_driver pmp_oot_driver = {
	.probe = pmp_oot_probe,
	.remove = pmp_oot_remove,
	.driver = {
		.name = "apple-pmp-oot",
		.of_match_table = pmp_oot_of_match,
	},
};
module_platform_driver(pmp_oot_driver);

MODULE_AUTHOR("Joshua Warren");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apple PMP out-of-tree driver with host-driven PM protocol");
