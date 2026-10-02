// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#ifndef __ANE_TM_H__
#define __ANE_TM_H__

#include "ane.h"

void ane_tm_enable(struct ane_device *ane, bool rec);
u32 ane_tm_status(struct ane_device *ane);
u32 ane_ps_act(struct ane_device *ane);
u32 ane_ps_act_probe(struct ane_device *ane);
bool ane_tm_islands_on(struct ane_device *ane, u32 *act);
int ane_tm_enqueue(struct ane_device *ane, struct ane_request *req);
int ane_tm_execute(struct ane_device *ane, struct ane_request *req);
int ane_tm_recover(struct ane_device *ane);
#define ANE_PS_ALL_ON		  ((1U << (4 * 6)) - 1)
u32 ane_tm_ps_act(struct ane_device *ane);

/*
 * Last raw TM tick read from TM_IRQ_TMST in the latest event drain.
 * Captured in ane_tm.c ane_tm_collect_events; consumed by ane_drv.c
 * stats glue. Unit is unknown — exported in the timeline file as a
 * raw tick, labeled in the file's header line.
 */
extern u32 ane_last_tmst;

#endif /* __ANE_TM_H__ */
