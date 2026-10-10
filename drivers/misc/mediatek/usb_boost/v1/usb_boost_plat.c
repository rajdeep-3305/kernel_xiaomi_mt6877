// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 MediaTek Inc.
 */
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mtk-pm-qos.h>
#include <helio-dvfsrc-opp.h>

#include "usb_boost.h"

/* platform specific parameter here */
#if defined(CONFIG_MACH_MT6758)
static int cpu_freq_test_para[] = {1, 5, 500, 0};
static int cpu_core_test_para[] = {1, 5, 500, 0};
static int dram_vcore_test_para[] = {1, 5, 500, 0};

/* -1 denote not used*/
struct act_arg_obj cpu_freq_test_arg = {2500000, -1, -1};
struct act_arg_obj cpu_core_test_arg = {4, -1, -1};
struct act_arg_obj dram_vcore_test_arg = {DDR_OPP_0, -1, -1};
#elif defined(CONFIG_MACH_MT6765)
static int cpu_freq_test_para[] = {1, 5, 500, 0};
static int cpu_core_test_para[] = {1, 5, 500, 0};
static int dram_vcore_test_para[] = {1, 5, 500, 0};

/* -1 denote not used*/
struct act_arg_obj cpu_freq_test_arg = {2500000, -1, -1};
struct act_arg_obj cpu_core_test_arg = {4, -1, -1};
struct act_arg_obj dram_vcore_test_arg = {DDR_OPP_0, -1, -1};
#elif defined(CONFIG_MACH_MT6761)
static int cpu_freq_test_para[] = {1, 5, 500, 0};
static int cpu_core_test_para[] = {1, 5, 500, 0};
static int dram_vcore_test_para[] = {1, 5, 500, 0};

/* -1 denote not used*/
struct act_arg_obj cpu_freq_test_arg = {2500000, -1, -1};
struct act_arg_obj cpu_core_test_arg = {4, -1, -1};
struct act_arg_obj dram_vcore_test_arg = {DDR_OPP_0, -1, -1};
#elif defined(CONFIG_MACH_MT6779)
static int cpu_freq_test_para[] = {1, 5, 500, 0};
static int cpu_core_test_para[] = {1, 5, 500, 0};
static int dram_vcore_test_para[] = {1, 5, 500, 0};

/* -1 denote not used*/
struct act_arg_obj cpu_freq_test_arg = {2500000, -1, -1};
struct act_arg_obj cpu_core_test_arg = {4, -1, -1};
struct act_arg_obj dram_vcore_test_arg = {DDR_OPP_0, -1, -1};
#elif defined(CONFIG_MACH_MT6763)
static int cpu_freq_test_para[] = {1, 5, 500, 0};
static int cpu_core_test_para[] = {1, 5, 500, 0};
static int dram_vcore_test_para[] = {1, 5, 500, 0};

/* -1 denote not used*/
struct act_arg_obj cpu_freq_test_arg = {2500000, -1, -1};
struct act_arg_obj cpu_core_test_arg = {4, -1, -1};
struct act_arg_obj dram_vcore_test_arg = {DDR_OPP_0, -1, -1};
#elif defined(CONFIG_MACH_MT6771) || defined(CONFIG_MACH_MT6768) \
		|| defined(CONFIG_MACH_MT6781) || defined(CONFIG_MACH_MT6785) \
		|| defined(CONFIG_MACH_MT6833) || defined(CONFIG_MACH_MT6853) \
		|| defined(CONFIG_MACH_MT6873) || defined(CONFIG_MACH_MT6877) \
		|| defined(CONFIG_MACH_MT6885) || defined(CONFIG_MACH_MT6893) \

static int cpu_freq_test_para[] = {1, 5, 500, 0};
static int cpu_core_test_para[] = {1, 5, 500, 0};
static int dram_vcore_test_para[] = {1, 5, 500, 0};

/* -1 denote not used*/
struct act_arg_obj cpu_freq_test_arg = {2500000, -1, -1};
struct act_arg_obj cpu_core_test_arg = {4, -1, -1};
struct act_arg_obj dram_vcore_test_arg = {DDR_OPP_0, -1, -1};
#elif defined(CONFIG_MACH_MT6739)
static int cpu_freq_test_para[] = {1, 5, 500, 0};
static int cpu_core_test_para[] = {1, 5, 500, 0};
static int dram_vcore_test_para[] = {1, 5, 500, 0};

/* -1 denote not used*/
struct act_arg_obj cpu_freq_test_arg = {1500000, -1, -1};
struct act_arg_obj cpu_core_test_arg = {4, -1, -1};
struct act_arg_obj dram_vcore_test_arg = {DDR_OPP_0, -1, -1};
#elif defined(CONFIG_ARCH_MT6XXX)
/* add new here */
#endif

static struct pm_qos_request pm_qos_req;
static struct mtk_pm_qos_request pm_qos_emi_req;

struct usb_policy {
	struct freq_qos_request qos_req;
	struct cpufreq_policy *policy;
	struct list_head node;
};

static LIST_HEAD(usb_policy_list);

static void remove_freq_qos_requests(void)
{
	struct usb_policy *req, *tmp;

	list_for_each_entry_safe(req, tmp, &usb_policy_list, node) {
		freq_qos_remove_request(&req->qos_req);
		cpufreq_cpu_put(req->policy);
		list_del(&req->node);
		kfree(req);
	}
}

static int add_freq_qos_requests(void)
{
	struct cpufreq_policy *policy;
	struct usb_policy *req;
	cpumask_t covered;
	int cpu, ret;

	cpumask_clear(&covered);
	list_for_each_entry(req, &usb_policy_list, node)
		cpumask_or(&covered, &covered, req->policy->related_cpus);

	for_each_possible_cpu(cpu) {
		if (cpumask_test_cpu(cpu, &covered))
			continue;

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		cpumask_or(&covered, &covered, policy->related_cpus);
		req = kzalloc(sizeof(*req), GFP_KERNEL);
		if (!req) {
			cpufreq_cpu_put(policy);
			ret = -ENOMEM;
			goto err_remove_requests;
		}

		req->policy = policy;
		ret = freq_qos_add_request(&policy->constraints, &req->qos_req,
					   FREQ_QOS_MIN,
					   FREQ_QOS_MIN_DEFAULT_VALUE);
		if (ret < 0) {
			cpufreq_cpu_put(policy);
			kfree(req);
			goto err_remove_requests;
		}

		list_add_tail(&req->node, &usb_policy_list);
	}

	return list_empty(&usb_policy_list) ? -ENODEV : 0;

err_remove_requests:
	remove_freq_qos_requests();
	return ret;
}

static int freq_hold(struct act_arg_obj *arg)
{
	struct usb_policy *req;
	int ret;

	USB_BOOST_DBG("\n");

	ret = add_freq_qos_requests();
	if (ret)
		return ret;

	list_for_each_entry(req, &usb_policy_list, node) {
		ret = freq_qos_update_request(&req->qos_req, req->policy->max);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int freq_release(struct act_arg_obj *arg)
{
	struct usb_policy *req;
	int ret;

	USB_BOOST_DBG("\n");

	list_for_each_entry(req, &usb_policy_list, node) {
		ret = freq_qos_update_request(&req->qos_req,
					      FREQ_QOS_MIN_DEFAULT_VALUE);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int core_hold(struct act_arg_obj *arg)
{
	USB_BOOST_DBG("\n");

	/*Disable MCDI to save around 100us
	 *"Power ON CPU -> CPU context restore"
	 */
	cpu_latency_qos_update_request(&pm_qos_req, 50);
	return 0;
}

static int core_release(struct act_arg_obj *arg)
{
	USB_BOOST_DBG("\n");

	/*Enable MCDI*/
	cpu_latency_qos_update_request(&pm_qos_req, PM_QOS_DEFAULT_VALUE);
	return 0;
}

static int vcorefs_hold(struct act_arg_obj *arg)
{
	mtk_pm_qos_update_request(&pm_qos_emi_req, DDR_OPP_0);
	return 0;
}

static int vcorefs_release(struct act_arg_obj *arg)
{
	mtk_pm_qos_update_request(&pm_qos_emi_req, DDR_OPP_UNREQ);
	return 0;
}

static int __init usbboost(void)
{
	/* mandatory, related resource inited*/
	usb_boost_init();

	/* optional, change setting for alorithm other than default*/
	usb_boost_set_para_and_arg(TYPE_CPU_FREQ, cpu_freq_test_para,
			ARRAY_SIZE(cpu_freq_test_para), &cpu_freq_test_arg);
	usb_boost_set_para_and_arg(TYPE_CPU_CORE, cpu_core_test_para,
			ARRAY_SIZE(cpu_core_test_para), &cpu_core_test_arg);
	usb_boost_set_para_and_arg(TYPE_DRAM_VCORE, dram_vcore_test_para,
			ARRAY_SIZE(dram_vcore_test_para), &dram_vcore_test_arg);

	/* mandatory, hook callback depends on platform */
	register_usb_boost_act(TYPE_CPU_FREQ, ACT_HOLD, freq_hold);
	register_usb_boost_act(TYPE_CPU_FREQ, ACT_RELEASE, freq_release);
	register_usb_boost_act(TYPE_CPU_CORE, ACT_HOLD, core_hold);
	register_usb_boost_act(TYPE_CPU_CORE, ACT_RELEASE, core_release);
	register_usb_boost_act(TYPE_DRAM_VCORE, ACT_HOLD, vcorefs_hold);
	register_usb_boost_act(TYPE_DRAM_VCORE, ACT_RELEASE, vcorefs_release);

	cpu_latency_qos_add_request(&pm_qos_req, PM_QOS_DEFAULT_VALUE);

	mtk_pm_qos_add_request(&pm_qos_emi_req, MTK_PM_QOS_DDR_OPP,
		MTK_PM_QOS_DDR_OPP_DEFAULT_VALUE);

	return 0;
}
module_init(usbboost);

static void __exit clean(void)
{
	remove_freq_qos_requests();
	cpu_latency_qos_remove_request(&pm_qos_req);
	mtk_pm_qos_remove_request(&pm_qos_emi_req);
}
module_exit(clean);
MODULE_LICENSE("GPL");
