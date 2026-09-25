/* aneregdump: open the ANERegDump user client and write the dump.
 *
 * Usage: aneregdump <outdir>
 * Writes dump.bin (header + bytes) and index.json (one row per range:
 * name, physical address, length, status, sha256).
 */
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <IOKit/IOKitLib.h>

#define ANE_DUMP_MAGIC 0x414e4531u
#define ANE_RANGE_MAX  40u
#define ANE_NAME_MAX   24u
#define ANE_DUMP_MAX   (8u * 1024u * 1024u)

struct ane_range_rec {
	char     name[ANE_NAME_MAX];
	uint64_t pa;
	uint64_t src;
	uint32_t len;
	uint32_t off;
	uint32_t status;
	uint32_t banned;
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
	uint32_t ps[8];
	uint64_t kaslr_slide;
	uint64_t globals_va;
	uint32_t ps_first[8];
	uint32_t poll_iters;
	uint32_t poll_us;
	uint64_t pmgr_pa;
	struct ane_range_rec range[ANE_RANGE_MAX];
};

static void sha256(const uint8_t *data, uint32_t len, char out[65])
{
	uint8_t d[CC_SHA256_DIGEST_LENGTH];
	uint32_t i;

	CC_SHA256(data, len, d);
	for (i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
		sprintf(out + 2 * i, "%02x", d[i]);
	out[64] = 0;
}
static const char *status_name[] = { "ok", "gated", "nomap", "absent" };


int main(int argc, char **argv)
{
	io_service_t svc;
	io_connect_t conn = 0;
	kern_return_t kr;
	uint8_t *buf;
	size_t outsz;
	struct ane_dump_hdr *hdr;
	char path[512], digest[65];
	FILE *f, *idx;
	uint32_t i;

	if (argc != 2) {
		fprintf(stderr, "usage: aneregdump <outdir>\n");
		return 2;
	}
	svc = IOServiceGetMatchingService(kIOMainPortDefault,
	    IOServiceMatching("ANERegDump"));
	if (!svc) {
		fprintf(stderr, "ANERegDump service not found\n");
		return 1;
	}
	kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr) {
		fprintf(stderr, "IOServiceOpen: 0x%x\n", kr);
		return 1;
	}
	buf = malloc(sizeof(*hdr) + ANE_DUMP_MAX);
	if (!buf)
		return 1;
	outsz = sizeof(*hdr) + ANE_DUMP_MAX;
	kr = IOConnectCallStructMethod(conn, 0, NULL, 0, buf, &outsz);
	IOServiceClose(conn);
	if (kr) {
		fprintf(stderr, "dump: 0x%x\n", kr);
		return 1;
	}
	hdr = (struct ane_dump_hdr *)buf;
	if (hdr->magic != ANE_DUMP_MAGIC || hdr->nranges > ANE_RANGE_MAX) {
		fprintf(stderr, "bad header\n");
		return 1;
	}
	snprintf(path, sizeof(path), "%s/dump.bin", argv[1]);
	f = fopen(path, "wb");
	if (!f) {
		perror(path);
		return 1;
	}
	fwrite(buf, 1, sizeof(*hdr) + hdr->data_bytes, f);
	fclose(f);

	snprintf(path, sizeof(path), "%s/index.json", argv[1]);
	idx = fopen(path, "w");
	fprintf(idx, "{\n  \"islands_up\": %u,\n  \"pmgr_pa\": \"%#llx\",\n"
	    "  \"poll_iters\": %u,\n  \"poll_us\": %u,\n"
	    "  \"kaslr_slide\": \"%#llx\",\n  \"globals_va\": \"%#llx\",\n"
	    "  \"ps_first\": [",
	    hdr->islands_up, hdr->pmgr_pa, hdr->poll_iters, hdr->poll_us,
	    hdr->kaslr_slide, hdr->globals_va);
	for (i = 0; i < 8; i++)
		fprintf(idx, "%s\"%#x\"", i ? ", " : "", hdr->ps_first[i]);
	fprintf(idx, "],\n  \"ps_first_actual\": [");
	for (i = 0; i < 8; i++)
		fprintf(idx, "%s%u", i ? ", " : "", (hdr->ps_first[i] >> 4) & 0xfu);
	fprintf(idx, "],\n  \"ps\": [");
	for (i = 0; i < 8; i++)
		fprintf(idx, "%s\"%#x\"", i ? ", " : "", hdr->ps[i]);
	fprintf(idx, "],\n  \"ps_actual\": [");
	for (i = 0; i < 8; i++)
		fprintf(idx, "%s%u", i ? ", " : "", (hdr->ps[i] >> 4) & 0xfu);
	fprintf(idx, "],\n  \"ranges\": [\n");
	for (i = 0; i < hdr->nranges; i++) {
		struct ane_range_rec *r = &hdr->range[i];
		const uint8_t *bytes = buf + sizeof(*hdr) + r->off;

		sha256(bytes, r->len, digest);
		snprintf(path, sizeof(path), "%s/%s.bin", argv[1], r->name);
		f = fopen(path, "wb");
		if (f && r->len) {
			fwrite(bytes, 1, r->len, f);
			fclose(f);
		}
		fprintf(idx, "    {\"name\": \"%s\", \"pa\": \"%#llx\", "
		    "\"src\": \"%#llx\", \"len\": %u, \"status\": \"%s\", "
		    "\"banned\": %u}%s\n",
		    r->name, r->pa, r->src, r->len,
		    r->status < 4 ? status_name[r->status] : "?",
		    r->banned, i + 1 < hdr->nranges ? "," : "");
	}
	fprintf(idx, "  ]\n}\n");
	fclose(idx);
	printf("islands_up=%u poll_iters=%u poll_us=%u ranges=%u bytes=%u\n",
	    hdr->islands_up, hdr->poll_iters, hdr->poll_us,
	    hdr->nranges, hdr->data_bytes);
	printf("ps_first");
	for (i = 0; i < 8; i++)
		printf(" %#x(act=%u)", hdr->ps_first[i],
		    (hdr->ps_first[i] >> 4) & 0xfu);
	printf("\nps");
	for (i = 0; i < 8; i++)
		printf(" %#x(act=%u)", hdr->ps[i], (hdr->ps[i] >> 4) & 0xfu);
	printf("\n");
	return hdr->islands_up ? 0 : 3;
}
