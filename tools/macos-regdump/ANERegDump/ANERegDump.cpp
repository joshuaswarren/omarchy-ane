/* ANERegDump.kext: a read-only snapshot of the T6021 ANE working state,
 * plus the iBoot handoff tables the macOS 27.0 kernel entry copied into
 * its globals. No device register is ever written.
 *
 * Engine-window reads (wrapper, mailbox, RVBAR, scratch) happen only
 * after all eight ANE pmgr islands read ACTUAL = 0xf. A read with an
 * island down wedges the fabric. The wrapper IRQ event queue and the
 * mailbox FIFOs pop on read, so those words are skipped and filled with
 * 0xdead in the dump. CoreSight is never mapped.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/libkern.h>

extern "C" {
#include "ane_regdump_filter.h"
extern vm_offset_t vm_kernel_slide;   /* not a declared KPI; resolved */
}

#define ANE_DUMP_MAGIC   0x414e4531u   /* 'ANE1' */
#define ANE_DUMP_VERSION 1u
#define ANE_RANGE_MAX    40u
#define ANE_NAME_MAX     24u
#define ANE_DUMP_MAX     (8u * 1024u * 1024u)

#define ANE_GLOBALS_STATIC 0xfffffe0007f48000ull
#define ANE_GLOBALS_LO     0x6c8u
#define ANE_GLOBALS_HI     0x7d0u
#define ANE_TEXT_VMADDR    0xfffffe0007004000ull
#define ANE_FOLLOW_BYTES   (64u * 1024u)

enum {
	ANE_DUMP_GET = 0,
	ANE_DUMP_COUNT = 1
};

enum {
	ANE_ST_OK = 0,
	ANE_ST_GATED = 1,    /* pmgr islands down, range not read */
	ANE_ST_NOMAP = 2,    /* mapping failed */
	ANE_ST_ABSENT = 3    /* source not present */
};

enum {
	ANE_F_GATED = 1,     /* read only while the islands are up */
	ANE_F_PROP = 2,      /* bytes copied from an ADT property */
	ANE_F_VIRT = 4       /* kernel virtual source */
};

/* Pointers the 27.0 entry actually dereferences. Everything else in the
 * window is a count or an address-range scalar and must not be followed.
 * From the entry decode, kernelcache 26A428 sha256 8304156f. */
static const uint32_t ane_handoff_ptr[] = {
	0x6d0, 0x6d8, 0x708, 0x710, 0x718, 0x730,
	0x748, 0x758, 0x768, 0x7a0, 0x7c8
};

struct ane_range_rec {
	char     name[ANE_NAME_MAX];
	uint64_t pa;
	uint64_t src;        /* virtual address, or 0 */
	uint32_t len;
	uint32_t off;        /* offset of the bytes in the data section */
	uint32_t status;
	uint32_t banned;     /* words replaced with 0xdead */
	uint32_t flags;
	uint32_t pad;
};

struct ane_dump_hdr {
	uint32_t magic;
	uint32_t version;
	uint32_t hdr_bytes;
	uint32_t nranges;
	uint32_t data_bytes;
	uint32_t islands_up;
	uint32_t ps[ANE_ISLAND_COUNT];
	uint64_t kaslr_slide;
	uint64_t globals_va;
	struct ane_range_rec range[ANE_RANGE_MAX];
};

class ANERegDumpUserClient;

class ANERegDump : public IOService
{
	OSDeclareDefaultStructors(ANERegDump)
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID,
	    UInt32 type, OSDictionary *properties,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
	IOReturn buildDump(IOMemoryDescriptor *out);
	struct map {
		char name[ANE_NAME_MAX];
		uint64_t pa;
		uint64_t src;
		uint32_t len;
		uint32_t flags;
	};
private:
	IOMemoryMap *mapPhys(uint64_t pa, uint32_t len);
	bool readIslands(uint32_t ps[ANE_ISLAND_COUNT]);
	uint32_t copyPhys(const struct map *m, uint8_t *dst, uint32_t *banned);
	uint32_t copyVirt(uint64_t va, uint8_t *dst, uint32_t len);
	uint32_t addFixed(struct map *m, uint32_t n);
	uint32_t addAdt(struct map *m, uint32_t n,
	    const uint8_t **prop_bytes, uint32_t *prop_len, uint32_t prop_cap,
	    IORegistryEntry **held, uint32_t *nheld);
	uint32_t addHandoff(struct map *m, uint32_t n, uint64_t slide);
	uint32_t ps_cache[ANE_ISLAND_COUNT];
};

class ANERegDumpUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(ANERegDumpUserClient)
public:
	virtual bool initWithTask(task_t owningTask, void *securityID,
	    UInt32 type, OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector,
	    IOExternalMethodArguments *args, IOExternalMethodDispatch *dp,
	    OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	static IOReturn dumpGet(ANERegDumpUserClient *target, void *ref,
	    IOExternalMethodArguments *args);
	ANERegDump *owner;
};

OSDefineMetaClassAndStructors(ANERegDump, IOService)
OSDefineMetaClassAndStructors(ANERegDumpUserClient, IOUserClient)

bool ANERegDump::start(IOService *provider)
{
	if (!IOService::start(provider))
		return false;
	registerService();
	return true;
}

void ANERegDump::stop(IOService *provider)
{
	IOService::stop(provider);
}

IOReturn ANERegDump::newUserClient(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
	ANERegDumpUserClient *c = new ANERegDumpUserClient;

	if (!c)
		return kIOReturnNoMemory;
	if (!c->initWithTask(owningTask, securityID, type, properties) ||
	    !c->attach(this) || !c->start(this)) {
		c->detach(this);
		c->release();
		return kIOReturnError;
	}
	c->owner = this;
	*handler = c;
	return kIOReturnSuccess;
}

/* Read-only, cache-inhibited. Read-only matters: PPL-owned MMIO mapped
 * writable is handed to the PPL and the kernel can no longer read it. */
IOMemoryMap *ANERegDump::mapPhys(uint64_t pa, uint32_t len)
{
	IOMemoryDescriptor *md;
	IOMemoryMap *mm;

	md = IOMemoryDescriptor::withPhysicalAddress(pa, len, kIODirectionNone);
	if (!md)
		return NULL;
	mm = md->map(kIOMapReadOnly | kIOMapInhibitCache);
	md->release();
	return mm;
}

/* The pmgr block is always on, so this read is safe in any island state. */
bool ANERegDump::readIslands(uint32_t ps[ANE_ISLAND_COUNT])
{
	IOMemoryMap *mm = mapPhys(ANE_PMGR_PHYS, ANE_PMGR_LEN);
	volatile uint32_t *w;
	size_t i;

	if (!mm)
		return false;
	w = (volatile uint32_t *)mm->getVirtualAddress();
	for (i = 0; i < ANE_ISLAND_COUNT; i++)
		ps[i] = w[ane_island_off[i] / 4];
	mm->release();
	return true;
}

/* Copy one physical range. Banned engine-window words become 0xdead. */
uint32_t ANERegDump::copyPhys(const struct map *m, uint8_t *dst,
    uint32_t *banned)
{
	IOMemoryMap *mm;
	volatile uint32_t *w;
	uint32_t i, n;

	*banned = 0;
	if ((m->flags & ANE_F_GATED) && !ane_islands_up(ps_cache))
		return ANE_ST_GATED;
	mm = mapPhys(m->pa, m->len);
	if (!mm)
		return ANE_ST_NOMAP;
	w = (volatile uint32_t *)mm->getVirtualAddress();
	n = m->len / 4;
	for (i = 0; i < n; i++) {
		uint64_t off = (m->pa - ANE_ENGINE_PHYS) + (uint64_t)i * 4;

		if (m->pa >= ANE_ENGINE_PHYS &&
		    m->pa < ANE_ENGINE_PHYS + ANE_ENGINE_LEN &&
		    ane_word_banned(off, 4)) {
			((uint32_t *)dst)[i] = 0xdead;
			(*banned)++;
			continue;
		}
		((uint32_t *)dst)[i] = w[i];
	}
	mm->release();
	return ANE_ST_OK;
}

/* Copy kernel virtual memory one page at a time. An unmapped page ends
 * the copy; the caller keeps what was read. */
uint32_t ANERegDump::copyVirt(uint64_t va, uint8_t *dst, uint32_t len)
{
	uint32_t done = 0;

	while (done < len) {
		uint64_t page = (va + done) & ~0x3fffull;
		uint32_t off = (uint32_t)((va + done) - page);
		uint32_t chunk = 0x4000 - off;
		IOMemoryDescriptor *md;
		IOMemoryMap *mm;

		if (chunk > len - done)
			chunk = len - done;
		md = IOMemoryDescriptor::withAddressRange(page, 0x4000,
		    kIODirectionNone, kernel_task);
		if (!md)
			break;
		mm = md->map(kIOMapReadOnly);
		md->release();
		if (!mm)
			break;
		memcpy(dst + done,
		    (void *)(mm->getVirtualAddress() + off), chunk);
		mm->release();
		done += chunk;
	}
	return done;
}

static void setname(ANERegDump::map *m, const char *s)
{
	strlcpy(m->name, s, ANE_NAME_MAX);
}

uint32_t ANERegDump::addFixed(struct map *m, uint32_t n)
{
	struct spec { const char *name; uint64_t pa; uint32_t len; int gated; };
	static const struct spec s[] = {
		{ "pmgr-ps",     ANE_PMGR_PHYS,              0x4040, 0 },
		{ "wrapper",     ANE_ENGINE_PHYS + 0x1400000, 0x14000, 1 },
		{ "mailbox",     ANE_ENGINE_PHYS + 0x1408000, 0x1000,  1 },
		{ "rvbar",       ANE_ENGINE_PHYS + 0x1050000, 0x1000,  1 },
		{ "scratch",     ANE_ENGINE_PHYS + 0x1840000, 0x1000,  1 },
		{ "dart0",       ANE_DART0_PHYS,              0x2000,  1 },
		{ "dart1",       ANE_DART0_PHYS + ANE_DART_STRIDE, 0x2000, 1 },
		{ "dart2",       ANE_DART0_PHYS + 2 * ANE_DART_STRIDE, 0x2000, 1 },
		{ "patchbay",    0x10001406870ull,            0x40,    0 },
	};
	size_t i;

	for (i = 0; i < sizeof(s) / sizeof(s[0]) && n < ANE_RANGE_MAX; i++) {
		memset(&m[n], 0, sizeof(m[n]));
		setname(&m[n], s[i].name);
		m[n].pa = s[i].pa;
		m[n].len = s[i].len;
		m[n].flags = s[i].gated ? ANE_F_GATED : 0;
		n++;
	}
	return n;
}

/* Firmware ranges come from the live ADT: the macOS 27 firmware layout
 * is not the 13.5 one. The registry entries stay held so their property
 * bytes remain valid until the caller copies them. */
uint32_t ANERegDump::addAdt(struct map *m, uint32_t n,
    const uint8_t **prop_bytes, uint32_t *prop_len, uint32_t prop_cap,
    IORegistryEntry **held, uint32_t *nheld)
{
	IORegistryEntry *arm, *ane, *chosen;
	OSData *seg, *reg, *bootargs;
	const uint8_t *b;
	unsigned int i, len;

	arm = IORegistryEntry::fromPath("IODeviceTree:/arm-io", gIODTPlane);
	ane = arm ? arm->childFromPath("ane0", gIODTPlane) : NULL;
	seg = ane ? OSDynamicCast(OSData, ane->getProperty("segment-ranges"))
		  : NULL;
	if (seg) {
		b = (const uint8_t *)seg->getBytesNoCopy();
		len = seg->getLength();
		/* {phys u64, iova u64, remap u64, size u32, flags u32}. */
		for (i = 0; i + 32 <= len && i < 64 && n < ANE_RANGE_MAX;
		     i += 32) {
			uint64_t phys, size;

			memcpy(&phys, b + i, 8);
			memcpy(&size, b + i + 24, 4);
			if (!phys || !size || size > 0x1000000)
				continue;
			memset(&m[n], 0, sizeof(m[n]));
			setname(&m[n], i ? "fw-data" : "fw-text");
			m[n].pa = phys;
			m[n].len = (uint32_t)size;
			n++;
		}
		if (prop_len[0] + len <= prop_cap && n < ANE_RANGE_MAX) {
			memset(&m[n], 0, sizeof(m[n]));
			setname(&m[n], "adt-segment-ranges");
			m[n].len = len;
			m[n].flags = ANE_F_PROP;
			prop_bytes[n] = b;
			prop_len[n] = len;
			n++;
		}
	}
	reg = ane ? OSDynamicCast(OSData, ane->getProperty("reg")) : NULL;
	if (reg && n < ANE_RANGE_MAX) {
		memset(&m[n], 0, sizeof(m[n]));
		setname(&m[n], "adt-ane-reg");
		m[n].len = reg->getLength();
		m[n].flags = ANE_F_PROP;
		prop_bytes[n] = (const uint8_t *)reg->getBytesNoCopy();
		prop_len[n] = reg->getLength();
		n++;
	}
	if (arm)
		held[(*nheld)++] = arm;

	chosen = IORegistryEntry::fromPath("IODeviceTree:/chosen", gIODTPlane);
	bootargs = chosen ? OSDynamicCast(OSData,
	    chosen->getProperty("boot-args")) : NULL;
	if (bootargs && n < ANE_RANGE_MAX) {
		memset(&m[n], 0, sizeof(m[n]));
		setname(&m[n], "adt-boot-args");
		m[n].len = bootargs->getLength();
		m[n].flags = ANE_F_PROP;
		prop_bytes[n] = (const uint8_t *)bootargs->getBytesNoCopy();
		prop_len[n] = bootargs->getLength();
		n++;
	}
	if (chosen)
		held[(*nheld)++] = chosen;
	return n;
}
/* Globals at the static address plus the KASLR slide, then each decoded
 * pointer followed for at most 64 KB. */
uint32_t ANERegDump::addHandoff(struct map *m, uint32_t n, uint64_t slide)
{
	uint64_t base = ANE_GLOBALS_STATIC + slide;
	uint8_t win[ANE_GLOBALS_HI - ANE_GLOBALS_LO];
	uint32_t got, i;

	if (n >= ANE_RANGE_MAX)
		return n;
	memset(&m[n], 0, sizeof(m[n]));
	setname(&m[n], "handoff-globals");
	m[n].src = base + ANE_GLOBALS_LO;
	m[n].len = ANE_GLOBALS_HI - ANE_GLOBALS_LO;
	m[n].flags = ANE_F_VIRT;
	n++;

	got = copyVirt(base + ANE_GLOBALS_LO, win, sizeof(win));
	if (got < sizeof(win))
		return n;
	for (i = 0; i < sizeof(ane_handoff_ptr) / sizeof(ane_handoff_ptr[0]) &&
	     n < ANE_RANGE_MAX; i++) {
		uint32_t o = ane_handoff_ptr[i] - ANE_GLOBALS_LO;
		uint64_t ptr;

		memcpy(&ptr, win + o, 8);
		if (!ptr)
			continue;
		memset(&m[n], 0, sizeof(m[n]));
		snprintf(m[n].name, ANE_NAME_MAX, "handoff-%03x",
		    ane_handoff_ptr[i]);
		m[n].src = ptr;
		m[n].len = ANE_FOLLOW_BYTES;
		m[n].flags = ANE_F_VIRT;
		n++;
	}
	return n;
}

IOReturn ANERegDump::buildDump(IOMemoryDescriptor *out)
{
	struct map table[ANE_RANGE_MAX];
	struct ane_dump_hdr hdr;
	uint8_t *buf;
	const uint8_t *prop_src[ANE_RANGE_MAX] = {};
	uint32_t prop_len[ANE_RANGE_MAX] = {};
	IORegistryEntry *held[4] = {};
	uint32_t nheld = 0;
	uint32_t n = 0, data = 0, i;
	uint64_t slide;
	IOReturn rc;

	memset(table, 0, sizeof(table));
	memset(&hdr, 0, sizeof(hdr));
	if (!readIslands(ps_cache))
		return kIOReturnNotReady;
	memcpy(hdr.ps, ps_cache, sizeof(hdr.ps));
	hdr.islands_up = ane_islands_up(ps_cache);

	n = addFixed(table, n);
	slide = vm_kernel_slide;
	n = addAdt(table, n, (const uint8_t **)prop_src, prop_len,
	    ANE_DUMP_MAX, held, &nheld);
	n = addHandoff(table, n, slide);

	buf = (uint8_t *)IOMalloc(ANE_DUMP_MAX);
	if (!buf) {
		for (i = 0; i < nheld; i++)
			held[i]->release();
		return kIOReturnNoMemory;
	}
	memset(buf, 0, ANE_DUMP_MAX);

	for (i = 0; i < n; i++) {
		struct ane_range_rec *r = &hdr.range[i];
		uint32_t banned = 0, got = 0;

		strlcpy(r->name, table[i].name, ANE_NAME_MAX);
		r->pa = table[i].pa;
		r->src = table[i].src;
		r->flags = table[i].flags;
		r->off = data;
		if (data + table[i].len > ANE_DUMP_MAX) {
			r->status = ANE_ST_NOMAP;
			continue;
		}
		if (table[i].flags & ANE_F_VIRT) {
			got = copyVirt(table[i].src, buf + data, table[i].len);
			r->status = got ? ANE_ST_OK : ANE_ST_NOMAP;
		} else if (table[i].flags & ANE_F_PROP) {
			if (prop_src[i] && prop_len[i] == table[i].len) {
				memcpy(buf + data, prop_src[i], prop_len[i]);
				r->status = ANE_ST_OK;
				r->len = prop_len[i];
			} else {
				r->status = ANE_ST_ABSENT;
				r->len = 0;
			}
		} else {
			r->status = copyPhys(&table[i], buf + data, &banned);
			r->banned = banned;
			r->len = (r->status == ANE_ST_OK) ? table[i].len : 0;
		}
		data += r->len;
	}
	hdr.magic = ANE_DUMP_MAGIC;
	hdr.version = ANE_DUMP_VERSION;
	hdr.hdr_bytes = sizeof(hdr);
	hdr.nranges = n;
	hdr.data_bytes = data;

	if (out->getLength() < sizeof(hdr) + data)
		rc = kIOReturnNoSpace;
	else if ((rc = out->prepare(kIODirectionIn)) == kIOReturnSuccess) {
		out->writeBytes(0, &hdr, sizeof(hdr));
		out->writeBytes(sizeof(hdr), buf, data);
		out->complete(kIODirectionIn);
	}
	for (i = 0; i < nheld; i++)
		held[i]->release();
	IOFree(buf, ANE_DUMP_MAX);
	return rc;
}

bool ANERegDumpUserClient::initWithTask(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties)
{
	return IOUserClient::initWithTask(owningTask, securityID, type,
	    properties);
}

IOReturn ANERegDumpUserClient::clientClose(void)
{
	if (owner)
		detach(owner);
	terminate();
	return kIOReturnSuccess;
}

IOReturn ANERegDumpUserClient::dumpGet(ANERegDumpUserClient *target, void *ref,
    IOExternalMethodArguments *args)
{
	(void)ref;
	if (!args->structureOutputDescriptor)
		return kIOReturnBadArgument;
	return target->owner->buildDump(args->structureOutputDescriptor);
}

IOReturn ANERegDumpUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *args, IOExternalMethodDispatch *dp,
    OSObject *target, void *reference)
{
	static const IOExternalMethodDispatch disp[] = {
		{ (IOExternalMethodAction)&ANERegDumpUserClient::dumpGet,
		  0, 0, 0, kIOUCVariableStructureSize }
	};

	(void)dp;
	(void)target;
	(void)reference;
	if (selector >= ANE_DUMP_COUNT)
		return kIOReturnBadArgument;
	return IOUserClient::externalMethod(selector, args,
	    (IOExternalMethodDispatch *)&disp[selector], this, NULL);
}
