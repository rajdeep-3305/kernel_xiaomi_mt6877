// SPDX-License-Identifier: GPL-2.0
/* Ruby MT6877 CPU isolation support for the Linux 6.6 scheduler. */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/sched/hotplug.h>

struct cpumask __cpu_isolated_mask;
EXPORT_SYMBOL_GPL(__cpu_isolated_mask);

static DEFINE_MUTEX(ruby_isolation_lock);
static unsigned int ruby_isolation_votes[NR_CPUS];

int sched_isolate_cpu(int cpu)
{
	int ret = 0;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return -EINVAL;

	mutex_lock(&ruby_isolation_lock);
	cpus_write_lock();

	if (!cpu_online(cpu)) {
		ret = -ENODEV;
		goto out;
	}

	if (ruby_isolation_votes[cpu]++)
		goto out;

	/* Keep at least one CPU active for ordinary runnable tasks. */
	if (num_active_cpus() <= 1) {
		ruby_isolation_votes[cpu]--;
		ret = -EBUSY;
		goto out;
	}

	ret = sched_cpu_deactivate(cpu);
	if (ret) {
		ruby_isolation_votes[cpu]--;
		goto out;
	}
	cpumask_set_cpu(cpu, &__cpu_isolated_mask);

out:
	cpus_write_unlock();
	mutex_unlock(&ruby_isolation_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sched_isolate_cpu);

int sched_unisolate_cpu(int cpu)
{
	int ret = 0;

	if (cpu < 0 || cpu >= nr_cpu_ids)
		return -EINVAL;

	mutex_lock(&ruby_isolation_lock);
	cpus_write_lock();

	if (!ruby_isolation_votes[cpu]) {
		ret = -EINVAL;
		goto out;
	}

	if (--ruby_isolation_votes[cpu])
		goto out;

	if (cpu_online(cpu)) {
		ret = sched_cpu_activate(cpu);
		if (ret) {
			ruby_isolation_votes[cpu]++;
			goto out;
		}
	}
	cpumask_clear_cpu(cpu, &__cpu_isolated_mask);

out:
	cpus_write_unlock();
	mutex_unlock(&ruby_isolation_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(sched_unisolate_cpu);
