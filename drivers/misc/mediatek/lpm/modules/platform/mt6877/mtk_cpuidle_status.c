// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/cpuidle.h>
#include <linux/kernel.h>
#include <linux/sched/clock.h>

#include <mtk_lpm.h>
#include <mtk_lpm_module.h>
#include <mtk_lpm_type.h>
#include <modules/include/mt6877/mt6877_spm_comm.h>

#include "mtk_cpuidle_status.h"

static unsigned long long mtk_lpm_last_cpuidle_dis;

static int mtk_cpuidle_s2idle_index(struct cpuidle_driver *drv)
{
	int i;

	if (!drv)
		return -ENODEV;

	for (i = drv->state_count - 1; i > 0; i--)
		if (!strcmp(drv->states[i].name, S2IDLE_STATE_NAME))
			return i;

	return -ENODEV;
}

static void mtk_cpuidle_set_driver_enabled(struct cpuidle_device *dev,
						   int state, bool enabled)
{
	if (!dev || state <= 0 || state >= CPUIDLE_STATE_MAX)
		return;

	if (enabled)
		dev->states_usage[state].disable &=
			~CPUIDLE_STATE_DISABLED_BY_DRIVER;
	else
		dev->states_usage[state].disable |=
			CPUIDLE_STATE_DISABLED_BY_DRIVER;
}

long mtk_cpuidle_get_param(struct cpuidle_driver *drv, int state,
				   enum idle_param param)
{
	struct cpuidle_device *dev;
	long val = -1;
	int cpu;

	if (!drv || state < 0 || state >= drv->state_count ||
	    state >= CPUIDLE_STATE_MAX)
		return val;

	switch (param) {
	case IDLE_PARAM_LAT:
		return drv->states[state].exit_latency;
	case IDLE_PARAM_RES:
		return drv->states[state].target_residency;
	case IDLE_PARAM_EN:
		val = 1;
		cpuidle_pause_and_lock();
		if (!drv->cpumask) {
			val = !(drv->states[state].flags & CPUIDLE_FLAG_UNUSABLE);
		} else {
			for_each_cpu(cpu, drv->cpumask) {
				dev = per_cpu(cpuidle_devices, cpu);
				if (!dev ||
				    (dev->states_usage[state].disable &
				     CPUIDLE_STATE_DISABLED_BY_DRIVER)) {
					val = 0;
					break;
				}
			}
		}
		cpuidle_resume_and_unlock();
		return val;
	default:
		return val;
	}
}
EXPORT_SYMBOL_GPL(mtk_cpuidle_get_param);

void mtk_cpuidle_set_param(struct cpuidle_driver *drv, int state,
				   enum idle_param param, long val)
{
	struct cpuidle_device *dev;
	int cpu;

	if (!drv || state <= 0 || state >= drv->state_count ||
	    state >= CPUIDLE_STATE_MAX)
		return;

	cpuidle_pause_and_lock();

	switch (param) {
	case IDLE_PARAM_LAT:
		drv->states[state].exit_latency = (unsigned int)val;
		drv->states[state].exit_latency_ns =
			(u64)(unsigned int)val * NSEC_PER_USEC;
		break;
	case IDLE_PARAM_RES:
		drv->states[state].target_residency = (unsigned int)val;
		drv->states[state].target_residency_ns =
			(u64)(unsigned int)val * NSEC_PER_USEC;
		break;
	case IDLE_PARAM_EN:
		if (!drv->cpumask) {
			if (val)
				drv->states[state].flags &= ~CPUIDLE_FLAG_UNUSABLE;
			else
				drv->states[state].flags |= CPUIDLE_FLAG_UNUSABLE;
		} else {
			for_each_cpu(cpu, drv->cpumask) {
				dev = per_cpu(cpuidle_devices, cpu);
				mtk_cpuidle_set_driver_enabled(dev, state, !!val);
			}
		}
		break;
	default:
		break;
	}

	cpuidle_resume_and_unlock();
}
EXPORT_SYMBOL_GPL(mtk_cpuidle_set_param);

void mtk_cpuidle_state_enable(bool en)
{
	struct cpuidle_driver *drv;
	struct cpuidle_device *dev;
	int i, cpu;
	int suspend_type = mtk_lpm_suspend_type_get();

	cpuidle_pause_and_lock();

	for_each_possible_cpu(cpu) {
		dev = per_cpu(cpuidle_devices, cpu);
		if (!dev)
			continue;
		drv = cpuidle_get_cpu_driver(dev);
		if (!drv)
			continue;

		for (i = drv->state_count - 1; i > 0; i--) {
			if (suspend_type == MTK_LPM_SUSPEND_S2IDLE &&
			    !strcmp(drv->states[i].name, S2IDLE_STATE_NAME))
				continue;

			mtk_cpuidle_set_driver_enabled(dev, i, en);
		}
	}

	if (!en)
		mtk_lpm_last_cpuidle_dis = sched_clock();

	cpuidle_resume_and_unlock();
}
EXPORT_SYMBOL_GPL(mtk_cpuidle_state_enable);

int mtk_s2idle_state_enable(bool en)
{
	struct cpuidle_driver *drv;
	struct cpuidle_device *dev;
	int i, cpu;
	int suspend_type = mtk_lpm_suspend_type_get();
	int ret = -ENODEV;

	if (suspend_type != MTK_LPM_SUSPEND_S2IDLE)
		return ret;

	cpuidle_pause_and_lock();

	for_each_possible_cpu(cpu) {
		dev = per_cpu(cpuidle_devices, cpu);
		if (!dev)
			continue;
		drv = cpuidle_get_cpu_driver(dev);
		i = mtk_cpuidle_s2idle_index(drv);
		if (i < 0)
			continue;

		mtk_cpuidle_set_driver_enabled(dev, i, en);
		ret = 0;
	}

	if (!en)
		mtk_lpm_last_cpuidle_dis = sched_clock();

	cpuidle_resume_and_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_s2idle_state_enable);

unsigned long long mtk_cpuidle_state_last_dis_ms(void)
{
	return mtk_lpm_last_cpuidle_dis / 1000000;
}
EXPORT_SYMBOL_GPL(mtk_cpuidle_state_last_dis_ms);

long mtk_cpuidle_state_enabled(void)
{
	struct cpuidle_driver *drv;
	struct cpuidle_device *dev;
	int i, cpu;
	int suspend_type = mtk_lpm_suspend_type_get();
	long enabled = 0;

	cpuidle_pause_and_lock();

	for_each_possible_cpu(cpu) {
		dev = per_cpu(cpuidle_devices, cpu);
		if (!dev)
			continue;
		drv = cpuidle_get_cpu_driver(dev);
		if (!drv)
			continue;

		for (i = 1; i < drv->state_count; i++) {
			if (suspend_type == MTK_LPM_SUSPEND_S2IDLE &&
			    !strcmp(drv->states[i].name, S2IDLE_STATE_NAME))
				continue;

			enabled += !(dev->states_usage[i].disable &
				     CPUIDLE_STATE_DISABLED_BY_DRIVER);
		}
	}

	cpuidle_resume_and_unlock();

	return enabled;
}
EXPORT_SYMBOL_GPL(mtk_cpuidle_state_enabled);
