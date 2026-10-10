// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2014 MediaTek Inc.
 */


#include <linux/cpuidle.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/cpu_pm.h>
#include <linux/context_tracking.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/psci.h>
#include <linux/smp.h>

#include <asm/cpuidle.h>
#include <asm/suspend.h>
#include <dt_idle_states.h>

#define MTK_CPUIDLE_PREPARE		(0)
#define MTK_CPUIDLE_RESUME		(1)

typedef int (*mtk_pwr_fn)(int type,
			  struct cpuidle_driver *drv,
			  int index);

static mtk_pwr_fn mtk_pwr_conservation;
static DEFINE_PER_CPU_READ_MOSTLY(u32 *, mtk_psci_states);

static __always_inline int __mtk_lp_enter(int index)
{
	u32 *states = __this_cpu_read(mtk_psci_states);

	return CPU_PM_CPU_IDLE_ENTER_PARAM_RCU(psci_cpu_suspend_enter,
						index, states[index]);
}

static __cpuidle int mtk_idle_state_enter(struct cpuidle_device *dev,
					 struct cpuidle_driver *drv,
					 int idx)
{
	bool rcu_idle = !(drv->states[idx].flags & CPUIDLE_FLAG_RCU_IDLE);
	int ret;

	if (mtk_pwr_conservation) {
		/* The cpuidle core enters the RCU idle section before calling us.
		 * MediaTek's model callbacks use ordinary kernel services, so make
		 * RCU watch them just as the 6.6 vendor LPM wrapper does.
		 */
		if (rcu_idle)
			ct_cpuidle_exit();
		ret = mtk_pwr_conservation(MTK_CPUIDLE_PREPARE, drv, idx);
		if (rcu_idle)
			ct_cpuidle_enter();
		if (!ret)
			ret = __mtk_lp_enter(idx);
		else if (ret > 0)
			ret = -EBUSY;
		/* Unwind the prepared model, including a rejected entry. */
		if (rcu_idle)
			ct_cpuidle_exit();
		mtk_pwr_conservation(MTK_CPUIDLE_RESUME, drv, idx);
		if (rcu_idle)
			ct_cpuidle_enter();
	} else {
		ret = -ENODEV;
	}

	return ret;
}

static __cpuidle int mtk_regular_idle_state_enter(struct cpuidle_device *dev,
						 struct cpuidle_driver *drv,
						 int idx)
{
	return __mtk_lp_enter(idx);
}

static struct cpuidle_driver mtk_cpuidle_driver __initdata = {
	.name = "mtk_cpuidle",
	.owner = THIS_MODULE,
	.states[0] = {
		.enter                  = mtk_regular_idle_state_enter,
		.exit_latency           = 1,
		.target_residency       = 1,
		.power_usage			= UINT_MAX,
		.name                   = "wfi",
		.desc                   = "wfi",
	},
	.state_count = 1
};

static const struct of_device_id mtk_idle_state_match[] __initconst = {
	{ .compatible = "mediatek,idle-state",
	  .data = mtk_idle_state_enter },
	{ .compatible = "arm,idle-state",
	  .data = mtk_regular_idle_state_enter },
	{ },
};

static int mtk_cpuidle_pm_drv_probe(struct platform_device *pdev)
{
	if (IS_ERR_OR_NULL(pdev->dev.platform_data))
		goto mtk_probe_fail;

	mtk_pwr_conservation = *((mtk_pwr_fn *)pdev->dev.platform_data);
	return 0;

mtk_probe_fail:
	return -ENODATA;
}

int mtk_cpuidle_pm_drv_remove(struct platform_device *pdev)
{
	mtk_pwr_conservation = NULL;
	return 0;
}

static struct platform_driver mtk_cpuidle_pm_driver = {
	.driver = {
	   .name = "mtk_cpuidle_pm",
	   .owner = THIS_MODULE,
	},
	.probe = mtk_cpuidle_pm_drv_probe,
	.remove = mtk_cpuidle_pm_drv_remove,
};

static int mtk_cpuidle_psci_init(unsigned int cpu, unsigned int state_count)
{
	struct device_node *cpu_node, *state_node;
	u32 *states;
	int i, ret = 0;

	if (!psci_ops.cpu_suspend)
		return -EOPNOTSUPP;

	cpu_node = of_cpu_device_node_get(cpu);
	if (!cpu_node)
		return -ENODEV;

	state_count++;
	states = kcalloc(state_count, sizeof(*states), GFP_KERNEL);
	if (!states) {
		ret = -ENOMEM;
		goto out_put_cpu;
	}

	for (i = 1; i < state_count; i++) {
		state_node = of_get_cpu_state_node(cpu_node, i - 1);
		if (!state_node) {
			ret = -ENODEV;
			goto out_free_states;
		}

		ret = of_property_read_u32(state_node,
					   "arm,psci-suspend-param",
					   &states[i]);
		of_node_put(state_node);
		if (ret)
			goto out_free_states;

		if (!psci_power_state_is_valid(states[i])) {
			pr_warn("CPU %u has invalid PSCI power state %#x\n",
				cpu, states[i]);
			ret = -EINVAL;
			goto out_free_states;
		}
	}

	per_cpu(mtk_psci_states, cpu) = states;
	of_node_put(cpu_node);
	return 0;

out_free_states:
	kfree(states);
out_put_cpu:
	of_node_put(cpu_node);
	return ret;
}

static void mtk_cpuidle_psci_deinit(unsigned int cpu)
{
	kfree(per_cpu(mtk_psci_states, cpu));
	per_cpu(mtk_psci_states, cpu) = NULL;
}

static int __init mtk_cpuidle_driver_init(void)
{
	int cpu, ret;
	struct cpuidle_driver *drv;
	struct cpuidle_device *dev;
	cpumask_t registered;

	cpumask_clear(&registered);

	ret = platform_driver_register(&mtk_cpuidle_pm_driver);
	if (ret)
		return ret;

	for_each_possible_cpu(cpu) {
		drv = kmemdup(&mtk_cpuidle_driver, sizeof(*drv), GFP_KERNEL);

		if (!drv) {
			ret = -ENOMEM;
			goto out_fail;
		}

		drv->cpumask = (struct cpumask *)cpumask_of(cpu);

#ifdef CONFIG_DT_IDLE_STATES
		ret = dt_init_idle_driver(drv, mtk_idle_state_match, 1);

		if (ret <= 0) {
			ret = ret ? : -ENODEV;
			goto out_kfree_drv;
		}
#endif

		ret = mtk_cpuidle_psci_init(cpu, ret);

		if (ret) {
			pr_err("CPU %d failed to initialize PSCI idle states\n", cpu);
			goto out_kfree_drv;
		}

		ret = cpuidle_register(drv, NULL);

		if (ret) {
			mtk_cpuidle_psci_deinit(cpu);
			goto out_kfree_drv;
		}
		cpumask_set_cpu(cpu, &registered);
	}

	return 0;

out_kfree_drv:
	kfree(drv);
out_fail:
	for_each_cpu(cpu, &registered) {
		dev = per_cpu(cpuidle_devices, cpu);
		if (!dev)
			continue;
		drv = cpuidle_get_cpu_driver(dev);
		if (!drv)
			continue;
		cpuidle_unregister(drv);
		kfree(drv);
		mtk_cpuidle_psci_deinit(cpu);
	}
	platform_driver_unregister(&mtk_cpuidle_pm_driver);

	return ret;
}
device_initcall_sync(mtk_cpuidle_driver_init);
