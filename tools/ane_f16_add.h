// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#ifndef __ANE_F16_ADD_H__
#define __ANE_F16_ADD_H__

#include <math.h>
#include <stdint.h>

/*
// FP16 addition reference with ties rounded away from zero.
//
// The T6021 ANE adds in fp16 with half-away-from-zero rounding: on the
// proven run every mismatch against a round-to-nearest-even reference was
// an exact half-ulp tie, and against half-away the output was bit-exact.
// Round-to-nearest-even helpers are WRONG on exact ties, so this rounds
// the exact double sum explicitly. Self-contained: no ane_f16.h.
*/

/* Exact fp16 -> double (every fp16 value is a double). */
static inline double ane_f16_to_f64(uint16_t h)
{
	uint32_t sign = h & 0x8000;
	uint32_t exp = (h >> 10) & 0x1f;
	uint32_t man = h & 0x3ff;
	double base;

	if (exp == 0x1f) {
		base = man ? (double)NAN : (double)INFINITY;
	} else if (exp == 0) {
		base = ldexp((double)man, -24); /* subnormal quantum 2^-24 */
	} else {
		base = ldexp((double)(man | 0x400), (int)exp - 25);
	}
	return sign ? -base : base;
}

/* Round a finite double to the nearest fp16 value, ties away from zero.
 * This is the rounding tail of the hardware-proven add; the mul and
 * scalar references share it. The rounding is exact in double: every
 * fp16-representable input and every product/sum of two fp16 magnitudes
 * is a multiple of 2^-24, inside the 53-bit mantissa. */
static inline uint16_t ane_f16_round_half_away(double s)
{
	uint16_t sign = signbit(s) ? (uint16_t)0x8000 : (uint16_t)0x0000;
	double a, ulp, frac, n;
	int e;

	if (isnan(s)) {
		return 0x7e00; /* canonical quiet NaN */
	}
	if (isinf(s)) {
		return (uint16_t)(sign | 0x7c00);
	}
	a = fabs(s);
	if (a == 0.0) {
		return sign; /* keep the sign of zero */
	}

	frexp(a, &e); /* a = f * 2^e, f in [0.5, 1) */
	/* Results below the min normal 2^-14 (e <= -14) sit on the
	 * subnormal quantum 2^-24; note e == -13 gives ulp 2^(e-11) ==
	 * 2^-24 too, but those results are normal numbers. */
	if (e <= -14) {
		ulp = 0x1p-24;
	} else {
		ulp = ldexp(1.0, e - 11);
	}
	frac = a / ulp; /* integer or exact half-integer */
	n = floor(frac);
	if (frac - n >= 0.5) {
		n += 1.0; /* ties away from zero */
	}

	if (e <= -14) {
		/* Subnormal result: n quanta; a tie can push n to 1024, the
		 * min normal exactly. */
		if (n >= 1024.0) {
			return sign | 0x0400;
		}
		return (uint16_t)(sign | (uint32_t)n);
	}
	if (n >= 2048.0) {
		n = 1024.0; /* exact step into the next binade */
		e += 1;
	}
	if (e > 16) {
		return (uint16_t)(sign | 0x7c00); /* overflow to inf */
	}
	/* n in [1024, 2048): biased exponent (e-1)+15, mantissa n-1024. */
	return (uint16_t)(sign | (uint32_t)((e + 14) << 10) |
			  (uint32_t)(n - 1024.0));
}

static inline uint16_t ane_f16_add_half_away(uint16_t ha, uint16_t hb)
{
	return ane_f16_round_half_away(ane_f16_to_f64(ha) +
				       ane_f16_to_f64(hb));
}

#endif /* __ANE_F16_ADD_H__ */
