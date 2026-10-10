// SPDX-License-Identifier: GPL-2.0
/* MT6877 big task rotation adapted from Ruby's 4.19 scheduler to 6.6. */

#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/cputime.h>
#include <linux/workqueue.h>
#include <trace/hooks/sched.h>

#include "sched.h"
#include "ruby_prefer.h"

#define RUBY_ROTATION_HEAVY_TASKS 4
#define RUBY_ROTATION_MIN_RUN_NS (6ULL * NSEC_PER_MSEC)

struct ruby_rotation_state {
	struct work_struct work;
	atomic_t busy;
	struct task_struct *observed;
	u64 observed_since;
	struct task_struct *source;
	struct task_struct *target;
	int source_cpu;
	int target_cpu;
};

static DEFINE_PER_CPU(struct ruby_rotation_state, ruby_rotation);
static bool ruby_rotation_enabled;
static DEFINE_MUTEX(ruby_rotation_boost_lock);
static bool ruby_rotation_uclamp_active;
static void ruby_rotation_reset_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(ruby_rotation_reset_work,
			    ruby_rotation_reset_workfn);

void ruby_rotation_set_enable(bool enable)
{
	mutex_lock(&ruby_rotation_boost_lock);
	WRITE_ONCE(ruby_rotation_enabled, enable);
	mutex_unlock(&ruby_rotation_boost_lock);
	if (!enable)
		mod_delayed_work(system_highpri_wq, &ruby_rotation_reset_work, 0);
}
EXPORT_SYMBOL_GPL(ruby_rotation_set_enable);

static void ruby_rotation_worker(struct work_struct *work)
{
	struct ruby_rotation_state *state =
		container_of(work, struct ruby_rotation_state, work);
	struct task_struct *source = state->source;
	struct task_struct *target = state->target;
	struct rq *source_rq = cpu_rq(state->source_cpu);
	struct rq *target_rq = cpu_rq(state->target_cpu);
	unsigned long flags;
	int ret;

	/* Serialize the enable state with the swap itself. Once a disable write
	 * returns, a worker queued by an earlier tick cannot migrate a task.
	 * migrate_swap rechecks that both tasks still reside on these CPUs.
	 */
	mutex_lock(&ruby_rotation_boost_lock);
	if (READ_ONCE(ruby_rotation_enabled))
		ret = migrate_swap(source, target, state->target_cpu,
				   state->source_cpu);
	else
		ret = -ECANCELED;
	if (!ret) {
		if (!ruby_rotation_set_top_app_uclamp(true)) {
			WRITE_ONCE(ruby_rotation_uclamp_active, true);
			mod_delayed_work(system_highpri_wq,
					 &ruby_rotation_reset_work,
					 msecs_to_jiffies(64));
		}
	}
	mutex_unlock(&ruby_rotation_boost_lock);

	local_irq_save(flags);
	double_rq_lock(source_rq, target_rq);
	source_rq->active_balance = 0;
	target_rq->active_balance = 0;
	double_rq_unlock(source_rq, target_rq);
	local_irq_restore(flags);

	put_task_struct(source);
	put_task_struct(target);
	atomic_set(&state->busy, 0);
}

static void ruby_rotation_reset_workfn(struct work_struct *work)
{
	int cpu, heavy = 0;

	if (!READ_ONCE(ruby_rotation_uclamp_active))
		return;

	if (READ_ONCE(ruby_rotation_enabled)) {
		for_each_online_cpu(cpu)
			if (READ_ONCE(cpu_rq(cpu)->misfit_task_load))
				heavy++;
		if (heavy >= RUBY_ROTATION_HEAVY_TASKS) {
			mod_delayed_work(system_highpri_wq,
					 &ruby_rotation_reset_work,
					 msecs_to_jiffies(64));
			return;
		}
	}

	mutex_lock(&ruby_rotation_boost_lock);
	if (ruby_rotation_uclamp_active) {
		ruby_rotation_set_top_app_uclamp(false);
		WRITE_ONCE(ruby_rotation_uclamp_active, false);
	}
	mutex_unlock(&ruby_rotation_boost_lock);
}

static unsigned long ruby_min_capacity(void)
{
	unsigned long min_capacity = ULONG_MAX;
	int cpu;

	for_each_online_cpu(cpu)
		if (cpu_active(cpu))
			min_capacity = min(min_capacity, capacity_orig_of(cpu));

	return min_capacity;
}

static u64 ruby_observed_run_ns(int cpu, struct task_struct *task, u64 now)
{
	struct ruby_rotation_state *state = &per_cpu(ruby_rotation, cpu);
	u64 since;

	if (smp_load_acquire(&state->observed) != task)
		return 0;

	since = READ_ONCE(state->observed_since);
	if (!since || since > now)
		return 0;

	return now - since;
}

static void ruby_rotation_tick(void *unused, struct rq *source_rq)
{
	struct ruby_rotation_state *state;
	struct task_struct *source, *target = NULL, *candidate;
	struct rq *rq, *target_rq;
	unsigned long min_capacity, source_capacity, flags;
	u64 now, source_run, longest_source = 0, longest_target = 0;
	int source_cpu = cpu_of(source_rq), target_cpu = -1;
	int selected_source_cpu = -1, cpu, heavy = 0;

	now = ktime_get_ns();
	state = &per_cpu(ruby_rotation, source_cpu);
	candidate = READ_ONCE(source_rq->curr);
	if (READ_ONCE(state->observed) != candidate) {
		WRITE_ONCE(state->observed_since, now);
		smp_store_release(&state->observed, candidate);
	}

	/* Stock Ruby attempts direct misfit migration on every fair tick before
	 * entering the heavier multi-task rotation policy. */
	if (ruby_migrate_misfit(source_rq, candidate))
		return;

	if (!READ_ONCE(ruby_rotation_enabled) || !cpu_active(source_cpu) ||
	    atomic_read(&state->busy))
		return;

	min_capacity = ruby_min_capacity();
	source_capacity = capacity_orig_of(source_cpu);
	if (source_capacity != min_capacity)
		return;

	rcu_read_lock();
	for_each_online_cpu(cpu) {
		rq = cpu_rq(cpu);
		if (READ_ONCE(rq->misfit_task_load))
			heavy++;
	}
	if (heavy < RUBY_ROTATION_HEAVY_TASKS)
		goto unlock;

	/* Only the longest-running misfit task on a slow CPU can rotate. */
	for_each_online_cpu(cpu) {
		if (!cpu_active(cpu) || capacity_orig_of(cpu) != min_capacity)
			continue;
		rq = cpu_rq(cpu);
		source = READ_ONCE(rq->curr);
		if (!READ_ONCE(rq->misfit_task_load) ||
		    READ_ONCE(rq->active_balance) ||
		    READ_ONCE(source->sched_class) != &fair_sched_class)
			continue;
		source_run = ruby_observed_run_ns(cpu, source, now);
		if (source_run > longest_source) {
			longest_source = source_run;
			selected_source_cpu = cpu;
		}
	}
	/* The hook runs on every CPU, but the swap must be initiated by the
	 * tick on the CPU that owns the longest-running slow misfit task. */
	if (selected_source_cpu != source_cpu || !longest_source)
		goto unlock;
	source = READ_ONCE(source_rq->curr);
	if (!source)
		goto unlock;

	for_each_online_cpu(cpu) {
		if (!cpu_active(cpu) || capacity_orig_of(cpu) <= min_capacity)
			continue;
		rq = cpu_rq(cpu);
		candidate = READ_ONCE(rq->curr);
		if (READ_ONCE(rq->active_balance) ||
		    READ_ONCE(rq->nr_running) != 1 ||
		    READ_ONCE(candidate->sched_class) != &fair_sched_class)
			continue;
		source_run = ruby_observed_run_ns(cpu, candidate, now);
		if (source_run >= RUBY_ROTATION_MIN_RUN_NS &&
		    source_run > longest_target) {
			longest_target = source_run;
			target_cpu = cpu;
			target = candidate;
		}
	}
	if (target_cpu < 0)
		goto unlock;

	target_rq = cpu_rq(target_cpu);
	local_irq_save(flags);
	double_rq_lock(source_rq, target_rq);
	if (source_rq->curr != source || target_rq->curr != target ||
	    !source_rq->misfit_task_load || target_rq->nr_running != 1 ||
	    source_rq->active_balance || target_rq->active_balance ||
	    source->sched_class != &fair_sched_class ||
	    target->sched_class != &fair_sched_class ||
	    !sched_core_cookie_match(target_rq, source) ||
	    !sched_core_cookie_match(source_rq, target) ||
	    !cpumask_test_cpu(target_cpu, source->cpus_ptr) ||
	    !cpumask_test_cpu(source_cpu, target->cpus_ptr) ||
	    !cpu_active(source_cpu) || !cpu_active(target_cpu) ||
	    atomic_cmpxchg(&state->busy, 0, 1)) {
		double_rq_unlock(source_rq, target_rq);
		local_irq_restore(flags);
		goto unlock;
	}

	source_rq->active_balance = 1;
	target_rq->active_balance = 1;
	get_task_struct(source);
	get_task_struct(target);
	state->source = source;
	state->target = target;
	state->source_cpu = source_cpu;
	state->target_cpu = target_cpu;
	double_rq_unlock(source_rq, target_rq);
	local_irq_restore(flags);

	if (!queue_work_on(source_cpu, system_highpri_wq, &state->work)) {
		local_irq_save(flags);
		double_rq_lock(source_rq, target_rq);
		source_rq->active_balance = 0;
		target_rq->active_balance = 0;
		double_rq_unlock(source_rq, target_rq);
		local_irq_restore(flags);
		put_task_struct(source);
		put_task_struct(target);
		atomic_set(&state->busy, 0);
	}
unlock:
	rcu_read_unlock();
}

static int __init ruby_rotation_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct ruby_rotation_state *state = &per_cpu(ruby_rotation, cpu);

		INIT_WORK(&state->work, ruby_rotation_worker);
		atomic_set(&state->busy, 0);
	}
	return register_trace_android_vh_scheduler_tick(ruby_rotation_tick, NULL);
}
late_initcall(ruby_rotation_init);
