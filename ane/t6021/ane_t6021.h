// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* T6021 (H14 / J414c) Apple Neural Engine — skeleton constants and types.
 *
 * Every constant below is static-decode provenance, no device writes:
 *  - reg windows, IRQ, pmgr islands: ADT j414c (DeviceTree.j414cap.im4p,
 *    receipts/2026-09-18-t6021-engine-layout-mined §2), Linux translation
 *    +0x200000000 (proven class, pmgr low-32 match).
 *  - ASC cpu block + mailbox: K14 kext disasm
 *    (receipts/2026-09-18-h14-rtkit-port-phase1 §2), m1n1 ASCRegs layout.
 *  - RTKit MGMT protocol: Asahi rtkit.c semantics, u64 message halves as
 *    staged in omarchy-ane rtkit/h14_rtkit_hello.py (commit 6ad26b7).
 *  - RTBuddy endpoint table + doorbell word: K14 kext cfg table
 *    __DATA_CONST.__const+0x814e520 and HandleRTBuddyMessage
 *    (receipts/2026-09-18-h14-w2-protocol-decode §3).
 *
 * DT binding (driver + ane/t6021-j414c-ane.dts are the two halves):
 *  compatible    = "apple,t6021-ane"
 *  reg/reg-names = "engine" (whole 32 MiB ADT range0, 0x284000000;
 *                  the H13-style +0x1c04000 engine delta does not exist
 *                  on this SoC — kext never computes it and first touch
 *                  external-aborted, proven 2026-09-18),
 *                  "pmgr" (0x28e080000+0x4034, pmgr1,t6021 island words),
 *                  "set"  (0x28e08c000+0x4000, SET window — read-only by
 *                  repo rule: direct SET writes external-abort the SoC)
 *  interrupts    = one AIC2 level-high interrupt named "ane" (raw 884;
 *                  [INFERENCE W2 §3: the ANE ASC mailbox doorbell IRQ].
 *                  dart-ane0 carries raw 885, provider-owned, never
 *                  fetched here)
 *  iommus        = dart-ane0 streams (apple,t6020-dart/apple,t8110-dart
 *                  nodes). The ADT dart-ane0 is one hardware block with a
 *                  four-window quartet (0x85800000/85810000/85820000/
 *                  85804000, each 0x4000); the installed overlay's
 *                  three-node split (t6001-proven pattern) is kept.
 *  power-domains = six: ane_cpu@2e0, ane_set1..4@4018-4030,
 *                  ane_sys_mpm@4000 (phandle chain per the overlay)
 */

#ifndef __ANE_T6021_H__
#define __ANE_T6021_H__

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/math.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/mutex.h>

/* reg windows (ane/t6021-j414c-ane.dts reg-names order) */
enum {
	ANE_T6021_REG_ENGINE,
	ANE_T6021_REG_PMGR,
	ANE_T6021_REG_SET,
	ANE_T6021_REG_COUNT
};

/* ANE-block-relative ASC/RTBuddy addresses (phase1 §2) */
#define ANE_ASC_CPU_CONTROL	0x1600044	/* RUN = BIT(4) */
#define ANE_ASC_RVBAR		0x1050000	/* fw entry | valid bit0 */
#define ANE_ASC_VERS		0x1840000
#define ANE_ASC_RTB_STATUS	0x1840088	/* K14 poll: value < 2 */
#define ANE_ASC_RTB_GPIO0	0x1840048	/* ack GPIOs 0..7, +0x48..+0x64 */

/* ASC mailbox, m1n1 ASCRegs layout at block +0x1608000 (= Asahi
 * apple-mailbox ASC variant: ctrl 0x110/0x114, send/recv 0x800-family,
 * FULL BIT(16) / EMPTY BIT(17); drivers/soc/apple/mailbox.c) */
#define ANE_MBOX_A2I_CONTROL	0x1608110
#define ANE_MBOX_I2A_CONTROL	0x1608114
#define ANE_MBOX_A2I_SEND0	0x1608800	/* u64 msg0 */
#define ANE_MBOX_A2I_SEND1	0x1608808	/* u32 msg1 (endpoint) */
#define ANE_MBOX_I2A_RECV0	0x1608830	/* u64 msg0 */
#define ANE_MBOX_I2A_RECV1	0x1608838	/* u32 msg1 */

#define ANE_MBOX_CONTROL_FULL	BIT(16)
#define ANE_MBOX_CONTROL_EMPTY	BIT(17)
#define ANE_MBOX_TX_TIMEOUT	500		/* ms, apple-mailbox.c value */

/* RTKit MGMT (EP 0); type bits [59:52], u64 message halves */
#define ANE_RTKIT_TYPE			GENMASK_ULL(59, 52)
#define ANE_RTKIT_MGMT_HELLO		1
#define ANE_RTKIT_MGMT_HELLO_REPLY	2
#define ANE_RTKIT_MGMT_STARTEP		5
#define ANE_RTKIT_MGMT_SET_IOP_PWR_STATE	6
#define ANE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK	7
#define ANE_RTKIT_MGMT_EPMAP		8
#define ANE_RTKIT_MGMT_SET_AP_PWR_STATE		0xb
#define ANE_RTKIT_MGMT_SET_AP_PWR_STATE_ACK	0xb

#define ANE_RTKIT_HELLO_MINVER		GENMASK_ULL(15, 0)
#define ANE_RTKIT_HELLO_MAXVER		GENMASK_ULL(31, 16)
#define ANE_RTKIT_EPMAP_LAST		BIT_ULL(51)
#define ANE_RTKIT_EPMAP_BASE		GENMASK_ULL(34, 32)
#define ANE_RTKIT_EPMAP_BITMAP		GENMASK_ULL(31, 0)
#define ANE_RTKIT_EPMAP_REPLY_MORE	BIT_ULL(0)
#define ANE_RTKIT_STARTEP_EP		GENMASK_ULL(39, 32)
#define ANE_RTKIT_STARTEP_FLAG		BIT_ULL(1)
#define ANE_RTKIT_PWR_STATE		GENMASK_ULL(15, 0)
#define ANE_RTKIT_PWR_STATE_ON		0x20

#define ANE_RTKIT_VER_MIN	11
#define ANE_RTKIT_VER_MAX	12

/* RTKit system endpoints rtkit.c starts when announced */
#define ANE_RTKIT_EP_CRASHLOG	1
#define ANE_RTKIT_EP_SYSLOG	2
#define ANE_RTKIT_EP_DEBUG	3
#define ANE_RTKIT_EP_IOREPORT	4
#define ANE_RTKIT_EP_OSLOG	8
#define ANE_RTKIT_EP_TRACEKIT	0xa

/* RTBuddy app endpoints — K14 InitializeRTBuddyEndpoints opens ids 1..6;
 * ring sizes + fourccs from the per-EP config table (W2 §3). fourcc is
 * byte-reversed in the table (0x54324643 = "T2FC"). */
enum ane_t6021_eps {
	ANE_T6021_EP_INIT = 1,	/* INIT — CSNE_CMD controller channel (W4) */
	ANE_T6021_EP_T2FC,	/* fw->host commands */
	ANE_T6021_EP_T2FH,	/* fw->host commands */
	ANE_T6021_EP_T2HS,
	ANE_T6021_EP_T2HC,
	ANE_T6021_EP_T2HT,	/* polled on the host */
	ANE_T6021_EP_COUNT = ANE_T6021_EP_T2HT + 1	/* arrays index by id */
};

/* CSNE command ids — selene id->name table, vaddr 0xea430, 96 entries
 * (full set: receipts/2026-09-18-h14-w2-protocol-decode §4 and
 * receipts/2026-09-18-h14-w2-protocol-decode/fw_cmd_table.json). The
 * fw parses the id as u16 at wire offset +4 (W4 correction of the W2
 * §4 "offset 0" claim: fw sites 0x4d244/0x4d264/0x5246c); the kext
 * controller header for the fw->host direction is a different shape
 * (u32 id @ +0x8, 0x24 bytes) and never rides host->fw submission. */
enum ane_t6021_csne_cmd {
	CSNE_CMD_START		= 0x0000,
	CSNE_CMD_STOP		= 0x0001,
	CSNE_CMD_REG_FILE_LOAD	= 0x0005,	/* fw _rtk_tunables 1456 B */
	CSNE_CMD_BUILDINFO	= 0x0006,
	CSNE_CMD_BOOT		= 0x0010,
	CSNE_CMD_PING		= 0x0011,
	CSNE_CMD_POWER_DEVICE_ON	= 0x0013,
	CSNE_CMD_IPC_ENDPOINT_SET	= 0x0015,
	CSNE_CMD_IPC_ENDPOINT_UNSET	= 0x0016,
	CSNE_CMD_PROCEDURE_CALL	= 0x0204,
	CSNE_CMD_INFERENCE_CALL	= 0x0404,
	CSNE_CMD_BACK_CHANNEL_RPC	= 0x7000,
};

/* Doorbell word (app-EP ring notification, W2 §3): 54-bit packed
 * offset[43:0] | size_code[51:44] | unit[53:52]; unit 0=code bytes,
 * 1=code*4K, 2=code*1M, 3=code*2M. K14 SetupEndpoints encodes with
 * unit 1 below 1 MiB and unit 2 at or above, and the size code is a
 * CEILING division (cinc on remainder @0x…95fe8dc) — the reconstructed
 * receive-side size is the rounded-up value. */
#define ANE_EP_DOORBELL_OFFSET	GENMASK_ULL(43, 0)
#define ANE_EP_DOORBELL_SIZE	GENMASK_ULL(51, 44)
#define ANE_EP_DOORBELL_UNIT	GENMASK_ULL(53, 52)

static inline u64 ane_ep_doorbell_encode(u32 offset, u32 size)
{
	u64 unit = (size >= SZ_1M) ? 2 : 1;	/* K14 size-class encoder */
	u32 code = DIV_ROUND_UP(size, 1u << (unit * 12));

	return (offset & ANE_EP_DOORBELL_OFFSET) |
	       FIELD_PREP(ANE_EP_DOORBELL_SIZE, code) |
	       FIELD_PREP(ANE_EP_DOORBELL_UNIT, unit);
}

static inline u32 ane_ep_doorbell_size(u64 msg)
{
	static const u8 shift[] = { 0, 12, 20, 21 };
	u32 unit = FIELD_GET(ANE_EP_DOORBELL_UNIT, msg);

	return FIELD_GET(ANE_EP_DOORBELL_SIZE, msg) << shift[unit];
}

struct ane_t6021_ep {
	u8 id;
	const char *name;
	u32 fourcc;
	u32 ring_size;
	u32 write_cursor;	/* next ring slot (K14 rec+0x20) */
	void *ring;			/* dma_alloc_coherent, ring_size */
	dma_addr_t ring_iova;
	bool started;
};

struct ane_t6021 {
	struct device *dev;
	void __iomem *base[ANE_T6021_REG_COUNT];
	int irq;

	struct device **pd_dev;
	struct device_link **pd_link;
	int pd_count;

	/* Single mailbox consumer (threaded IRQ + probe drain serialize
	 * here); sends ride the same lock to keep the 1-deep FIFO sane. */
	struct mutex mbox_lock;

	DECLARE_BITMAP(announced, 256);	/* EPMAP accumulation */
	bool booted;		/* SET_AP_PWR_STATE ACK seen */

	struct ane_t6021_ep ep[ANE_T6021_EP_COUNT];
};

/* ane_t6021_rtkit.c */
int ane_t6021_rtkit_init(struct ane_t6021 *ane);
void ane_t6021_rtkit_shutdown(struct ane_t6021 *ane);
void ane_t6021_rtkit_drain(struct ane_t6021 *ane);
irqreturn_t ane_t6021_rtkit_irq_thread(int irq, void *data);

/* ---- CSNE_CMD wire structs (host->fw on the INIT channel) ----
 *
 * W4 static decode, extending W2 §4 (the receipt's "fw parses a u16 id
 * at header offset 0" was WRONG — three independent fw parse sites read
 * the id as u16 at offset +4: the generic CSNE processor @0x4d264, its
 * preload @0x4d244, and the LOAD_PROGRAM/CREATE_PROCESS/PROCEDURE_CALL
 * pre-parse @0x5246c, all `ldrh wN, [xM, #4]` in selene). Wire is
 * little-endian; this module is arm64-only so host order is wire order.
 *
 * The generic processor takes commands < 0x1b89 bytes (work-item size
 * bound @0x4d134-0x4d158) and writes completion state back into the
 * command block (strb @0x4d324 byte +6, str @0x4d3b0 qword +8) — the
 * ring slot doubles as the response area. PING/BUILDINFO/BOOT/
 * REG_FILE_LOAD/IPC_ENDPOINT_SET fall through the processor's id tree
 * to the default path (0x4e65c): carried without field parsing, so
 * their payloads beyond the header are opaque until the W1 live
 * exchange pins them. */
struct ane_csne_hdr {
	u32 rsvd0;	/* bytes 0..3: never read by the fw processor; 0 */
	u16 id;		/* PROVEN u16 @ +4 (sites above) */
	u8 flags;	/* fw-written byte @ +6 (status/scratch) [INFERENCE] */
	u8 rsvd7;
};
static_assert(sizeof(struct ane_csne_hdr) == 8);

static inline void ane_csne_hdr_init(struct ane_csne_hdr *h, u16 id)
{
	memset(h, 0, sizeof(*h));
	h->id = id;
}

/* Header-only CSNE_CMDs: BOOT (0x10 — boot-arg surfaces ride
 * SCRATCH0-7 / SetupFWInitBootArgs, not the command, phase1 §2.5),
 * PING (0x11), BUILDINFO (0x06). sizeof(struct ane_csne_hdr) bytes. */

/* REG_FILE_LOAD (0x05) payload: the 1456 B blob is the fw's own
 * __DATA._rtk_tunables section (@0x100590, size 0x5b0) — its transport
 * (inline vs shared-memory iova) is [INFERENCE], pinned by W1. */
struct ane_csne_cmd_reg_file_load {
	struct ane_csne_hdr hdr;
	u8 blob[];
};

/* IPC_ENDPOINT_SET (0x15) payload: binds a host RTBuddy endpoint to a
 * fw channel. No field parsed by the fw processor — layout
 * [INFERENCE], pinned by W1. */
struct ane_csne_cmd_ipc_endpoint_set {
	struct ane_csne_hdr hdr;
	u8 payload[];
};

/* PROCEDURE_CALL (0x204) / INFERENCE_CALL (0x404) — the kext
 * CANE_SUB_PACKET_CMD_PROCEDURE_CALL family. Field offsets PROVEN from
 * the fw parse; field semantics named only where the kext asserts name
 * them (programId, procedureId, numIoBuffers in assert order, W2 §4):
 *   +0x08/+0x0c u32 pair validated together (ldp @0x4d654)
 *   +0x10        u64 passed to the fw validator (0x4d6d4)
 *   +0x18        u32 required in [8,15] (@0x4d660-0x4d66c)
 *   +0x28        u32 element count (loop bound @0x524bc-0x524c4)
 *   +0x60        count × 0x30-byte io-buffer records (@0x524a8-0x524b4)
 * The kext `size % sizeof(sCSneCmdProcedureCall) == 0` assert says
 * commands form same-shape arrays in the ring; this driver submits one
 * command per slot. INFERENCE_CALL shares the shape [INFERENCE: the
 * 0x404 id is not in the decoded id tree — it is the W4 submission
 * endpoint per the phase-1 workstream plan]. */
struct ane_csne_io_elem {
	u8 bytes[0x30];	/* internal layout not decoded */
};

struct ane_csne_cmd_procedure_call {
	struct ane_csne_hdr hdr;
	u32 program_id;		/* +0x08 */
	u32 procedure_id;	/* +0x0c */
	u64 field_10;
	u32 field_18;		/* fw requires 8..15 */
	u32 rsvd_1c;
	u64 field_20;
	u32 num_io_buffers;	/* +0x28 = element count */
	u32 rsvd_2c;
	u8 gap_30[0x30];
	struct ane_csne_io_elem io[];
};
static_assert(offsetof(struct ane_csne_cmd_procedure_call, program_id) == 0x08);
static_assert(offsetof(struct ane_csne_cmd_procedure_call, procedure_id) == 0x0c);
static_assert(offsetof(struct ane_csne_cmd_procedure_call, field_10) == 0x10);
static_assert(offsetof(struct ane_csne_cmd_procedure_call, field_18) == 0x18);
static_assert(offsetof(struct ane_csne_cmd_procedure_call, num_io_buffers) == 0x28);
static_assert(offsetof(struct ane_csne_cmd_procedure_call, io) == 0x60);

static inline size_t
ane_csne_cmd_procedure_call_size(unsigned int num_io_buffers)
{
	return sizeof(struct ane_csne_cmd_procedure_call) +
	       num_io_buffers * sizeof(struct ane_csne_io_elem);
}

/* Fw-side bound: the generic CSNE processor rejects work items of
 * 0x1b89 bytes and above (@0x4d134 cmp x2, #0x1b89). What x2 names
 * beyond "the command's size" is [INFERENCE] — enforced here as a
 * fail-fast so an oversized command cannot enter the ring. */
#define ANE_CSNE_CMD_MAX_SIZE	0x1b88

/* Submit one CSNE command block on the INIT (EP1) ring: K14
 * rtbuddyEndpointSendMessage semantics (W2 §3) — slot alloc with wrap,
 * memcpy into the ring, 54-bit doorbell on the mailbox, cursor
 * advanced only on doorbell success. No synchronous response matching:
 * fw->host responses arrive on the T2F* channels and are not walked
 * yet (W2 §3). Sleeps (mutex, mailbox poll) — process context only. */
int ane_t6021_csne_submit(struct ane_t6021 *ane, const void *cmd, size_t size);

#endif /* __ANE_T6021_H__ */
