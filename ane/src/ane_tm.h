// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#ifndef __ANE_TM_H__
#define __ANE_TM_H__

#include "ane.h"

void ane_tm_enable(struct ane_device *ane, bool rec);
u32 ane_tm_status(struct ane_device *ane);
u32 ane_ps_act(struct ane_device *ane);
u32 ane_ps_act_probe(struct ane_device *ane);
int ane_tm_enqueue(struct ane_device *ane, struct ane_request *req);
int ane_tm_execute(struct ane_device *ane, struct ane_request *req);
int ane_tm_recover(struct ane_device *ane);

#endif /* __ANE_TM_H__ */
