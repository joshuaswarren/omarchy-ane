#include "shim.h"
static inline void put_unaligned_le64(u64 value, void *address)
{
	unsigned char *bytes = address;
	unsigned int i;

	for (i = 0; i < 8; i++)
		bytes[i] = value >> (i * 8);
}
