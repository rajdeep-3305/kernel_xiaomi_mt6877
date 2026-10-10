// SPDX-License-Identifier: GPL-2.0
/* MT6877 perfmgr CPU frequency limits on the Linux 6.6 cpufreq QoS API. */

#include <linux/cpufreq.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#ifdef CONFIG_MTK_CPU_CTRL_CFP
#include <linux/cpumask.h>
#include "load_track.h"
#endif

#include <mt-plat/cpu_ctrl.h>

#include "mtk_perfmgr_internal.h"
#include "topo_ctrl.h"

struct ruby_freq_cluster {
	struct cpufreq_policy *policy;
	struct freq_qos_request min_req;
	struct freq_qos_request max_req;
	struct cpu_ctrl_data limit[CPU_MAX_KIR];
};

static DEFINE_MUTEX(ruby_freq_lock);
static struct ruby_freq_cluster *ruby_freq_clusters;
static int ruby_nr_clusters;

int powerhal_tid;

#ifdef CONFIG_MTK_CPU_CTRL_CFP
#define RUBY_CFP_MAX_OPP 15

enum ruby_cfp_param {
	RUBY_CFP_ENABLE,
	RUBY_CFP_POLLING_MS,
	RUBY_CFP_UP_OPP,
	RUBY_CFP_DOWN_OPP,
	RUBY_CFP_UP_TIME,
	RUBY_CFP_DOWN_TIME,
	RUBY_CFP_UP_LOADING,
	RUBY_CFP_DOWN_LOADING,
	RUBY_CFP_PARAM_COUNT,
};

static int ruby_cfp_param[RUBY_CFP_PARAM_COUNT] = {
	[RUBY_CFP_ENABLE] = 1,
	[RUBY_CFP_POLLING_MS] = 64,
	[RUBY_CFP_UP_OPP] = 15,
	[RUBY_CFP_DOWN_OPP] = 15,
	[RUBY_CFP_UP_TIME] = 1,
	[RUBY_CFP_DOWN_TIME] = 16,
	[RUBY_CFP_UP_LOADING] = 90,
	[RUBY_CFP_DOWN_LOADING] = 80,
};
static DEFINE_MUTEX(ruby_cfp_monitor_lock);
static bool ruby_cfp_ceiled;
static bool ruby_cfp_monitoring;
static int ruby_cfp_registered_poll;
static int ruby_cfp_headroom_opp;
static int ruby_cfp_up_samples;
static int ruby_cfp_down_samples;
static int ruby_cfp_loading;

static int ruby_cfp_relax_max(struct ruby_freq_cluster *cluster, int cap)
{
	struct cpufreq_frequency_table *opp;
	unsigned int next, freq;
	int step;

	if (!cluster->policy || !cluster->policy->freq_table ||
	    cap == FREQ_QOS_MAX_DEFAULT_VALUE)
		return cap;

	freq = cap;
	for (step = 0; step < ruby_cfp_headroom_opp; step++) {
		next = UINT_MAX;
		cpufreq_for_each_valid_entry(opp, cluster->policy->freq_table) {
			if (opp->frequency > freq && opp->frequency < next)
				next = opp->frequency;
		}
		if (next == UINT_MAX)
			break;
		freq = next;
	}
	return freq;
}
#endif

static int ruby_attach_policy(struct ruby_freq_cluster *cluster, int id)
{
	struct cpufreq_policy *policy;
	int cpu, ret;

	if (cluster->policy)
		return 0;

	cpu = topo_ctrl_get_cluster_cpu_id(id);
	if (cpu < 0 || cpu >= nr_cpu_ids)
		return -ENODEV;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return -ENODEV;

	ret = freq_qos_add_request(&policy->constraints, &cluster->min_req,
				   FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
	if (ret < 0)
		goto put_policy;

	ret = freq_qos_add_request(&policy->constraints, &cluster->max_req,
				   FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
	if (ret < 0) {
		freq_qos_remove_request(&cluster->min_req);
		goto put_policy;
	}

	cluster->policy = policy;
	return 0;

put_policy:
	cpufreq_cpu_put(policy);
	return ret;
}

static void ruby_effective_limit(struct ruby_freq_cluster *cluster, int kicker,
				 const struct cpu_ctrl_data *new_limit,
				 int *min_freq, int *max_freq)
{
	int i, min = FREQ_QOS_MIN_DEFAULT_VALUE;
	int max = FREQ_QOS_MAX_DEFAULT_VALUE;

	for (i = 0; i < CPU_MAX_KIR; i++) {
		const struct cpu_ctrl_data *limit =
			i == kicker ? new_limit : &cluster->limit[i];

		if (limit->min >= 0)
			min = max_t(int, min, limit->min);
		if (limit->max >= 0)
			max = min_t(int, max, limit->max);
	}

#ifdef CONFIG_MTK_CPU_CTRL_CFP
	if (ruby_cfp_param[RUBY_CFP_ENABLE] && ruby_cfp_ceiled)
		max = ruby_cfp_relax_max(cluster, max);
#endif

	/* Match the old perfmgr behavior when its own floor exceeds its cap. */
	*min_freq = min;
	*max_freq = max_t(int, min, max);
}

static int ruby_apply_limits_locked(void)
{
	int i, min, max, ret;

	for (i = 0; i < ruby_nr_clusters; i++) {
		struct ruby_freq_cluster *cluster = &ruby_freq_clusters[i];

		if (!cluster->policy)
			continue;
		ruby_effective_limit(cluster, -1, NULL, &min, &max);
		ret = freq_qos_update_request(&cluster->max_req, max);
		if (ret < 0)
			return ret;
		ret = freq_qos_update_request(&cluster->min_req, min);
		if (ret < 0)
			return ret;
	}
	return 0;
}

#ifdef CONFIG_MTK_CPU_CTRL_CFP
static void ruby_cfp_update_ceiled_locked(void)
{
	int i, j;

	ruby_cfp_ceiled = false;
	for (i = 0; i < ruby_nr_clusters; i++) {
		for (j = 0; j < CPU_MAX_KIR; j++) {
			if (ruby_freq_clusters[i].limit[j].max >= 0) {
				ruby_cfp_ceiled = true;
				return;
			}
		}
	}
	ruby_cfp_headroom_opp = 0;
	ruby_cfp_up_samples = 0;
	ruby_cfp_down_samples = 0;
}

static void ruby_cfp_load(int mask_loading, int loading,
			  u64 *idle_time, u64 *wall_time)
{
	int before;

	if (loading < 0 || loading > 100)
		return;

	mutex_lock(&ruby_freq_lock);
	if (!ruby_cfp_param[RUBY_CFP_ENABLE] || !ruby_cfp_ceiled)
		goto out;

	ruby_cfp_loading = loading;
	before = ruby_cfp_headroom_opp;
	if (loading > ruby_cfp_param[RUBY_CFP_UP_LOADING]) {
		ruby_cfp_down_samples = 0;
		ruby_cfp_up_samples = min(ruby_cfp_up_samples + 1,
					 ruby_cfp_param[RUBY_CFP_UP_TIME]);
		if (ruby_cfp_up_samples >= ruby_cfp_param[RUBY_CFP_UP_TIME])
			ruby_cfp_headroom_opp = min(
				ruby_cfp_headroom_opp +
				ruby_cfp_param[RUBY_CFP_UP_OPP],
				RUBY_CFP_MAX_OPP);
	} else if (loading > ruby_cfp_param[RUBY_CFP_DOWN_LOADING]) {
		ruby_cfp_up_samples = 0;
		ruby_cfp_down_samples = 0;
	} else {
		ruby_cfp_up_samples = 0;
		ruby_cfp_down_samples = min(ruby_cfp_down_samples + 1,
					   ruby_cfp_param[RUBY_CFP_DOWN_TIME]);
		if (ruby_cfp_down_samples >= ruby_cfp_param[RUBY_CFP_DOWN_TIME])
			ruby_cfp_headroom_opp = max(
				ruby_cfp_headroom_opp -
				ruby_cfp_param[RUBY_CFP_DOWN_OPP], 0);
	}
	if (before != ruby_cfp_headroom_opp)
		ruby_apply_limits_locked();
out:
	mutex_unlock(&ruby_freq_lock);
}

static int ruby_cfp_reconcile(void)
{
	bool want;
	int poll, ret = 0;

	mutex_lock(&ruby_cfp_monitor_lock);
	mutex_lock(&ruby_freq_lock);
	want = ruby_cfp_param[RUBY_CFP_ENABLE] && ruby_cfp_ceiled;
	poll = ruby_cfp_param[RUBY_CFP_POLLING_MS];
	mutex_unlock(&ruby_freq_lock);

	if (ruby_cfp_monitoring &&
	    (!want || poll != ruby_cfp_registered_poll)) {
		ret = unreg_loading_tracking(ruby_cfp_load);
		if (ret)
			goto out;
		ruby_cfp_monitoring = false;
	}
	if (want && !ruby_cfp_monitoring) {
		ret = reg_loading_tracking(ruby_cfp_load, poll,
					cpu_possible_mask);
		if (!ret) {
			ruby_cfp_monitoring = true;
			ruby_cfp_registered_poll = poll;
		}
	}
out:
	mutex_unlock(&ruby_cfp_monitor_lock);
	return ret;
}
#endif

int update_userlimit_cpu_freq(int kicker, int num_cluster,
			      struct cpu_ctrl_data *freq_limit)
{
	int i, ret = 0;

	if (!freq_limit || kicker < 0 || kicker >= CPU_MAX_KIR)
		return -EINVAL;

	mutex_lock(&ruby_freq_lock);
	if (!ruby_freq_clusters || num_cluster != ruby_nr_clusters) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < ruby_nr_clusters; i++) {
		if (freq_limit[i].min < -1 || freq_limit[i].max < -1) {
			ret = -EINVAL;
			goto out;
		}
		ret = ruby_attach_policy(&ruby_freq_clusters[i], i);
		if (ret)
			goto out;
	}

	for (i = 0; i < ruby_nr_clusters; i++)
		ruby_freq_clusters[i].limit[kicker] = freq_limit[i];

#ifdef CONFIG_MTK_CPU_CTRL_CFP
	ruby_cfp_update_ceiled_locked();
#endif
	ret = ruby_apply_limits_locked();
out:
	mutex_unlock(&ruby_freq_lock);
#ifdef CONFIG_MTK_CPU_CTRL_CFP
	if (!ret)
		ret = ruby_cfp_reconcile();
#endif
	return ret;
}
EXPORT_SYMBOL(update_userlimit_cpu_freq);

static int ruby_perfserv_freq_show(struct seq_file *m, void *unused)
{
	int i;

	mutex_lock(&ruby_freq_lock);
	for (i = 0; i < ruby_nr_clusters; i++)
		seq_printf(m, "cluster %d min:%d max:%d\n", i,
			   ruby_freq_clusters[i].limit[CPU_KIR_PERF].min,
			   ruby_freq_clusters[i].limit[CPU_KIR_PERF].max);
	mutex_unlock(&ruby_freq_lock);
	return 0;
}

static int ruby_perfserv_freq_open(struct inode *inode, struct file *file)
{
	return single_open(file, ruby_perfserv_freq_show, NULL);
}

static ssize_t ruby_perfserv_freq_write(struct file *file,
					const char __user *user_buf,
					size_t count, loff_t *pos)
{
	struct cpu_ctrl_data *limits;
	char *buf, *cursor, *token;
	int fields = 0, value, ret;

	if (!count || count >= PAGE_SIZE)
		return -EINVAL;

	buf = memdup_user_nul(user_buf, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	limits = kcalloc(ruby_nr_clusters, sizeof(*limits), GFP_KERNEL);
	if (!limits) {
		ret = -ENOMEM;
		goto free_buf;
	}

	cursor = buf;
	while ((token = strsep(&cursor, " \t\n"))) {
		if (!*token)
			continue;
		if (fields >= ruby_nr_clusters * 2 ||
		    kstrtoint(token, 10, &value)) {
			ret = -EINVAL;
			goto free_limits;
		}
		if (fields & 1)
			limits[fields / 2].max = value;
		else
			limits[fields / 2].min = value;
		fields++;
	}
	if (fields != ruby_nr_clusters * 2) {
		ret = -EINVAL;
		goto free_limits;
	}

	ret = update_userlimit_cpu_freq(CPU_KIR_PERF, ruby_nr_clusters, limits);
	if (!ret)
		powerhal_tid = current->pid;
free_limits:
	kfree(limits);
free_buf:
	kfree(buf);
	return ret ? ret : count;
}

static const struct proc_ops ruby_perfserv_freq_ops = {
	.proc_open = ruby_perfserv_freq_open,
	.proc_read = seq_read,
	.proc_write = ruby_perfserv_freq_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

#ifdef CONFIG_MTK_CPU_CTRL_CFP
static int ruby_cfp_param_show(struct seq_file *m, void *unused)
{
	enum ruby_cfp_param id = (long)m->private;

	mutex_lock(&ruby_freq_lock);
	seq_printf(m, "%d\n", ruby_cfp_param[id]);
	mutex_unlock(&ruby_freq_lock);
	return 0;
}

static int ruby_cfp_param_open(struct inode *inode, struct file *file)
{
	return single_open(file, ruby_cfp_param_show, pde_data(inode));
}

static ssize_t ruby_cfp_param_write(struct file *file,
				    const char __user *buf,
				    size_t count, loff_t *pos)
{
	enum ruby_cfp_param id = (long)pde_data(file_inode(file));
	int val, ret;

	ret = kstrtoint_from_user(buf, count, 10, &val);
	if (ret)
		return ret;

	mutex_lock(&ruby_freq_lock);
	if (val == ruby_cfp_param[id] ||
	    (id == RUBY_CFP_ENABLE && (val < 0 || val > 1)) ||
	    (id == RUBY_CFP_POLLING_MS && val < 1) ||
	    ((id == RUBY_CFP_UP_OPP || id == RUBY_CFP_DOWN_OPP) &&
	     (val < 0 || val > RUBY_CFP_MAX_OPP)) ||
	    ((id == RUBY_CFP_UP_TIME || id == RUBY_CFP_DOWN_TIME) &&
	     val < 1) ||
	    (id == RUBY_CFP_UP_LOADING &&
	     (val < ruby_cfp_param[RUBY_CFP_DOWN_LOADING] || val > 100)) ||
	    (id == RUBY_CFP_DOWN_LOADING &&
	     (val < 0 || val > ruby_cfp_param[RUBY_CFP_UP_LOADING]))) {
		mutex_unlock(&ruby_freq_lock);
		return -EINVAL;
	}

	ruby_cfp_param[id] = val;
	ruby_cfp_headroom_opp = 0;
	ruby_cfp_up_samples = 0;
	ruby_cfp_down_samples = 0;
	ret = ruby_apply_limits_locked();
	mutex_unlock(&ruby_freq_lock);
	if (ret)
		return ret;
	ret = ruby_cfp_reconcile();
	return ret ? ret : count;
}

static const struct proc_ops ruby_cfp_param_ops = {
	.proc_open = ruby_cfp_param_open,
	.proc_read = seq_read,
	.proc_write = ruby_cfp_param_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int ruby_cfp_stat_show(struct seq_file *m, void *unused)
{
	mutex_lock(&ruby_freq_lock);
	seq_puts(m, "enable\tceiled\tloading\theadroom_opp\tup_time\tdown_time\n");
	seq_printf(m, "%d\t%d\t%d\t%d\t\t%d\t%d\n",
		   ruby_cfp_param[RUBY_CFP_ENABLE], ruby_cfp_ceiled,
		   ruby_cfp_loading, ruby_cfp_headroom_opp,
		   ruby_cfp_up_samples, ruby_cfp_down_samples);
	mutex_unlock(&ruby_freq_lock);
	return 0;
}

static int ruby_cfp_stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, ruby_cfp_stat_show, NULL);
}

static const struct proc_ops ruby_cfp_stat_ops = {
	.proc_open = ruby_cfp_stat_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static const char * const ruby_cfp_nodes[RUBY_CFP_PARAM_COUNT] = {
	[RUBY_CFP_ENABLE] = "cfp_enable",
	[RUBY_CFP_POLLING_MS] = "cfp_polling_ms",
	[RUBY_CFP_UP_OPP] = "cfp_up_opp",
	[RUBY_CFP_DOWN_OPP] = "cfp_down_opp",
	[RUBY_CFP_UP_TIME] = "cfp_up_time",
	[RUBY_CFP_DOWN_TIME] = "cfp_down_time",
	[RUBY_CFP_UP_LOADING] = "cfp_up_loading",
	[RUBY_CFP_DOWN_LOADING] = "cfp_down_loading",
};
#endif

int cpu_ctrl_init(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *dir;
	int i, j;

	ruby_nr_clusters = topo_ctrl_get_nr_clusters();
	if (ruby_nr_clusters <= 0)
		return -ENODEV;

	ruby_freq_clusters = kcalloc(ruby_nr_clusters,
				     sizeof(*ruby_freq_clusters), GFP_KERNEL);
	if (!ruby_freq_clusters)
		return -ENOMEM;

	for (i = 0; i < ruby_nr_clusters; i++)
		for (j = 0; j < CPU_MAX_KIR; j++)
			ruby_freq_clusters[i].limit[j] =
				(struct cpu_ctrl_data){ .min = -1, .max = -1 };

	dir = proc_mkdir("cpu_ctrl", parent);
	if (!dir || !proc_create("perfserv_freq", 0644, dir,
				 &ruby_perfserv_freq_ops))
		goto remove_dir;

#ifdef CONFIG_MTK_CPU_CTRL_CFP
	for (i = 0; i < RUBY_CFP_PARAM_COUNT; i++)
		if (!proc_create_data(ruby_cfp_nodes[i], 0644, dir,
				      &ruby_cfp_param_ops, (void *)(long)i))
			goto remove_dir;
	if (!proc_create("cfp_curr_stat", 0444, dir, &ruby_cfp_stat_ops))
		goto remove_dir;
#endif
	return 0;

remove_dir:
	if (dir)
		proc_remove(dir);
	kfree(ruby_freq_clusters);
	ruby_freq_clusters = NULL;
	return -ENOMEM;
}

void cpu_ctrl_exit(void)
{
	int i;

#ifdef CONFIG_MTK_CPU_CTRL_CFP
	mutex_lock(&ruby_cfp_monitor_lock);
	if (ruby_cfp_monitoring) {
		unreg_loading_tracking(ruby_cfp_load);
		ruby_cfp_monitoring = false;
	}
	mutex_unlock(&ruby_cfp_monitor_lock);
#endif
	mutex_lock(&ruby_freq_lock);
	for (i = 0; i < ruby_nr_clusters; i++) {
		struct ruby_freq_cluster *cluster = &ruby_freq_clusters[i];

		if (!cluster->policy)
			continue;
		freq_qos_remove_request(&cluster->max_req);
		freq_qos_remove_request(&cluster->min_req);
		cpufreq_cpu_put(cluster->policy);
	}
	kfree(ruby_freq_clusters);
	ruby_freq_clusters = NULL;
	ruby_nr_clusters = 0;
	mutex_unlock(&ruby_freq_lock);
}
