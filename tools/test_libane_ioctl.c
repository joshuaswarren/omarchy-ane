// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ane_accel.h>
#include "ane.h"
#include "ane_m2.h"
#include "ane_sha256.h"

/*
// HOST-ONLY ioctl argument check: no device, no kernel, no hardware.
//
// libane is compiled into this program with -ftrivial-auto-var-init=pattern
// (tools/Makefile), so a stack member that libane does not set holds 0xfe
// bytes, every run. The link wraps open, ioctl and mmap: open of
// /dev/accel/accel0 gives a fake DRM node (any other accel node is refused,
// so no real device is opened), and the fake answers DRM_IOCTL_VERSION and
// every ANE ioctl. Like the in-tree drivers, it refuses a nonzero pad,
// flags or reserved member with -EINVAL, and it checks that every BO handle
// is freed exactly once. The program file is a real H14 fixture. Nothing
// executes it: the fake checks only the argument structs.
//
// usage: test_libane_ioctl [fixtures-dir]   (default ../fixtures/h14-anec)
*/

#define FAKE_NODE "/dev/accel/accel0"
#define MAX_BOS 1024

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long request, ...);
void *__real_mmap(void *addr, size_t len, int prot, int flags, int fd,
		  off_t off);

static int abi_major;
static int fake_fd = -1;
/* Every device open registers its fd: a session (and this test) holds
 * several nns open at once, and an ioctl must be answered for each of
 * them, not only the latest. */
#define MAX_FAKE_FDS 64
static int fake_fds[MAX_FAKE_FDS];
static unsigned n_fake_fds;
static int mmap_countdown; /* nonzero: the Nth mmap of the fake node fails */
static int failures;
static uint32_t next_handle;
static uint8_t live[MAX_BOS + 1];

static int fake_fd_p(int fd)
{
	unsigned i;

	for (i = 0; i < n_fake_fds; i++) {
		if (fake_fds[i] == fd) {
			return 1;
		}
	}
	return 0;
}

/* Driver-lifetime emulation for the section-release case: page-aligned
 * byte accounting (bo_total_bytes), a firmware hold (a BO whose IOVA rode
 * a sent LOAD_PROGRAM keeps its bytes counted until reboot, never freed),
 * and dedup of byte-identical PROG_LOADs by a digest over every section's
 * id, size and bytes. A dedup hit returns the cached program id and never
 * marks its BOs fw_ref -- exactly ane_t6021_rtclient_main.c. */
static uint64_t held_bytes;
static uint64_t bo_bytes[MAX_BOS + 1];
static uint8_t fw_ref[MAX_BOS + 1];
static uint8_t is_sec[MAX_BOS + 1]; /* rode a PROG_LOAD section list */
static unsigned live_sec_bos;
static uint8_t *bo_map[MAX_BOS + 1]; /* fake mmap base per handle */
static uint64_t bo_map_len[MAX_BOS + 1];

/* Every BO_INIT size in order: the section-allocation order test reads
 * the first six of an open. */
#define MAX_INIT_SIZES 4096
static uint64_t init_sizes[MAX_INIT_SIZES];
static unsigned n_init_sizes;

static struct {
	uint8_t digest[ANE_SHA256_LEN];
	uint32_t prog_id;
} fake_progs[64];
static unsigned fake_nprogs;
static uint32_t next_prog_id;

/* PROG_LOOKUP modelling. The registry key must be the SAME digest
 * libane presents with PROG_LOOKUP, so the fake hashes the section
 * bytes it reads from the BO maps with the same SHA-256 construction
 * libane uses (u64 id, u64 size, bytes, sections in order). */
static int fake_lookup_enotty;	/* kernel without PROG_LOOKUP */
static int fake_quarantine;	/* quarantined firmware refuses */
static int fail_next_prog_load;	/* a failed load publishes nothing */

static uint64_t page_align64(uint64_t v)
{
	return (v + 0x3fffull) & ~0x3fffull;
}

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

static int refuse(const char *what, uint64_t value)
{
	printf("FAIL %s = %#llx (the in-tree driver returns -EINVAL)\n", what,
	       (unsigned long long)value);
	failures++;
	errno = EINVAL;
	return -1;
}

int __wrap_open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	va_list ap;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	if (!strcmp(path, FAKE_NODE)) {
		fake_fd = __real_open("/dev/null", O_RDWR | O_CLOEXEC);
		if (fake_fd >= 0 && n_fake_fds < MAX_FAKE_FDS) {
			fake_fds[n_fake_fds++] = fake_fd;
		}
		return fake_fd;
	}
	if (!strncmp(path, "/dev/accel/", strlen("/dev/accel/"))) {
		errno = ENOENT;
		return -1;
	}
	return __real_open(path, flags, mode);
}

void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd,
		  off_t off)
{
	if (!fake_fd_p(fd)) {
		return __real_mmap(addr, len, prot, flags, fd, off);
	}
	if (mmap_countdown && --mmap_countdown == 0) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	/* The fake hands out offsets of handle<<32; remember each base so
	 * a PROG_LOAD can read the bytes it must hash. */
	{
		void *base = __real_mmap(NULL, len, PROT_READ | PROT_WRITE,
					 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (base != MAP_FAILED && (off >> 32) <= MAX_BOS) {
			bo_map[off >> 32] = base;
			bo_map_len[off >> 32] = len;
		}
		return base;
	}
}

static int fake_version(drm_version_t *v)
{
	if (v->name && v->name_len >= 3) {
		memcpy(v->name, "ane", 3);
	}
	v->name_len = 3;
	v->date_len = 0;
	v->desc_len = 0;
	v->version_major = abi_major;
	v->version_minor = 0;
	v->version_patchlevel = 0;
	return 0;
}

static int fake_bo_init(struct drm_ane_bo_init *a)
{
	if (a->pad) {
		return refuse("drm_ane_bo_init.pad", a->pad);
	}
	if (!a->size || next_handle == MAX_BOS) {
		return refuse("drm_ane_bo_init.size", a->size);
	}
	a->handle = ++next_handle;
	a->offset = (uint64_t)a->handle << 32;
	live[a->handle] = 1;
	bo_bytes[a->handle] = page_align64(a->size);
	held_bytes += bo_bytes[a->handle];
	if (n_init_sizes < MAX_INIT_SIZES) {
		init_sizes[n_init_sizes++] = a->size;
	}
	return 0;
}

static int fake_bo_free(const struct drm_ane_bo_free *a)
{
	if (a->pad) {
		return refuse("drm_ane_bo_free.pad", a->pad);
	}
	if (!a->handle || a->handle > next_handle || !live[a->handle]) {
		return refuse("drm_ane_bo_free.handle (not live)", a->handle);
	}
	live[a->handle] = 0;
	bo_map[a->handle] = NULL;
	bo_map_len[a->handle] = 0;
	if (is_sec[a->handle]) {
		is_sec[a->handle] = 0;
		live_sec_bos--;
	}
	if (fw_ref[a->handle]) {
		/* The firmware received this IOVA: like
		 * ane_t6021_bo_release, the bytes stay held until reboot. */
		fw_ref[a->handle] = 0;
		return 0;
	}
	held_bytes -= bo_bytes[a->handle];
	bo_bytes[a->handle] = 0;
	return 0;
}

static int fake_submit(const struct drm_ane_submit *a)
{
	if (a->pad) {
		return refuse("drm_ane_submit.pad", a->pad);
	}
	return 0;
}

static int fake_prog_load(struct drm_ane_prog_load *a)
{
	const struct drm_ane_generic_bind *b =
		(const void *)(uintptr_t)a->generic_ptr;
	const struct drm_ane_section *s =
		(const void *)(uintptr_t)a->sections_ptr;
	struct ane_sha256_ctx sha;
	uint8_t digest[ANE_SHA256_LEN];
	uint32_t i;
	unsigned j;

	if (a->pad) {
		return refuse("drm_ane_prog_load.pad", a->pad);
	}
	if (a->generic_count > ANE_M2_MAX_BINDS) {
		return refuse("drm_ane_prog_load.generic_count",
			      a->generic_count);
	}
	for (i = 0; i < a->generic_count; i++) {
		if (b[i].pad) {
			return refuse("drm_ane_generic_bind.pad", b[i].pad);
		}
	}
	if (fake_quarantine) {
		/* A quarantined firmware refuses every load-time
		 * command; a failed load publishes nothing. */
		errno = ETIMEDOUT;
		return -1;
	}
	/* The driver's key: SHA-256 over every section's (u64 id, u64
	 * size, bytes), sections in order -- the same construction
	 * libane presents with PROG_LOOKUP. */
	ane_sha256_init(&sha);
	for (i = 0; i < a->section_count; i++) {
		uint64_t hdr[2] = { s[i].id, s[i].size };

		if (!s[i].bo_handle || s[i].bo_handle > next_handle) {
			return refuse("drm_ane_section.bo_handle",
				      s[i].bo_handle);
		}
		ane_sha256_update(&sha, hdr, sizeof(hdr));
		if (bo_map[s[i].bo_handle] &&
		    s[i].offset + s[i].size <= bo_map_len[s[i].bo_handle]) {
			ane_sha256_update(&sha,
					  bo_map[s[i].bo_handle] +
						  s[i].offset,
					  s[i].size);
		}
		/* is_sec marks section-ness for every PROG_LOAD (test
		 * instrumentation); fw_ref below stays miss-only, like the
		 * driver. */
		if (!is_sec[s[i].bo_handle]) {
			is_sec[s[i].bo_handle] = 1;
			live_sec_bos++;
		}
	}
	ane_sha256_final(&sha, digest);
	if (fail_next_prog_load) {
		/* A failed load publishes NO reusable entry. */
		fail_next_prog_load = 0;
		errno = ETIMEDOUT;
		return -1;
	}
	for (j = 0; j < fake_nprogs; j++) {
		if (!memcmp(fake_progs[j].digest, digest,
			    ANE_SHA256_LEN)) {
			a->prog_id_out = fake_progs[j].prog_id;
			printf("  [fake LOAD hit j=%u key=%02x%02x%02x]\n", j,
			       digest[0], digest[1], digest[2]);
			return 0;
		}
	}
	if (fake_nprogs == sizeof(fake_progs) / sizeof(*fake_progs)) {
		return refuse("firmware program table", fake_nprogs);
	}
	memcpy(fake_progs[fake_nprogs].digest, digest, ANE_SHA256_LEN);
	fake_progs[fake_nprogs].prog_id = ++next_prog_id;
	fake_nprogs++;
	for (i = 0; i < a->section_count; i++) {
		fw_ref[s[i].bo_handle] = 1;
		if (!is_sec[s[i].bo_handle]) {
			is_sec[s[i].bo_handle] = 1;
			live_sec_bos++;
		}
	}
	a->prog_id_out = next_prog_id;
	return 0;
}

static int fake_prog_lookup(struct drm_ane_prog_lookup *a)
{
	unsigned j;

	if (a->pad || a->digest_len != ANE_SHA256_LEN) {
		return refuse("drm_ane_prog_lookup.digest_len", a->digest_len);
	}
	if (fake_quarantine) {
		/* Quarantine invalidates eligibility before any lookup. */
		errno = ENODEV;
		return -1;
	}
	if (fake_lookup_enotty) {
		/* An older kernel: the ioctl does not exist. */
		errno = ENOTTY;
		return -1;
	}
	for (j = 0; j < fake_nprogs; j++) {
		if (!memcmp(fake_progs[j].digest,
			    (const void *)(uintptr_t)a->digest_ptr,
			    ANE_SHA256_LEN)) {
			a->found_out = 1;
			a->prog_id_out = fake_progs[j].prog_id;
			printf("  [fake LOOKUP hit j=%u key=%02x%02x%02x]\n",
			       j, fake_progs[j].digest[0],
			       fake_progs[j].digest[1],
			       fake_progs[j].digest[2]);
			return 0;
		}
	}
	a->found_out = 0;
	a->prog_id_out = 0;
	printf("  [fake LOOKUP miss key=%02x%02x%02x nprogs=%u]\n",
	       ((const uint8_t *)(uintptr_t)a->digest_ptr)[0],
	       ((const uint8_t *)(uintptr_t)a->digest_ptr)[1],
	       ((const uint8_t *)(uintptr_t)a->digest_ptr)[2], fake_nprogs);
	return 0;
}

static int fake_exec(const struct drm_ane_exec *a)
{
	const struct drm_ane_exec_io *io = (const void *)(uintptr_t)a->io_ptr;
	uint32_t i;

	if (a->pad) {
		return refuse("drm_ane_exec.pad", a->pad);
	}
	if (!a->count || a->count > ANE_M2_MAX_BINDS) {
		return refuse("drm_ane_exec.count", a->count);
	}
	for (i = 0; i < a->count; i++) {
		if (io[i].flags) {
			return refuse("drm_ane_exec_io.flags", io[i].flags);
		}
		/* dma is the in-tree header's reserved member. */
		if (io[i].dma) {
			return refuse("drm_ane_exec_io.dma", io[i].dma);
		}
	}
	return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (!fake_fd_p(fd)) {
		return __real_ioctl(fd, request, arg);
	}
	switch (request) {
	case DRM_IOCTL_VERSION:
		return fake_version(arg);
	case DRM_IOCTL_ANE_BO_INIT:
		return fake_bo_init(arg);
	case DRM_IOCTL_ANE_BO_FREE:
		return fake_bo_free(arg);
	case DRM_IOCTL_ANE_SUBMIT:
		return abi_major == ANE_ABI_MAJOR ? fake_submit(arg) :
			refuse("SUBMIT on ABI 2", request);
	case DRM_IOCTL_ANE_PROG_LOAD:
		return fake_prog_load(arg);
	case DRM_IOCTL_ANE_PROC_CREATE:
		((struct drm_ane_proc_create *)arg)->proc_id_out = 1;
		return 0;
	case DRM_IOCTL_ANE_EXEC:
		return abi_major == ANE_ABI_M2_MAJOR ? fake_exec(arg) :
			refuse("EXEC on ABI 1", request);
	case DRM_IOCTL_ANE_PROG_LOOKUP:
		return abi_major == ANE_ABI_M2_MAJOR ?
			fake_prog_lookup(arg) :
			refuse("PROG_LOOKUP on ABI 1", request);
	}
	return refuse("unknown ioctl", request);
}

/* mmap_fail: 0 runs init, exec and free; N makes the Nth BO mmap fail, so
 * init must fail and free every BO it created. */
static void run_case(const char *name, int abi, const char *path,
		     int mmap_fail)
{
	int before = failures;
	struct ane_nn *nn;
	uint32_t h;

	abi_major = abi;
	mmap_countdown = mmap_fail;
	nn = ane_init(path);
	if (mmap_fail && nn) {
		printf("FAIL %s: init passed with a failed mmap\n", name);
		failures++;
		ane_free(nn);
	} else if (!mmap_fail && !nn) {
		printf("FAIL %s: init failed\n", name);
		failures++;
	} else if (nn) {
		if (ane_exec(nn)) {
			printf("FAIL %s: exec failed\n", name);
			failures++;
		}
		ane_free(nn);
	}
	if (mmap_countdown) {
		printf("FAIL %s: mmap %d not reached\n", name, mmap_fail);
		failures++;
	}
	for (h = 1; h <= next_handle; h++) {
		if (live[h]) {
			printf("FAIL %s: BO handle %u not freed\n", name, h);
			failures++;
			live[h] = 0;
		}
	}
	printf("  [%s] %s\n", failures == before ? "ok" : "FAIL", name);
}

/* A/B: load the same program through the staged fallback (ANE_LOAD_STAGED=1)
 * and the direct path (default), and require byte-identical buffer object
 * contents -- chans[] and the bootstrap channel -- plus the documented
 * nn->data meaning (staging pointer in staged mode, NULL in direct mode). */
struct bo_snap {
	uint64_t size;
	uint8_t *bytes;
};

static struct bo_snap bo_snap_chan(struct ane_bo *bo)
{
	struct bo_snap s = { .size = bo->size, .bytes = NULL };

	if (bo->size) {
		s.bytes = malloc(bo->size);
		memcpy(s.bytes, bo->map, bo->size);
	}
	return s;
}

static int bo_snap_same(const struct bo_snap *a, const struct bo_snap *b)
{
	return a->size == b->size &&
	       (!a->size || !memcmp(a->bytes, b->bytes, a->size));
}

static void bo_snap_free(struct bo_snap *s)
{
	free(s->bytes);
	s->bytes = NULL;
}

static void run_ab(const char *path)
{
	const int before = failures;
	struct ane_nn *staged_nn, *direct_nn;
	struct bo_snap staged_chans[TILE_COUNT], direct_chans[TILE_COUNT];
	struct bo_snap staged_btsp, direct_btsp;
	int bdx;

	setenv("ANE_LOAD_STAGED", "1", 1);
	staged_nn = ane_init(path);
	unsetenv("ANE_LOAD_STAGED");
	direct_nn = ane_init(path);

	if (!staged_nn || !direct_nn) {
		printf("FAIL A/B: init failed (staged %p direct %p)\n",
		       (void *)staged_nn, (void *)direct_nn);
		failures++;
		if (staged_nn) {
			ane_free(staged_nn);
		}
		if (direct_nn) {
			ane_free(direct_nn);
		}
		return;
	}

	if (staged_nn->data == NULL) {
		printf("FAIL A/B: staged mode left nn->data NULL\n");
		failures++;
	} else if ((uintptr_t)staged_nn->data & 0x3fff) {
		printf("FAIL A/B: staged nn->data is not 16 KiB aligned\n");
		failures++;
	}
	if (direct_nn->data != NULL) {
		printf("FAIL A/B: direct mode allocated a staging buffer "
		       "(nn->data %p)\n", direct_nn->data);
		failures++;
	}

	for (bdx = 0; bdx < TILE_COUNT; bdx++) {
		staged_chans[bdx] = bo_snap_chan(&staged_nn->chans[bdx]);
		direct_chans[bdx] = bo_snap_chan(&direct_nn->chans[bdx]);
		if (!bo_snap_same(&staged_chans[bdx], &direct_chans[bdx])) {
			printf("FAIL A/B: chans[%d] bytes differ (staged %llu B "
			       "vs direct %llu B)\n", bdx,
			       (unsigned long long)staged_chans[bdx].size,
			       (unsigned long long)direct_chans[bdx].size);
			failures++;
		}
	}
	staged_btsp = bo_snap_chan(&staged_nn->btsp_chan);
	direct_btsp = bo_snap_chan(&direct_nn->btsp_chan);
	if (!bo_snap_same(&staged_btsp, &direct_btsp)) {
		printf("FAIL A/B: btsp_chan bytes differ\n");
		failures++;
	}

	for (bdx = 0; bdx < TILE_COUNT; bdx++) {
		bo_snap_free(&staged_chans[bdx]);
		bo_snap_free(&direct_chans[bdx]);
	}
	bo_snap_free(&staged_btsp);
	bo_snap_free(&direct_btsp);
	ane_free(staged_nn);
	ane_free(direct_nn);
	printf("  [%s] A/B: staged vs direct BO bytes, and nn->data\n",
	       failures == before ? "ok" : "FAIL");
}

/* A program file shorter than its header's size: libane loads it with the
 * unread tail zero (not leftover bytes) -- in the staging buffer in staged
 * mode (16 KiB aligned), in the chans[0] buffer object in direct mode.
 * M_PERTURB fills new heap memory with nonzero bytes, so a tail that
 * nothing zeroes is seen on every run. */
static void run_short_file(const char *path, uint64_t cut, int staged)
{
	const uint64_t cut_amt = cut;
	char tmp[] = "/tmp/test_libane_short-XXXXXX";
	int before = failures;
	struct ane_nn *nn;
	uint8_t buf[4096];
	uint64_t size, left;
	FILE *in = fopen(path, "rb");
	int fd = mkstemp(tmp);
	FILE *out = fd < 0 ? NULL : fdopen(fd, "wb");
	size_t got;

	if (!in || !out) {
		printf("FAIL short file: cannot copy %s\n", path);
		failures++;
		goto done;
	}
	if (fread(buf, 1, 0x1000, in) != 0x1000) {
		printf("FAIL short file: %s has no 4 KiB header\n", path);
		failures++;
		goto done;
	}
	memcpy(&size, buf, sizeof(size)); /* struct anec: size comes first */
	fwrite(buf, 1, 0x1000, out);
	for (left = size - cut_amt; left; left -= got) {
		got = fread(buf, 1, left < sizeof(buf) ? left : sizeof(buf), in);
		if (!got)
			break;
		fwrite(buf, 1, got, out);
	}
	fclose(out);
	out = NULL;

	if (!staged) {
		run_ab(tmp);
	}
	if (staged) {
		setenv("ANE_LOAD_STAGED", "1", 1);
	} else {
		unsetenv("ANE_LOAD_STAGED");
	}
	abi_major = ANE_ABI_MAJOR;
	mallopt(M_PERTURB, 0x5a);
	nn = ane_init(tmp);
	mallopt(M_PERTURB, 0);
	unsetenv("ANE_LOAD_STAGED");
	if (!nn) {
		printf("FAIL short file: init failed\n");
		failures++;
		goto done;
	}
	if (staged) {
		if ((uintptr_t)nn->data & 0x3fff) {
			printf("FAIL short file: nn->data %p is not 16 KiB "
			       "aligned\n", nn->data);
			failures++;
		}
		for (uint64_t i = size - cut_amt; i < size; i++) {
			if (((uint8_t *)nn->data)[i]) {
				printf("FAIL short file: byte %#llx past the "
				       "file end is %#x\n", (unsigned long long)i,
				       ((uint8_t *)nn->data)[i]);
				failures++;
				break;
			}
		}
	} else {
		if (nn->data != NULL) {
			printf("FAIL short file: direct mode allocated a "
			       "staging buffer\n");
			failures++;
		}
		for (uint64_t i = size - cut_amt; i < size; i++) {
			if (((uint8_t *)nn->chans[0].map)[i]) {
				printf("FAIL short file: chans[0] byte %#llx "
				       "past the file end is %#x\n",
				       (unsigned long long)i,
				       ((uint8_t *)nn->chans[0].map)[i]);
				failures++;
				break;
			}
		}
	}
	ane_free(nn);
done:
	if (in)
		fclose(in);
	if (out)
		fclose(out);
	if (fd >= 0)
		unlink(tmp);
	printf("  [%s] ABI 1: short program file (%llu B cut), zero tail, "
	       "%s mode\n", failures == before ? "ok" : "FAIL",
	       (unsigned long long)cut_amt, staged ? "staged" : "direct");
}

/* The resident-session shape that fails on hardware (boot 2105990f:
 * REFUSE at prog_006 after the whole table was cached): one client's load
 * leaves the fw_ref floor held, further clients of byte-identical
 * programs must not hold their own section copies, and closing everyone
 * returns the counter to the floor. */
#define SEC_RELEASE_N 4

static void fake_reset(void)
{
	uint32_t h;

	for (h = 0; h <= MAX_BOS; h++) {
		live[h] = 0;
		bo_bytes[h] = 0;
		fw_ref[h] = 0;
		is_sec[h] = 0;
		bo_map[h] = NULL;
		bo_map_len[h] = 0;
	}
	held_bytes = 0;
	live_sec_bos = 0;
	next_handle = 0;
	next_prog_id = 0;
	fake_nprogs = 0;
	n_init_sizes = 0;
	fake_lookup_enotty = 0;
	fake_quarantine = 0;
	fail_next_prog_load = 0;
}

static void run_section_release(const char *path)
{
	const int before = failures;
	struct ane_nn *seed_nn, *nns[SEC_RELEASE_N];
	uint64_t base, floor_bytes;
	uint32_t k;

	/* Seed: the first load of the program is a dedup MISS, so its
	 * section BOs are sent to the firmware and stay held (fw_ref)
	 * after the client closes -- the floor a fresh boot builds. */
	abi_major = ANE_ABI_M2_MAJOR;
	fake_reset();
	base = held_bytes;
	seed_nn = ane_init(path);
	if (!seed_nn) {
		printf("FAIL section-release: seed init failed\n");
		failures++;
		return;
	}
	ane_free(seed_nn);
	floor_bytes = held_bytes - base;
	if (!floor_bytes) {
		printf("FAIL section-release: no fw_ref floor after a first "
		       "load (fake marks nothing)\n");
		failures++;
	}
	if (live_sec_bos) {
		printf("FAIL section-release: %u section BOs live after the "
		       "seed closed\n", live_sec_bos);
		failures++;
	}
	/* SEC_RELEASE_N further clients of the byte-identical program:
	 * every load is a dedup hit, so no client may hold section BOs
	 * once its program is loaded. */
	for (k = 0; k < SEC_RELEASE_N; k++) {
		nns[k] = ane_init(path);
		if (!nns[k]) {
			printf("FAIL section-release: client %u init "
			       "failed\n", k);
			failures++;
			break;
		}
		if (live_sec_bos) {
			printf("FAIL section-release: client %u holds %u "
			       "section BOs after its load (duplicate copies "
			       "pinned for the nn lifetime)\n", k,
			       live_sec_bos);
			failures++;
			break;
		}
	}
	while (k--) {
		ane_free(nns[k]);
	}
	if (held_bytes != base + floor_bytes) {
		printf("FAIL section-release: held %llu != floor %llu after "
		       "all clients closed\n", (unsigned long long)held_bytes,
		       (unsigned long long)(base + floor_bytes));
		failures++;
	}
	printf("  [%s] ABI 2: resident pattern, one fw_ref floor, no client "
	       "section copies\n", failures == before ? "ok" : "FAIL");
}

/* The open allocates the six section BOs in DESCENDING size order: the
 * big contiguous request must land while the dma32 window's largest
 * hole is fresh. The fake records every BO_INIT size in order. */
static void run_sec_order(const char *path)
{
	const int before = failures;
	struct ane_nn *nn;
	unsigned k;

	abi_major = ANE_ABI_M2_MAJOR;
	fake_reset();
	nn = ane_init(path);
	if (!nn) {
		printf("FAIL section-order: init failed\n");
		failures++;
		return;
	}
	if (n_init_sizes < ANE_M2_SEC_COUNT) {
		printf("FAIL section-order: only %u BO_INITs recorded\n",
		       n_init_sizes);
		failures++;
		ane_free(nn);
		return;
	}
	for (k = 1; k < ANE_M2_SEC_COUNT; k++) {
		if (init_sizes[k] > init_sizes[k - 1]) {
			printf("FAIL section-order: BO_INIT %u (%llu B) is "
			       "larger than BO_INIT %u (%llu B) -- sections "
			       "are not allocated largest first\n",
			       k, (unsigned long long)init_sizes[k],
			       k - 1, (unsigned long long)init_sizes[k - 1]);
			failures++;
			break;
		}
	}
	ane_free(nn);
	printf("  [%s] ABI 2: section BOs allocated descending by size "
	       "(%llu, %llu, %llu, %llu, %llu, %llu)\n",
	       failures == before ? "ok" : "FAIL",
	       (unsigned long long)init_sizes[0],
	       (unsigned long long)init_sizes[1],
	       (unsigned long long)init_sizes[2],
	       (unsigned long long)init_sizes[3],
	       (unsigned long long)init_sizes[4],
	       (unsigned long long)init_sizes[5]);
}

/* Three passes of LOOKUP_N synthetic programs (byte-different copies
 * of the fixture; the flip lands in the constant tail so every parse
 * still succeeds and every digest differs). Each pass runs as its own
 * set of device opens -- own fds, own handles, everything closed at
 * pass end, so the state that survives into the next pass is exactly
 * what survives a process exit. Pass 1 first-loads and holds; passes 2
 * and 3 must reuse the held programs through PROG_LOOKUP with ZERO
 * section BO_INITs and flat held bytes. A different-content program of
 * the same section sizes must NOT alias a held one. */
#define LOOKUP_N 38

/* Build + digest a program buffer the way ane_m2_open would. */
static int ane_m2_fread_build_digest(const void *buf, uint64_t len,
				     uint8_t out[ANE_SHA256_LEN])
{
	struct ane_m2_model model;
	struct ane_m2_sections secs;
	int err;

	err = ane_m2_program_build(buf, len, &model, &secs);
	if (err) {
		return -1;
	}
	err = ane_m2_program_digest(&secs, out);
	ane_m2_sections_free(&secs);
	return err;
}

static void run_prog_lookup(const char *path)
{
	const int before = failures;
	static struct ane_nn *nns[3][LOOKUP_N];
	static char paths[LOOKUP_N][64];
	static unsigned d1_prog[LOOKUP_N];
	uint64_t held_pass1 = 0;
	int pass, n;

	abi_major = ANE_ABI_M2_MAJOR;
	fake_reset();

	/* Two-process overlap and owner exit: the first process loads
	 * and stays open, the second process gets the same program by
	 * lookup, the first process exits, the survivor still works. */
	{
		struct ane_nn *a = ane_init(path);
		struct ane_nn *b = a ? ane_init(path) : NULL;

		check(a != NULL, "lookup: first process loads");
		check(b != NULL, "lookup: second process loads by lookup");
		if (a) {
			ane_free(a);	/* owner exit */
		}
		if (b) {
			check(ane_exec(b) == 0,
			      "lookup: survivor still executes");
			ane_free(b);
		}
	}

	/* LOOKUP_N byte-different programs. The flip walks back from the
	 * tail until the BUILT sections' digest actually differs from
	 * the unmodified fixture (the file tail can be unhashed
	 * padding). */
	for (n = 0; n < LOOKUP_N; n++) {
		uint8_t *buf;
		long len, off;
		int ok = 0;
		char name[64];
		uint8_t base[ANE_SHA256_LEN], d[ANE_SHA256_LEN];
		FILE *in = fopen(path, "rb");
		FILE *out;

		snprintf(name, sizeof(name),
			 "/tmp/t6021-lookup-p%02d.anec", n);
		memcpy(paths[n], name, sizeof(name));
		if (!in) {
			printf("FAIL lookup: cannot read %s\n", path);
			failures++;
			return;
		}
		fseek(in, 0, SEEK_END);
		len = ftell(in);
		fseek(in, 0, SEEK_SET);
		buf = malloc((size_t)len);
		if (fread(buf, 1, (size_t)len, in) != (size_t)len) {
			len = 0;
		}
		fclose(in);
		if (!len ||
		    ane_m2_fread_build_digest(buf, (uint64_t)len, base)) {
			printf("FAIL lookup: cannot digest %s\n", path);
			failures++;
			free(buf);
			return;
		}
		for (off = 1; off < 512 && off <= len; off++) {
			buf[len - off] ^= (uint8_t)(n + 1);
			if (!ane_m2_fread_build_digest(buf, (uint64_t)len,
						       d) &&
			    memcmp(base, d, ANE_SHA256_LEN) != 0) {
				ok = 1;
				break;
			}
			buf[len - off] ^= (uint8_t)(n + 1);
		}
		if (ok) {
			printf("  [dbg p%02d off=%ld base=%02x%02x%02x new=%02x%02x%02x]\n",
			       n, (long)off, base[0], base[1], base[2], d[0],
			       d[1], d[2]);
		}
		out = fopen(paths[n], "wb");
		if (!ok || !out) {
			if (out)
				fclose(out);
			printf("FAIL lookup: no content-affecting flip for "
			       "%s\n", paths[n]);
			failures++;
			free(buf);
			return;
		}
		fwrite(buf, 1, (size_t)len, out);
		fclose(out);
		/* Read back and digest the FILE: the written bytes must
		 * build to a different program than the fixture. */
		in = fopen(paths[n], "rb");
		if (!in) {
			printf("FAIL lookup: cannot reread %s\n", paths[n]);
			failures++;
			free(buf);
			return;
		}
		{
			uint8_t *check_buf = malloc((size_t)len);
			uint8_t d2[ANE_SHA256_LEN];

			if (fread(check_buf, 1, (size_t)len, in) !=
				    (size_t)len ||
			    ane_m2_fread_build_digest(check_buf,
						      (uint64_t)len, d2) ||
			    memcmp(d2, base, ANE_SHA256_LEN) == 0) {
				printf("FAIL lookup: written %s does not "
				       "change the program digest\n",
				       paths[n]);
				failures++;
				fclose(in);
				free(check_buf);
				free(buf);
				return;
			}
			free(check_buf);
			fclose(in);
		}
		free(buf);
	}

	for (pass = 0; pass < 3; pass++) {
		unsigned start = n_init_sizes;
		unsigned per_prev = 0;

		for (n = 0; n < LOOKUP_N; n++) {
			unsigned delta;

			nns[pass][n] = ane_init(paths[n]);
			check(nns[pass][n] != NULL, "lookup: init ok");
			if (!nns[pass][n]) {
				continue;
			}
			delta = n_init_sizes - start - per_prev;
			per_prev = n_init_sizes - start;
			if (pass == 0) {
				/* Every first load costs its sections
				 * plus its io BOs. */
				d1_prog[n] = delta;
			} else {
				/* A lookup hit costs the io BOs only:
				 * ZERO section BO_INITs. */
				check(delta == d1_prog[n] - ANE_M2_SEC_COUNT,
				      "lookup pass 2/3: zero section BO_INITs");
			}
			if (delta != (pass == 0 ? d1_prog[n] :
				      d1_prog[n] - ANE_M2_SEC_COUNT)) {
				printf("  [dbg lookup pass %d prog %02d delta=%u sizes:", pass, n, delta);
				for (unsigned q = start; q < start + delta && q < MAX_INIT_SIZES; q++) {
					printf(" %llu", (unsigned long long)init_sizes[q]);
				}
				printf("]\n");
			}
		}
		for (n = 0; n < LOOKUP_N; n++) {
			if (nns[pass][n]) {
				ane_free(nns[pass][n]);
			}
		}
		if (pass == 0) {
			held_pass1 = held_bytes;
			/* LOOKUP_N programs + the add fixture the
			 * overlap sub-case loaded. */
			check(fake_nprogs == LOOKUP_N + 1,
			      "lookup: distinct programs registered");
		} else {
			check(held_bytes == held_pass1,
			      "lookup: held bytes flat");
		}
	}

	/* Same sizes, different content: no alias. The 39th program
	 * allocates its own sections (15 BO_INITs -- a size-keyed lookup
	 * would have returned the held program and allocated nothing)
	 * and becomes its own registered program. */
	{
		const char *alias = "/tmp/t6021-lookup-alias.anec";
		unsigned start = n_init_sizes;
		unsigned delta;
		struct ane_nn *nn;
		uint8_t pdig[ANE_SHA256_LEN], adig[ANE_SHA256_LEN];
		FILE *in = fopen(paths[0], "rb");
		FILE *out = fopen(alias, "wb");
		uint8_t *buf;
		long len, off;
		int ok = 0;

		if (!in || !out) {
			if (in)
				fclose(in);
			if (out)
				fclose(out);
			printf("FAIL lookup: cannot write %s\n", alias);
			failures++;
			return;
		}
		fseek(in, 0, SEEK_END);
		len = ftell(in);
		fseek(in, 0, SEEK_SET);
		buf = malloc((size_t)len);
		if (fread(buf, 1, (size_t)len, in) != (size_t)len) {
			len = 0;
		}
		fclose(in);
		if (ane_m2_fread_build_digest(buf, (uint64_t)len, pdig)) {
			printf("FAIL lookup: cannot digest %s\n", paths[0]);
			failures++;
			free(buf);
			fclose(out);
			return;
		}
		/* Same sizes, different bytes: walk the flip until the
		 * built digest differs from the held program's. */
		for (off = 1; off < 512 && off <= len; off++) {
			buf[len - off] ^= 0x5a;
			if (!ane_m2_fread_build_digest(buf, (uint64_t)len,
						       adig) &&
			    memcmp(pdig, adig, ANE_SHA256_LEN) != 0) {
				ok = 1;
				break;
			}
			buf[len - off] ^= 0x5a;
		}
		fwrite(buf, 1, (size_t)len, out);
		free(buf);
		fclose(out);
		if (!ok) {
			printf("FAIL lookup: no content-affecting flip for "
			       "%s\n", alias);
			failures++;
			return;
		}
		nn = ane_init(alias);
		delta = n_init_sizes - start;
		check(nn != NULL, "lookup: different content loads");
		check(delta == d1_prog[0],
		      "lookup: same-size different-content did NOT alias "
		      "a held program (allocated its own sections)");
		check(fake_nprogs == LOOKUP_N + 2,
		      "lookup: the different content is its own program");
		if (nn) {
			ane_free(nn);
		}
		unlink(alias);
	}

	/* cleanup */
	for (n = 0; n < LOOKUP_N; n++) {
		unlink(paths[n]);
	}
	(void)before;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "../fixtures/h14-anec";
	char path[512];

	/* One stream, so libane's expected mmap errors print in order, above the
	 * scenario that injected them, under any log collector (CI keeps stdout
	 * and stderr apart and merges them out of order). */
	setvbuf(stdout, NULL, _IOLBF, 0);
	dup2(STDOUT_FILENO, STDERR_FILENO);
	snprintf(path, sizeof(path), "%s/add/program-0.anec", dir);
	run_case("ABI 2: load, exec and free", ANE_ABI_M2_MAJOR, path, 0);
	run_case("ABI 2: first BO mmap fails", ANE_ABI_M2_MAJOR, path, 1);
	run_case("ABI 2: third BO mmap fails", ANE_ABI_M2_MAJOR, path, 3);
	run_case("ABI 1: load, submit and free (direct)", ANE_ABI_MAJOR, path, 0);
	run_case("ABI 1: third BO mmap fails (direct)", ANE_ABI_MAJOR, path, 3);
	run_case("ABI 1: load, submit and free (staged)", ANE_ABI_MAJOR, path, 0);
	run_case("ABI 1: third BO mmap fails (staged)", ANE_ABI_MAJOR, path, 3);
	run_ab(path);
	run_short_file(path, 64, 0);
	run_short_file(path, 64, 1);
	run_short_file(path, (1 << 20), 0);
	run_section_release(path);
	run_sec_order(path);
	run_prog_lookup(path);
	printf(failures ? "IOCTL-CHECK FAIL\n" : "IOCTL-CHECK PASS\n");
	return failures != 0;
}
