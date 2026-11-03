/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ANE_H15_DOMAIN_WINDOW_H
#define ANE_H15_DOMAIN_WINDOW_H

enum ane_h15_window {
	ANE_WINDOW_MISSING,
	ANE_WINDOW_UNBOUNDED,
	ANE_WINDOW_OUTSIDE,
	ANE_WINDOW_INSIDE,
};

static inline enum ane_h15_window
ane_h15_classify_window(const struct iommu_domain *domain, u64 start, u64 size)
{
	u64 first, last;

	if (!domain)
		return ANE_WINDOW_MISSING;
	if (!domain->geometry.force_aperture)
		return ANE_WINDOW_UNBOUNDED;
	first = domain->geometry.aperture_start;
	last = domain->geometry.aperture_end;
	if (!size || first > last || start < first || start > last || size - 1 > last - start)
		return ANE_WINDOW_OUTSIDE;
	return ANE_WINDOW_INSIDE;
}

#endif
