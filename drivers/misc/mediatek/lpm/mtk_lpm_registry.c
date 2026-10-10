// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/kernel.h>
#include <linux/cpuidle.h>
#include <linux/cpu.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "mtk_lpm_registry.h"
#include "mtk_lpm_internal.h"

#define MTK_LPM_REGISTRY_MAGIC		0xCDAC9131

struct mtk_lpm_registry_wk {
	unsigned int magic;
	int type;
	void *priv;
	blockcall cb;
	struct cpumask cpus;
};

#define INIT_MTK_LPM_REG_WK(wk, _type, _cb, _priv) ({\
		(wk)->magic = MTK_LPM_REGISTRY_MAGIC;\
		(wk)->type = _type;\
		(wk)->cb = _cb;\
		(wk)->priv = _priv; })


#define IS_MTK_LPM_REG_WK(wk)\
	((wk) && ((wk)->magic == MTK_LPM_REGISTRY_MAGIC))



#define IS_MTK_LPM_REG_WAKEALL(cpumask)\
	(cpumask_weight(cpumask) == num_online_cpus())

static long mtk_lpm_registry_work(void *pData)
{
	struct mtk_lpm_registry_wk *lpm_wk =
			(struct mtk_lpm_registry_wk *)pData;
	unsigned long flags;
	int ret = 0;

	if (!IS_MTK_LPM_REG_WK(lpm_wk))
		return -EINVAL;

	switch (lpm_wk->type) {
	case MTK_LPM_REG_PER_CPU:
		mtk_lpm_system_lock(flags);
		ret = lpm_wk->cb(smp_processor_id(), lpm_wk->priv);
		mtk_lpm_system_unlock(flags);
		synchronize_rcu();
		return ret;
	case MTK_LPM_REG_ALL_ONLINE:
		mtk_lpm_system_lock(flags);
		cpumask_set_cpu(smp_processor_id(), &lpm_wk->cpus);
		if (IS_MTK_LPM_REG_WAKEALL(&lpm_wk->cpus))
			ret = lpm_wk->cb(smp_processor_id(), lpm_wk->priv);
		mtk_lpm_system_unlock(flags);
		synchronize_rcu();
		return ret;
	default:
		return -EINVAL;
	}
}

int mtk_lpm_do_work(int type, blockcall call, void *priv)
{
	int cpu;
	long ret, first_error = 0;
	struct mtk_lpm_registry_wk lpm_wk;

	if (!call || (type != MTK_LPM_REG_PER_CPU &&
		     type != MTK_LPM_REG_ALL_ONLINE))
		return -EINVAL;

	INIT_MTK_LPM_REG_WK(&lpm_wk, type, call, priv);
	cpus_read_lock();
	cpuidle_pause_and_lock();

	cpumask_clear(&lpm_wk.cpus);

	for_each_online_cpu(cpu) {
		ret = work_on_cpu(cpu, mtk_lpm_registry_work, &lpm_wk);
		if (ret && !first_error)
			first_error = ret;
	}

	cpuidle_resume_and_unlock();
	cpus_read_unlock();

	return (int)first_error;
}
