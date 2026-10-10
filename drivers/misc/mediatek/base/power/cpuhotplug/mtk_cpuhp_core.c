// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016 MediaTek Inc.
 */

#define pr_fmt(fmt) "cpuhp: " fmt

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/topology.h>
#include <linux/suspend.h>
#include <linux/of.h>

#include "mtk_cpuhp_private.h"

static int arch_get_nr_clusters(void)
{
	int max_id = 0;
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		int package_id = cpu_topology[cpu].package_id;

		if (package_id > max_id)
			max_id = package_id;
	}

	return max_id + 1;
}

static int arch_get_cluster_id(unsigned int cpu)
{
	int package_id = cpu_topology[cpu].package_id;

	return package_id < 0 ? 0 : package_id;
}

#ifdef CONFIG_ARM64
static void arch_get_cluster_cpus(struct cpumask *cpus, int cluster_id)
{
	unsigned int cpu;

	cpumask_clear(cpus);
	for_each_possible_cpu(cpu) {
		struct cpu_topology *cpu_topo = &cpu_topology[cpu];

		if (cpu_topo->package_id == cluster_id)
			cpumask_set_cpu(cpu, cpus);
	}
}
# else
static void arch_get_cluster_cpus(struct cpumask *cpus, int cluster_id)
{
	unsigned int cpu;

	cpumask_clear(cpus);
	for_each_possible_cpu(cpu) {
		struct cputopo_arm *cpu_topo = &cpu_topology[cpu];

		if (cpu_topo->socket_id == cluster_id)
			cpumask_set_cpu(cpu, cpus);
	}
}
#endif

static int is_multi_cluster(void)
{
	struct device_node *cn, *map;
	int multi_cluster = 0;

	cn = of_find_node_by_path("/cpus");
	if (!cn) {
		pr_debug("No CPU information found in DT\n");
		return 0;
	}

	map = of_get_child_by_name(cn, "virtual-cpu-map");
	if (map) {
		of_node_put(map);
		goto out;
	}

	map = of_get_child_by_name(cn, "cpu-map");
	if (map) {
		multi_cluster = arch_get_nr_clusters() > 1;
		of_node_put(map);
	}
out:
	of_node_put(cn);
	return multi_cluster;
}

static int get_cpu_topology(int cpu, int *isalone)
{
	int cluster;
	struct cpumask cpumask_this_cluster;

	cluster = arch_get_cluster_id(cpu);

	/*
	 * test this being hotplugged up/down CPU if the first/last core in
	 * this cluster. It would affect each platform's buck control policy.
	 */
	if (is_multi_cluster()) {
		arch_get_cluster_cpus(&cpumask_this_cluster, cluster);
		cpumask_and(&cpumask_this_cluster,
			    &cpumask_this_cluster, cpu_online_mask);

		pr_debug("cluster=%d, cpumask_weight(&cpumask_this_cluster)=%d\n",
			 cluster, cpumask_weight(&cpumask_this_cluster));

		if (cpumask_weight(&cpumask_this_cluster))
			*isalone = 0;
		else
			*isalone = 1;
	} else {
		*isalone = 0;
	}

	return cluster;
}

static int cpuhp_cpu_dead(unsigned int cpu)
{
	int cluster;
	int isalone;
	int rc = 0;

	cluster = get_cpu_topology(cpu, &isalone);

	pr_debug_ratelimited("cpu_off cluster=%d, cpu=%d, isalone=%d\n",
		cluster, (int)cpu, isalone);

	rc = cpuhp_platform_cpuoff(cluster, cpu, isalone, CPU_DEAD);

	if (rc)
		pr_debug_ratelimited("cpu off error! rc: %d\n", rc);

	return rc;
}

static int cpuhp_cpu_up(unsigned int cpu)
{
	int cluster;
	int isalone;
	int rc = 0;

	cluster = get_cpu_topology(cpu, &isalone);

	pr_debug_ratelimited("cpu_on cluster=%d, cpu=%d, isalone=%d\n",
		cluster, (int)cpu, isalone);

	rc = cpuhp_platform_cpuon(cluster, cpu, isalone, CPUHP_BRINGUP_CPU);

	if (rc)
		pr_debug_ratelimited("cpu on error! rc: %d\n", rc);

	return rc;
}

#ifdef CONFIG_PM_SLEEP
static int cpuhp_pm_callback(struct notifier_block *nb,
			     unsigned long action, void *ptr)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		break;

	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
		break;

	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}
#endif

static int __init cpuhp_init(void)
{
	int rc, state;

	pr_debug("%s+\n", __func__);

	state = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN,
				"hps/cpuhotplug",
				cpuhp_cpu_up,
				cpuhp_cpu_dead);
	if (state < 0)
		return state;

	rc = cpuhp_platform_init();
	if (rc)
		goto remove_state;

	rc = ppm_notifier();
	if (rc)
		goto remove_state;

#ifdef CONFIG_PM_SLEEP
	pm_notifier(cpuhp_pm_callback, 0);
#endif

	pr_debug("%s-\n", __func__);

	return 0;

remove_state:
	cpuhp_remove_state_nocalls(state);
	return rc;
}
late_initcall(cpuhp_init);
