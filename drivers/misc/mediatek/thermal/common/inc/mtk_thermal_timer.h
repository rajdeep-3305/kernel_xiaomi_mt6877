/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef _MTK_THERMAL_TIMER_H_
#define _MTK_THERMAL_TIMER_H_

#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_MACH_MT6877)
void mtk_thermal_cpu_pm_set_ready(bool ready);
#else
static inline void mtk_thermal_cpu_pm_set_ready(bool ready) { }
#endif

extern int mtkTTimer_register
(const char *name, void (*start_timer) (void), void (*cancel_timer) (void));

extern int mtkTTimer_unregister(const char *name);
#endif	/* _MTK_THERMAL_TIMER_H_ */
