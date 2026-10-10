// SPDX-License-Identifier: GPL-2.0
/*
 * MT6877 legacy thermal power budgets -> Linux CPUfreq frequency QoS.
 *
 * Preserve the legacy ABI: budgets and estimates are in mW; zero releases
 * only this thermal client's limit. UPower's CPU dynamic + leakage model
 * is used, not the scheduler's dynamic-only Energy Model. Account for all
 * possible cores: the old PPM/HPS core-count enforcement is not available
 * here, so never assume an unimplemented hotplug limit saved any power.
 */
#include <linux/cpu.h>
#include <linux/export.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/workqueue.h>

#include "mtk_cpufreq_internal.h"
#include "mtk_cpufreq_thermal.h"
#include "plat_k6877/mtk_thermal_power_data.h"

#define THERMAL_CLUSTERS	2
#define THERMAL_OPPS	16
#define THERMAL_TAG	"mt6877-cpu-thermal: "

struct mtk_thermal_cluster {
	struct cpufreq_policy *policy;
	struct freq_qos_request max_req;
	const struct mtk_thermal_power_row *table;
	unsigned int cores;
	unsigned int cap_khz;
	bool ready;
};

static DEFINE_MUTEX(thermal_lock);
static struct mtk_thermal_cluster thermal_clusters[THERMAL_CLUSTERS];
static unsigned int thermal_budget;
static unsigned int thermal_model_mw;
static bool thermal_saturated;
static int thermal_last_error;

static void thermal_retry_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(thermal_retry, thermal_retry_work);

static u64 thermal_row_power(const struct mtk_thermal_power_row *row,
			     unsigned int cores)
{
	return ((u64)row->dynamic_uw + row->leakage_uw) * cores;
}

/* Caller holds thermal_lock. Min/max are feasible all-core DVFS bounds. */
static unsigned int thermal_bound(unsigned int opp)
{
	u64 power = 0;
	unsigned int cl, bin;

	for (cl = 0; cl < THERMAL_CLUSTERS; cl++) {
		const struct mtk_thermal_cluster *c = &thermal_clusters[cl];
		u64 worst = 0;

		if (c->table) {
			power += thermal_row_power(&c->table[opp], c->cores);
			continue;
		}
		/* Before CPUfreq identifies the bin, do not report zero power. */
		for (bin = 0; bin < ARRAY_SIZE(mt6877_thermal_power); bin++)
			worst = max(worst, thermal_row_power(
				&mt6877_thermal_power[bin][cl][opp], cl ? 2 : 6));
		power += worst;
	}
	return DIV_ROUND_UP_ULL(power, 1000);
}

/* Exhaustive 16 x 16 search; no unowned CPU hotplug or firmware commands. */
static void thermal_select(unsigned int budget, unsigned int selected[2])
{
	const struct mtk_thermal_cluster *little = &thermal_clusters[0];
	const struct mtk_thermal_cluster *big = &thermal_clusters[1];
	u64 best_power = U64_MAX, budget_uw = (u64)budget * 1000;
	u32 best_perf = 0;
	unsigned int l, b;

	selected[0] = 0;
	selected[1] = 0;
	thermal_saturated = false;
	thermal_model_mw = 0;
	if (!little->table || !big->table)
		return;

	if (!budget) {
		selected[0] = THERMAL_OPPS - 1;
		selected[1] = THERMAL_OPPS - 1;
		thermal_model_mw = thermal_bound(THERMAL_OPPS - 1);
		return;
	}

	for (l = 0; l < THERMAL_OPPS; l++) {
		for (b = 0; b < THERMAL_OPPS; b++) {
			u64 power = thermal_row_power(&little->table[l], little->cores) +
				thermal_row_power(&big->table[b], big->cores);
			u32 perf = little->table[l].capacity * little->cores +
				big->table[b].capacity * big->cores;

			if (power > budget_uw || perf < best_perf)
				continue;
			if (perf == best_perf && power >= best_power)
				continue;
			selected[0] = l;
			selected[1] = b;
			best_perf = perf;
			best_power = power;
		}
	}

	if (!best_perf) {
		/* Frequency limiting cannot meet a budget below this floor. */
		thermal_saturated = true;
		thermal_model_mw = thermal_bound(0);
	} else {
		thermal_model_mw = DIV_ROUND_UP_ULL(best_power, 1000);
	}
}

static int thermal_update(struct mtk_thermal_cluster *c, unsigned int cap)
{
	int ret;

	if (!freq_qos_request_active(&c->max_req) || c->cap_khz == cap)
		return 0;
	ret = freq_qos_update_request(&c->max_req, cap);
	if (ret < 0)
		return ret;
	c->cap_khz = cap;
	arch_update_thermal_pressure(c->policy->related_cpus, cap);
	return 0;
}

static void thermal_apply(void)
{
	unsigned int selected[THERMAL_CLUSTERS], caps[THERMAL_CLUSTERS];
	unsigned int cl;
	int ret;

	thermal_select(thermal_budget, selected);
	thermal_last_error = 0;
	for (cl = 0; cl < THERMAL_CLUSTERS; cl++) {
		struct mtk_thermal_cluster *c = &thermal_clusters[cl];

		if (c->ready && !freq_qos_request_active(&c->max_req)) {
			ret = freq_qos_add_request(&c->policy->constraints,
						   &c->max_req, FREQ_QOS_MAX,
						   FREQ_QOS_MAX_DEFAULT_VALUE);
			if (ret < 0)
				thermal_last_error = ret;
		}
		if (thermal_budget && !c->ready)
			thermal_last_error = -EAGAIN;
		caps[cl] = thermal_budget && c->table ?
			c->table[selected[cl]].frequency : FREQ_QOS_MAX_DEFAULT_VALUE;
	}

	/* Submit tighter QoS requests first; CPUfreq applies them asynchronously. */
	for (cl = 0; cl < THERMAL_CLUSTERS; cl++) {
		struct mtk_thermal_cluster *c = &thermal_clusters[cl];

		if (caps[cl] >= c->cap_khz)
			continue;
		ret = thermal_update(c, caps[cl]);
		if (ret)
			thermal_last_error = ret;
	}
	if (thermal_last_error)
		goto out;
	for (cl = 0; cl < THERMAL_CLUSTERS; cl++) {
		ret = thermal_update(&thermal_clusters[cl], caps[cl]);
		if (ret)
			thermal_last_error = ret;
	}
out:
	if (thermal_last_error) {
		pr_err_ratelimited(THERMAL_TAG "budget %u mW: QoS update failed: %d\n",
				   thermal_budget, thermal_last_error);
		mod_delayed_work(system_wq, &thermal_retry, msecs_to_jiffies(1000));
	}
	if (thermal_saturated)
		pr_warn_ratelimited(THERMAL_TAG "budget %u mW below all-core DVFS floor %u mW\n",
				    thermal_budget, thermal_model_mw);
}

static void thermal_retry_work(struct work_struct *work)
{
	mutex_lock(&thermal_lock);
	thermal_apply();
	mutex_unlock(&thermal_lock);
}

void mt_ppm_cpu_thermal_protect(unsigned int limited_power)
{
	mutex_lock(&thermal_lock);
	thermal_budget = limited_power;
	thermal_apply();
	pr_info_ratelimited(THERMAL_TAG "budget=%u mW caps=%u/%u kHz model=%u mW saturated=%d error=%d\n",
		thermal_budget, thermal_clusters[0].cap_khz,
		thermal_clusters[1].cap_khz, thermal_model_mw,
		thermal_saturated, thermal_last_error);
	mutex_unlock(&thermal_lock);
}
EXPORT_SYMBOL_GPL(mt_ppm_cpu_thermal_protect);

unsigned int mt_ppm_thermal_get_min_power(void)
{
	unsigned int power;

	mutex_lock(&thermal_lock);
	power = thermal_bound(0);
	mutex_unlock(&thermal_lock);
	return power;
}
EXPORT_SYMBOL_GPL(mt_ppm_thermal_get_min_power);

unsigned int mt_ppm_thermal_get_max_power(void)
{
	unsigned int power;

	mutex_lock(&thermal_lock);
	power = thermal_bound(THERMAL_OPPS - 1);
	mutex_unlock(&thermal_lock);
	return power;
}
EXPORT_SYMBOL_GPL(mt_ppm_thermal_get_max_power);

unsigned int mt_ppm_thermal_get_cur_power(void)
{
	u64 power = 0;
	unsigned int cl;

	mutex_lock(&thermal_lock);
	for (cl = 0; cl < THERMAL_CLUSTERS; cl++) {
		struct mtk_thermal_cluster *c = &thermal_clusters[cl];
		unsigned int opp, freq, online;

		if (!c->table) {
			power = (u64)thermal_bound(THERMAL_OPPS - 1) * 1000;
			break;
		}
		if (!c->policy)
			continue;
		if (!c->ready) {
			power += thermal_row_power(&c->table[THERMAL_OPPS - 1], c->cores);
			continue;
		}
		online = cpumask_weight_and(c->policy->related_cpus, cpu_online_mask);
		freq = READ_ONCE(c->policy->cur);
		for (opp = 0; opp < THERMAL_OPPS - 1; opp++)
			if (c->table[opp].frequency >= freq)
				break;
		power += thermal_row_power(&c->table[opp], online);
	}
	mutex_unlock(&thermal_lock);
	return DIV_ROUND_UP_ULL(power, 1000);
}
EXPORT_SYMBOL_GPL(mt_ppm_thermal_get_cur_power);

int mtk_cpu_thermal_prepare(struct cpufreq_policy *policy,
			    const struct mt_cpu_dvfs *dvfs)
{
	const struct mtk_thermal_power_row *table;
	struct mtk_thermal_cluster *c;
	unsigned int i, cores;
	int ret = 0;

	if (dvfs->id >= THERMAL_CLUSTERS ||
	    dvfs->cpu_level >= ARRAY_SIZE(mt6877_thermal_power) ||
	    dvfs->nr_opp_tbl != THERMAL_OPPS)
		return -EINVAL;
	cores = cpumask_weight(policy->cpus);
	if (cores != (dvfs->id ? 2 : 6))
		return -EINVAL;
	table = mt6877_thermal_power[dvfs->cpu_level][dvfs->id];
	for (i = 0; i < THERMAL_OPPS; i++) {
		const struct mt_cpu_freq_info *opp = &dvfs->opp_tbl[THERMAL_OPPS - 1 - i];

		/* Reject mismatched/overclocked tables rather than underestimating. */
		if (opp->cpufreq_khz != table[i].frequency ||
		    opp->cpufreq_volt != table[i].voltage) {
			pr_err(THERMAL_TAG "cluster %u OPP %u does not match power model\n",
			       dvfs->id, i);
			return -EINVAL;
		}
	}
	mutex_lock(&thermal_lock);
	c = &thermal_clusters[dvfs->id];
	if (c->policy && (c->policy != policy || c->ready)) {
		ret = -EBUSY;
	} else {
		c->policy = policy;
		c->table = table;
		c->cores = cores;
		c->cap_khz = FREQ_QOS_MAX_DEFAULT_VALUE;
	}
	mutex_unlock(&thermal_lock);
	return ret;
}

void mtk_cpu_thermal_ready(struct cpufreq_policy *policy)
{
	unsigned int cl;

	mutex_lock(&thermal_lock);
	for (cl = 0; cl < THERMAL_CLUSTERS; cl++) {
		struct mtk_thermal_cluster *c = &thermal_clusters[cl];

		if (c->policy != policy)
			continue;
		c->ready = true;
		thermal_apply();
		pr_info(THERMAL_TAG "CPU%u: %u cores, nominal dynamic+leakage model, QoS=%u budget=%u mW cap=%u kHz error=%d\n",
			policy->cpu, c->cores, freq_qos_request_active(&c->max_req),
			thermal_budget, c->cap_khz, thermal_last_error);
	}
	mutex_unlock(&thermal_lock);
}

void mtk_cpu_thermal_exit(struct cpufreq_policy *policy)
{
	unsigned int cl;

	mutex_lock(&thermal_lock);
	for (cl = 0; cl < THERMAL_CLUSTERS; cl++) {
		struct mtk_thermal_cluster *c = &thermal_clusters[cl];

		if (c->policy != policy)
			continue;
		if (freq_qos_request_active(&c->max_req))
			freq_qos_remove_request(&c->max_req);
		arch_update_thermal_pressure(policy->related_cpus,
					     FREQ_QOS_MAX_DEFAULT_VALUE);
		c->policy = NULL;
		c->ready = false;
		c->cap_khz = FREQ_QOS_MAX_DEFAULT_VALUE;
	}
	mutex_unlock(&thermal_lock);
}

static int thermal_status_show(struct seq_file *m, void *unused)
{
	unsigned int cl;

	mutex_lock(&thermal_lock);
	seq_puts(m, "model: MT6877 nominal UPower dynamic+leakage; all possible cores\n");
	seq_printf(m, "budget_mw: %u\nselected_model_mw: %u\nsaturated: %u\nlast_error: %d\n",
		   thermal_budget, thermal_model_mw, thermal_saturated, thermal_last_error);
	seq_printf(m, "min_mw: %u\nmax_mw: %u\n",
		   thermal_bound(0), thermal_bound(THERMAL_OPPS - 1));
	for (cl = 0; cl < THERMAL_CLUSTERS; cl++) {
		struct mtk_thermal_cluster *c = &thermal_clusters[cl];

		seq_printf(m, "cluster%u: ready=%u cores=%u thermal_cap_khz=%u\n",
			   cl, freq_qos_request_active(&c->max_req), c->cores, c->cap_khz);
		if (c->ready)
			seq_printf(m, " policy_cpu=%u min_khz=%u max_khz=%u cur_khz=%u\n",
				   c->policy->cpu, READ_ONCE(c->policy->min),
				   READ_ONCE(c->policy->max), READ_ONCE(c->policy->cur));
	}
	mutex_unlock(&thermal_lock);
	return 0;
}

static int __init mtk_cpu_thermal_status_init(void)
{
	if (!proc_create_single("mt6877_cpu_thermal", 0444, NULL, thermal_status_show))
		return -ENOMEM;
	return 0;
}
late_initcall(mtk_cpu_thermal_status_init);
