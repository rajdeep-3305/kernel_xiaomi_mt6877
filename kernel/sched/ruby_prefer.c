// SPDX-License-Identifier: GPL-2.0
/* Ruby MT6877 CPU preference port for the Linux 6.6 fair scheduler. */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/cputime.h>

#include "sched.h"
#include "ruby_prefer.h"

enum ruby_cpu_prefer {
	RUBY_PREFER_NONE,
	RUBY_PREFER_BIG,
	RUBY_PREFER_LITTLE,
};

/* Matches stock Ruby's SCHED_NO_BOOST and SCHED_ALL_BOOST modes. */
static int ruby_sched_boost_type;

int set_sched_boost_type(int type)
{
	if (type < 0 || type > 1)
		return -EINVAL;

	WRITE_ONCE(ruby_sched_boost_type, type);
	return type;
}
EXPORT_SYMBOL(set_sched_boost_type);

int get_sched_boost_type(void)
{
	return READ_ONCE(ruby_sched_boost_type);
}
EXPORT_SYMBOL(get_sched_boost_type);

int sched_set_task_cpuprefer(struct task_struct *p, unsigned int prefer_type)
{
	unsigned long flags;

	if (!p || prefer_type > RUBY_PREFER_LITTLE)
		return -EINVAL;

	raw_spin_lock_irqsave(&p->pi_lock, flags);
	WRITE_ONCE(p->ruby_cpu_prefer, prefer_type);
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);
	return 0;
}
EXPORT_SYMBOL(sched_set_task_cpuprefer);

int sched_set_cpuprefer(pid_t pid, unsigned int prefer_type)
{
	struct task_struct *p;
	int ret = 0;

	if (pid < 0 || prefer_type > RUBY_PREFER_LITTLE)
		return -EINVAL;

	rcu_read_lock();
	p = find_task_by_vpid(pid);
	if (!p) {
		ret = -ESRCH;
	} else {
		ret = sched_set_task_cpuprefer(p, prefer_type);
	}
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL(sched_set_cpuprefer);

int ruby_prefer_cpu(struct task_struct *p, int selected_cpu)
{
	unsigned long selected_capacity = 0, idle_capacity = 0, capacity;
	unsigned int prefer = READ_ONCE(p->ruby_cpu_prefer);
	bool prefer_idle = READ_ONCE(task_group(p)->ruby_prefer_idle);
	int idle_cpu = -1, cpu;

	/* Stock Ruby drops a LITTLE preference when UClamp boosts the task. */
	if (prefer == RUBY_PREFER_LITTLE &&
	    uclamp_eff_value(p, UCLAMP_MIN) > 0)
		prefer = RUBY_PREFER_NONE;

	/* Stock Ruby lets the task's cpuset override its per-task preference. */
	{
		unsigned int cgroup_prefer = ruby_cpuset_prefer_cpu(p);

		if (cgroup_prefer > RUBY_PREFER_NONE &&
		    cgroup_prefer <= RUBY_PREFER_LITTLE)
			prefer = cgroup_prefer;
	}

	if (READ_ONCE(ruby_sched_boost_type))
		prefer = RUBY_PREFER_BIG;
	if ((prefer == RUBY_PREFER_NONE && !prefer_idle) ||
	    prefer > RUBY_PREFER_LITTLE)
		return selected_cpu;
	if (prefer == RUBY_PREFER_NONE && selected_cpu >= 0 &&
	    selected_cpu < nr_cpu_ids && available_idle_cpu(selected_cpu))
		return selected_cpu;

	if (selected_cpu >= 0 && selected_cpu < nr_cpu_ids &&
	    cpu_active(selected_cpu) &&
	    cpumask_test_cpu(selected_cpu, p->cpus_ptr))
		selected_capacity = capacity_orig_of(selected_cpu);

	/* Stock Ruby searches capacity classes in preference order. Within a
	 * class it keeps the scheduler's selected CPU, then looks for an idle
	 * allowed CPU. Scan once for the best idle class and compare it with
	 * the selected CPU's class to preserve that order.
	 */
	for_each_online_cpu(cpu) {
		if (!cpu_active(cpu) ||
		    !cpumask_test_cpu(cpu, p->cpus_ptr) ||
		    !available_idle_cpu(cpu))
			continue;

		capacity = capacity_orig_of(cpu);
		if (idle_cpu < 0 ||
		    (prefer == RUBY_PREFER_BIG && capacity > idle_capacity) ||
		    (prefer == RUBY_PREFER_LITTLE && capacity < idle_capacity) ||
		    (prefer == RUBY_PREFER_NONE && selected_capacity &&
		     abs((long)capacity - (long)selected_capacity) <
		     abs((long)idle_capacity - (long)selected_capacity))) {
			idle_cpu = cpu;
			idle_capacity = capacity;
		}
	}

	if (idle_cpu >= 0 &&
	    (!selected_capacity ||
	     (prefer == RUBY_PREFER_BIG && idle_capacity > selected_capacity) ||
	     (prefer == RUBY_PREFER_LITTLE && idle_capacity < selected_capacity) ||
	     prefer == RUBY_PREFER_NONE))
		return idle_cpu;

	return selected_cpu;
}
