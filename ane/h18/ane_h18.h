/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h18.h: per-SoC table for the H18 ANE (T8152, Mac mini M6, board J873g).
 *
 * Sources of every number, by exact commit or file:
 * - Window addresses, sizes, mailbox number and PSD slots: the live M6 ADT dump (/arm-io/ane: reg,
 *   power-gates [30, 31], clock-gates [30, 31], service-gates, msgbox-mailbox-num 12, msgbox-using-postoffice
 *   true). Each ADT address plus 0x200000000 is the physical address, the translation the kernel tree's
 *   t8152-j873 device tree uses (linux-aurora, j873 tip 4513ee61d6a0, "translated ADT addresses").
 * - ASC wrapper register offsets (CPU_CONTROL 0x44, CPU_STATUS 0x48, ASC_STATUS 0x8048, IORVBAR at +0 of its block):
 *   UNVERIFIED by any checked-in receipt. They come from macOS 27.0 (26A434) behaviour recorded
 *   in the lab notebook (2026-10-19), which this repository does not carry. The board has
 *   confirmed only that reads of CPU_STATUS, ASC_STATUS and IORVBAR at these offsets return sane values with the
 *   PSD slots on (stage power) and that CPU_CONTROL reads 0 before the core runs (read_ctrl).
 * - PSD slot window layout and request protocol: drivers/pmdomain/apple/pmgr-psd.c of the same kernel tree,
 *   proven on this board for slot 71 (ATC0 USB).
 * - RTKit management protocol and power state values (drivers/soc/apple/rtkit.c) and the T8152 counted FIFO of a Postoffice channel (mailbox.c):
 *   linux-aurora commit dc7b5b9c539b85bdf2557f2a0eb44223a4219de9. Gen3 DART layout: drivers/iommu/apple-dart.c of the kernel tree this Image is
 *   built from, which no public commit carries yet; its sha256 is 2904d16b83d0ae065586e16157cc5b54e7233c174c51dcda1c70c2b02a637621.
 * INFERENCE, not measured: that ANE slots 30 and 31 accept state 0 (run) and what the ASC wrapper reads
 * once they are on. The ANE needs more than the PSD slots (clock-ids 329, 328 and 375 are not modeled).
 */
#ifndef __ANE_H18_H__
#define __ANE_H18_H__

#include <linux/types.h>

/* PSD slot window: 0x100 bytes at psd_base + (slot << 8). Offsets and states as in pmgr-psd.c. */
#define ANE_H18_PSD_BASE	0x303a10000ULL
#define ANE_H18_PSD_WIN_SIZE	0x100
#define ANE_H18_PSD_STATUS	0x80	/* bits 2:0 = current state; a read clears the acknowledge */
#define ANE_H18_PSD_REQUEST	0x90	/* write the requested state */
#define ANE_H18_PSD_DIAG	0xa0
#define ANE_H18_PSD_VALID	0xbc	/* bit 0 = status is valid */
#define ANE_H18_PSD_STATE_MASK	0x7u
#define ANE_H18_PSD_P0_RUN	0
#define ANE_H18_PSD_P6_RAIL	6
#define ANE_H18_NPSD		2

/* ASC wrapper (reg[0]) offsets. CPU_CONTROL (+0x44) is read only with read_ctrl=1 and is never written by this module. */
#define ANE_H18_ASC_CPU_CONTROL	0x0044
#define ANE_H18_ASC_CPU_STATUS	0x0048
#define ANE_H18_ASC_STATUS	0x8048
/* CPU_CONTROL bit 4 starts the core (macOS behaviour in the lab notebook; m1n1's R_CPU_CONTROL.RUN is the same bit).
 * CPU_STATUS: bit 0 RUNNING, bit 1 STOPPED (m1n1 R_CPU_STATUS). The board read 0xa before the core ever ran. */
#define ANE_H18_CPU_RUN		(1u << 4)
#define ANE_H18_ST_RUNNING	(1u << 0)
#define ANE_H18_ST_STOPPED	(1u << 1)
/* IORVBAR (reg[1]) is a 64-bit register at +0, read as two 32-bit words. */
#define ANE_H18_RVBAR_OFF	0x0
/* Postoffice (reg[2]): 0x80 bytes per channel, status word at +0x20 of the channel. */
#define ANE_H18_PO_STRIDE	0x80
#define ANE_H18_PO_STATUS	0x20
#define ANE_H18_PO_OUTBOX	0x00	/* 16 bytes, four 32-bit words; the last word read pops the message (m1n1 OUTBOX1) */
#define ANE_H18_PO_MAXMSG	8	/* at most this many messages are read per run */
#define ANE_H18_PO_NCAND	3	/* candidate channels, see ane_h18_stage_power() */
/* The alias bank: the same Postoffice window at +0x10000, channel 2 * msgbox-mailbox-num. For this ANE the ADT has msgbox-using-postoffice and no
 * msgbox-using-alias, which the shipped system turns into base + 0x10000 and channel 2 * num (0x316010c00 for ANE0). */
#define ANE_H18_PO_ALIAS_OFF	0x10000
/* The eight SCRATCH words of the engine block (shipped-system handshake registers): engine + 0x18a0024 to +0x18a0040 for ane-type 0x310 (h18g), the table
 * macOS uses for this SoC. Read only here, in the page at scratch_pa. */
#define ANE_H18_SCRATCH_OFF	0x24
#define ANE_H18_SCRATCH_N	8
/* SCRATCH handshake of macOS (macOS behaviour on build 26A434, mac18g; recorded in a private lab note):
 * after READY the host publishes the address of a 0x174 byte init block
 * in SCRATCH0 (low) and SCRATCH1 (high), writes the wake word to SCRATCH7, and the firmware answers READY again with the address of its channel table in
 * SCRATCH1:SCRATCH0. The firmware already shows READY before the wake (board read), so the answer is judged by the table address, not by SCRATCH7. */
#define ANE_H18_HS_WAKE		0xf7fbdff9u
#define ANE_H18_HS_READY	0x08042006u
#define ANE_H18_HS_REQ_LIMIT	0x21	/* SCRATCH0 at or above this: macOS does not go on */
#define ANE_H18_HS_BLOCK_SIZE	0x174
#define ANE_H18_HS_POOL_SIZE	0x40000	/* the pool surface the block is taken from, and the qword at block +0x60: the T6021 values (ane_t6021_boot.c, confirmed there) */
#define ANE_H18_HS_IPC_MAX	0x100000	/* refuse a firmware request above these before allocating */
#define ANE_H18_HS_HEAP_MAX	0x4000000
/* Channel step after the handshake (macOS behaviour): the table the firmware publishes has CH_COUNT entries of CH_ENTRY bytes (name[0x40], type u32 +0x40, bit u32
 * +0x44, size u64 +0x48 in 64 byte slots, ring u64 +0x50), the same layout as ane/t6021/ane_t6021_boot.h. Type 0 is host to firmware, 1 firmware to host, 2 the
 * terminal. The host sets every H2T slot header to 1 (host owned), writes READY to SCRATCH3 and waits for the firmware to clear it. A command is a 64 byte slot
 * on the IO ring: header = the DMA address of a command block (bit 0 clear hands it to the firmware), size words, doorbell BIT(bit) at engine + 0x18a4000. The
 * firmware answers by setting bit 0 of the header and a zero acknowledgement at +6 of the block. The command block has the u16 id at +4 and the payload from +8. */
#define ANE_H18_CH_COUNT	8
#define ANE_H18_CH_ENTRY	0x100
#define ANE_H18_CH_NAME_LEN	0x40
#define ANE_H18_CH_SLOT		0x40
#define ANE_H18_CH_MAX_SLOTS	0x400
#define ANE_H18_DOORBELL_OFF	0x18a4000
#define ANE_H18_IPI_PENDING_OFF	0x18ac000	/* IPI pending (engine + 0x18ac000) */
#define ANE_H18_IPI_ACK_OFF	0x18b0000	/* IPI ack: the value read from pending is written back (engine + 0x18b0000) */
/* SHAREDMALLOC ring entries (firmware behaviour, same shapes as ane/t6021/ane_t6021_mring.h): MALLOC = header 0, size, mode; FREE = header is the IOVA
 * the firmware gives back, size and mode 0; a header with bit 0 set was answered. The host answers a MALLOC with a buffer (size word 0, mode word = ordinal,
 * header = DMA | 1) and a FREE with header 1, then rings the ring's doorbell bit. The firmware waits for the answer with no timeout. */
#define ANE_H18_SNAP_PER_WAIT	6	/* at most this many change snapshots per wait: a firmware console that prints every millisecond must not flood the log */
#define ANE_H18_MAL_MAX_BUFS	16
#define ANE_H18_MAL_MAX_SIZE	0x200000
#define ANE_H18_MAL_MAX_BYTES	0x4000000
enum ane_h18_mal_kind { ANE_H18_MAL_IDLE, ANE_H18_MAL_MALLOC, ANE_H18_MAL_FREE, ANE_H18_MAL_ANSWERED, ANE_H18_MAL_BAD };

static inline enum ane_h18_mal_kind ane_h18_mal_classify(u64 hdr, u64 size, u64 mode)
{
	if (hdr & 1)
		return ANE_H18_MAL_ANSWERED;
	if (!hdr) {
		if (!size)
			return ANE_H18_MAL_IDLE;
		return mode > 0xffffffffULL ? ANE_H18_MAL_BAD : ANE_H18_MAL_MALLOC;
	}
	return (size || mode) ? ANE_H18_MAL_BAD : ANE_H18_MAL_FREE;
}
#define ANE_H18_CMD_BLOCK	0x100	/* the commands here are 12 and 16 bytes, the reply fits in the block too: clear no more of the pool than that */
#define ANE_H18_CMD_POOL_OFF	0x400	/* the command block lives in the pool surface, 0x100 aligned, as the shipped system takes every command block from the pool */
#define ANE_H18_CSNE_CH_PROPERTY_WRITE	0x1f	/* 0x14 bytes: u32 0 at +8, property id at +0xc, u32 value at +0x10 */
#define ANE_H18_CSNE_PRINT_ENABLE	0x04
#define ANE_H18_CSNE_MCACHE_SIZE_GET	0x26	/* 12 bytes, no argument; the reply u32 at +8 is the memory cache size */
#define ANE_H18_CSNE_DEFAULT_SETTING_SET	0x2e	/* 0x18 bytes: u64 1 at +8 (one setting), u32 2 at +0x10 (register id 2), u32 0x2710 at +0x14; the shipped system sends it after the property writes (value from config +0x124) */
#define ANE_H18_CSNE_SET_DYNAMIC_POWERGATE	0x2d	/* 12 bytes, one byte at +8: 1 enables, 0 disables the firmware's dynamic power gating (where macOS sends it is not known; that 1 enables is an inference) */
#define ANE_H18_CSNE_SET_SNE_PMU_BASE2	0x29	/* 16 bytes, u64 at +8: the PMU base the firmware uses for its own power control; macOS sends it after START and before CONFIG_GET */
#define ANE_H18_PMU_BASE_H18G	0x300700000ULL	/* the constant the shipped system uses for ANE types 0x300 and 0x310 */
#define ANE_H18_CSNE_CONFIG_GET		0x03
#define ANE_H18_CSNE_START		0x00
#define ANE_H18_CSNE_LOAD_PROGRAM	0x200	/* the T6021 wire: nine 0x30 byte section records at +8 (flags 1, id +4, iova +0x18, size +0x20), 0x1c0 bytes, the program id comes back at +0x1b8 */
#define ANE_H18_CSNE_DESTROY_PROGRAM	0x201	/* 0x0c bytes, program id at +8, needs zero processes */
#define ANE_H18_CSNE_CREATE_PROCESS	0x202	/* 0x10 bytes, program id at +8, the process id comes back at +0xc */
#define ANE_H18_CSNE_TERMINATE_PROCESS	0x203	/* 0x10 bytes, program id at +8, process id at +0xc */
#define ANE_H18_CSNE_PROCEDURE_CALL	0x204	/* 0x60 bytes plus one 0x30 byte element per buffer */
/* hs_prog: the program file (ane-prog-pack) is validated before any write. Limits keep a wrong file from asking for the machine's memory. */
#define ANE_H18_PROG_BLOCK	0x400	/* command block size with hs_prog: LOAD_PROGRAM is 0x1c0 bytes and a call with 8 buffers 0x1e0 */
#define ANE_H18_PROG_MAGIC	0x50454e41	/* 'ANEP' */
#define ANE_H18_PROG_VERSION	1
#define ANE_H18_PROG_MAX_SECS	9
#define ANE_H18_PROG_MAX_IO	8
#define ANE_H18_PROG_MAX_BUF	0x4000000	/* one buffer, 64 MiB */
#define ANE_H18_PROG_MAX_BYTES	0xc000000	/* all buffers together, 192 MiB */
#define ANE_H18_PROG_COOKIE	0xADD0	/* the call cookie of the T6021 driver, returned in the IO_T2H events */
#define ANE_H18_PROG_PRIORITY	2
#define ANE_H18_CSNE_PREMAP_BUFFER	0x20d	/* 0x3490 bytes, type 2 for the inference request pool: the shipped system sends it at init, before any call */
#define ANE_H18_PREMAP_LEN	0x3490
#define ANE_H18_PREMAP_POOL	0x3898	/* one inference request, rounded to a page for the surface */
#define ANE_H18_PREMAP_BLOCK	0x4000	/* command block size with hs_premap */
#define ANE_H18_DART_INSTANCES	4	/* ADT dart-ane reg[0..3], 0xc000 each, 0x20000 apart; the DAPF at 0x419810000 is not read */
#define ANE_H18_DART_INST_STRIDE	0x20000
#define ANE_H18_DART_EXC_WORDS	8	/* words of the DART exception bitmap read (256 streams) */
#define ANE_H18_DART_EXC_MAXSID	256	/* streams whose 0x40 byte record lies inside the DART window (0xc000) */
#define ANE_H18_DART_EXC_SIDS	8	/* at most this many stream records printed */
#define ANE_H18_CSNE_RESOURCE_INFO_GET	0x22	/* 100 byte command, macOS copies 16 bytes from +0x20 and +0x50 of the reply */
/* ANE DART (dart,gen3, instance 0): read only liveness words, see ane_h18_read_dart_words(). */
#define ANE_H18_DART_NREAD	12
/* Postoffice channel layout used for the RTKit exchange, as drivers/soc/apple/mailbox.c has it for the T8152 counted FIFO: RX (the outbox, firmware to host)
 * at +0x00, TX (the inbox) at +0x10, status at +0x20 with the RX occupancy in bits 3:0 and the free TX slots in bits 11:8. A message is 16 bytes and is
 * moved with one paired access (ldp from RX, stp to TX); four separate 32-bit accesses are not what that driver does on this SoC. */
#define ANE_H18_PO_RX		0x00
#define ANE_H18_PO_TX		0x10
#define ANE_H18_PO_MAXSEND	12	/* at most this many management messages sent per run */
/* RTKit management protocol (the Linux apple-rtkit dialect): message type in bits 59:52 of the first 64-bit word, endpoint in the low byte of the second. */
#define ANE_H18_RTK_TYPE_SHIFT	52
#define ANE_H18_RTK_HELLO	1
#define ANE_H18_RTK_HELLO_REPLY	2
#define ANE_H18_RTK_STARTEP	5
#define ANE_H18_RTK_IOP_PWR_ACK	7
#define ANE_H18_RTK_EPMAP	8	/* the reply has the same type */
#define ANE_H18_RTK_AP_PWR	0xb	/* SET_AP_PWR_STATE */
#define ANE_H18_RTK_PWR_ON	0x20
#define ANE_H18_RTK_PWR_OFF	0x00	/* rtkit_stop=2: OFF on both sides, cannot be restarted (apple_rtkit_poweroff) */
#define ANE_H18_RTK_PWR_QUIESCED	0x10	/* the AP state of apple_rtkit_shutdown */
#define ANE_H18_RTK_PWR_SLEEP	0x01	/* the IOP state of apple_rtkit_shutdown: sleeping, can be restarted (OFF cannot) */
#define ANE_H18_RTK_IOP_IDLE	0xc
#define ANE_H18_RTK_SET_IOP_PWR	6
#define ANE_H18_RTK_STOP_WAIT_MS	600	/* how long the shutdown handshake waits for the IOP acknowledgement */
#define ANE_H18_RTK_APP_EP	0x20	/* endpoints from here are application endpoints, started only once the coprocessor runs */
#define ANE_H18_RTK_VERSION	12	/* the version macOS and Linux answer with on this firmware family */

/* A firmware segment the boot loader left in DRAM (ADT /arm-io/ane segment-ranges: physical address, size, and the IOVA the firmware is linked at in the
 * ANE DART's address space). The DART maps it before the core runs. TEXT is mapped read only, DATA read and write. */
struct ane_h18_fw_seg {
	u64 pa;
	u64 size;
	u64 iova;
	bool write;
};

struct ane_h18_soc {
	const char *name;
	u32 ane_type;
	u64 asc_pa;
	u32 asc_size;
	u64 rvbar_pa;
	u32 rvbar_size;
	u64 mbox_pa;
	u32 mbox_size;
	u64 engine_pa;
	u32 engine_size;
	u64 scratch_pa;		/* the page of the engine block that holds SCRATCH0 to 7 */
	u32 scratch_size;
	u64 dart_pa;		/* ANE DART instance 0 (ADT /arm-io/dart-ane reg[0]) */
	u32 dart_size;
	u32 mbox_num;		/* ADT msgbox-mailbox-num */
	u64 rvbar_expect;	/* IORVBAR as the boot loader leaves it: entry 0x10000000000 (the segment-ranges remap) | valid bit. Measured on the board. */
	u32 psd_slot[ANE_H18_NPSD];
	struct ane_h18_fw_seg fw_seg[2];
};

extern const struct ane_h18_soc ane_t8152_soc;

#endif /* __ANE_H18_H__ */
