// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */
#ifndef ANE_SHA256_H
#define ANE_SHA256_H

#include <stdint.h>

#define ANE_SHA256_LEN 32

struct ane_sha256_ctx {
	uint32_t st[8];
	uint8_t buf[64];
	unsigned buflen;
	uint64_t total;
};

void ane_sha256_init(struct ane_sha256_ctx *c);
void ane_sha256_update(struct ane_sha256_ctx *c, const void *data,
		       uint64_t len);
void ane_sha256_final(struct ane_sha256_ctx *c, uint8_t out[ANE_SHA256_LEN]);

/* Runtime compression-path selection. hw_supported reports whether the
 * ARMv8 SHA-2 crypto extension is usable (always 0 off arm64); forced
 * portable keeps the reference C path even when it is. The test uses
 * both to cross-check the two paths. */
int ane_sha256_hw_supported(void);
void ane_sha256_force_portable(int force);

/* One-shot convenience. */
void ane_sha256(const void *data, uint64_t len, uint8_t out[ANE_SHA256_LEN]);

#endif /* ANE_SHA256_H */
