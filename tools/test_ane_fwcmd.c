/* Host unit test for the CSNE_CMD request builders in
 * ane/t6021/ane_t6021_fwcmd.h. Run:
 *   make -C tools test_ane_fwcmd && tools/test_ane_fwcmd
 */
#include <assert.h>
#include <stdio.h>

#define ANE_FWCMD_HOSTTEST
#include "../ane/t6021/ane_t6021_fwcmd.h"

int main(void)
{
	unsigned char buf[ANE_FWCMD_DEFSETTING_LEN];
	size_t i;

	/* 0x26: 12 zero bytes; the transport writes the opcode at +4. */
	ane_fwcmd_mcache_size_get(buf);
	for (i = 0; i < ANE_FWCMD_SIZE_GET_LEN; i++)
		assert(buf[i] == 0);

	/* 0x2e: count u64 = 2 at +8; {4,0x33},{3,0xe} at +0x10..+0x1f;
	 * opcode halfword at +4 stays zero here. */
	ane_fwcmd_default_setting_set(buf);
	assert(buf[0x04] == 0 && buf[0x05] == 0);
	assert(buf[0x08] == 2);
	assert(buf[0x09] == 0 && buf[0x0a] == 0 && buf[0x0b] == 0 &&
	       buf[0x0c] == 0 && buf[0x0d] == 0 && buf[0x0e] == 0 &&
	       buf[0x0f] == 0);
	assert(buf[0x10] == 4);
	assert(buf[0x14] == 0x33);
	assert(buf[0x18] == 3);
	assert(buf[0x1c] == 0x0e);

	/* 0x25: dsid u32 little-endian at +8, rest of the 12 bytes zero. */
	ane_fwcmd_dsid_set(buf, 9);
	assert(buf[0x08] == 9);
	assert(buf[0x09] == 0 && buf[0x0a] == 0 && buf[0x0b] == 0);
	assert(buf[0x04] == 0 && buf[0x05] == 0 && buf[0x06] == 0 &&
	       buf[0x07] == 0);
	ane_fwcmd_dsid_set(buf, 0x0e040302);
	assert(buf[0x08] == 0x02 && buf[0x09] == 0x03 &&
	       buf[0x0a] == 0x04 && buf[0x0b] == 0x0e);

	printf("test_ane_fwcmd: ok\n");
	return 0;
}
