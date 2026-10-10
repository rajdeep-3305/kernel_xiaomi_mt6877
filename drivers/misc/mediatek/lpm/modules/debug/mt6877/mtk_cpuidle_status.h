/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef __MTK_CPUIDLE_STATUS_H__
#define __MTK_CPUIDLE_STATUS_H__

#include <linux/cpuidle.h>
#include <linux/seq_file.h>

#define sec_to_ns(v)	((v) * 1000 * 1000 * 1000ULL)

#define S2IDLE_STATE_NAME	"s2idle"
enum idle_param {
	IDLE_PARAM_LAT,
	IDLE_PARAM_RES,
	IDLE_PARAM_EN,

	NF_IDLE_PARAM
};

long mtk_cpuidle_get_param(struct cpuidle_driver *drv, int state,
			   enum idle_param param);
void mtk_cpuidle_set_param(struct cpuidle_driver *drv, int state,
			   enum idle_param param, long val);

void mtk_cpuidle_set_stress_test(bool en);
bool mtk_cpuidle_get_stress_status(void);
void mtk_cpuidle_set_stress_time(unsigned int val);
unsigned int mtk_cpuidle_get_stress_time(void);

void mtk_cpuidle_prof_ratio_start(void);
void mtk_cpuidle_prof_ratio_stop(void);
void mtk_cpuidle_prof_ratio_dump(struct seq_file *m);

void mtk_cpuidle_ctrl_timer_en(bool enable);
bool mtk_cpuidle_ctrl_timer_sta_get(void);
void mtk_cpuidle_ctrl_log_en(bool enable);
bool mtk_cpuidle_ctrl_log_sta_get(void);

void mtk_cpuidle_state_enable(bool en);
int mtk_s2idle_state_enable(bool en);
long mtk_cpuidle_state_enabled(void);

int __init mtk_cpuidle_status_init(void);
void __exit mtk_cpuidle_status_exit(void);

unsigned long long mtk_cpuidle_state_last_dis_ms(void);

#endif /* __MTK_CPUIDLE_STATUS_H__ */
