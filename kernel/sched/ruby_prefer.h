/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KERNEL_SCHED_RUBY_PREFER_H
#define _KERNEL_SCHED_RUBY_PREFER_H

struct rq;
struct task_struct;

#ifdef CONFIG_MTK_RUBY_SCHED_PREFER
int ruby_cpuset_prefer_cpu(struct task_struct *task);
int ruby_prefer_cpu(struct task_struct *p, int selected_cpu);
int ruby_migrate_misfit(struct rq *rq, struct task_struct *p);
#endif

#endif
