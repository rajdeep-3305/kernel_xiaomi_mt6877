// SPDX-License-Identifier: GPL-2.0
/*
 * Deterministic panic capture for pre-userspace boot hangs.
 *
 * This remains inert unless mrdump.boot_panic=<seconds> is supplied on the
 * kernel command line.  It is armed only after mrdump has registered its
 * panic and die notifiers, so the resulting task dump reaches EXPDB.
 */

#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/panic.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>

#include "mrdump_private.h"

#define MRDUMP_BOOT_PANIC_MIN_SECONDS	10U
#define MRDUMP_BOOT_PANIC_MAX_SECONDS	600U

static unsigned int boot_panic_seconds;
static struct task_struct *boot_panic_task;

module_param_named(boot_panic, boot_panic_seconds, uint, 0);
MODULE_PARM_DESC(boot_panic,
		 "Force an mrdump panic after N seconds to diagnose boot hangs");

static int mrdump_boot_panic_thread(void *unused)
{
	struct task_struct *init;
	unsigned long timeout = boot_panic_seconds * HZ;
	int cpu;

	set_user_nice(current, -20);
	while (timeout && !kthread_should_stop())
		timeout = schedule_timeout_interruptible(timeout);

	if (kthread_should_stop())
		return 0;

	pr_emerg("mrdump: boot diagnostic timeout after %u seconds\n",
		 boot_panic_seconds);

	init = find_get_task_by_vpid(1);
	if (init) {
		pr_emerg("mrdump: PID 1 state at boot timeout\n");
		sched_show_task(init);
		put_task_struct(init);
	} else {
		pr_emerg("mrdump: PID 1 is not present at boot timeout\n");
	}

	for_each_online_cpu(cpu)
		dump_cpu_task(cpu);

	pr_emerg("mrdump: uninterruptible tasks at boot timeout\n");
	show_state_filter(TASK_UNINTERRUPTIBLE);

	panic("mrdump: forced boot diagnostic panic");
}

void __init mrdump_boot_panic_arm(void)
{
	if (!boot_panic_seconds)
		return;

	if (boot_panic_seconds < MRDUMP_BOOT_PANIC_MIN_SECONDS ||
	    boot_panic_seconds > MRDUMP_BOOT_PANIC_MAX_SECONDS) {
		pr_warn("mrdump: boot_panic=%u outside supported range %u-%u\n",
			boot_panic_seconds, MRDUMP_BOOT_PANIC_MIN_SECONDS,
			MRDUMP_BOOT_PANIC_MAX_SECONDS);
		return;
	}

	boot_panic_task = kthread_run(mrdump_boot_panic_thread, NULL,
				      "mrdump_boot_panic");
	if (IS_ERR(boot_panic_task)) {
		pr_err("mrdump: failed to start boot panic thread: %ld\n",
		       PTR_ERR(boot_panic_task));
		boot_panic_task = NULL;
		return;
	}

	pr_warn("mrdump: boot diagnostic panic armed for %u seconds\n",
		boot_panic_seconds);
}

void mrdump_boot_panic_disarm(void)
{
	if (!boot_panic_task)
		return;

	kthread_stop(boot_panic_task);
	boot_panic_task = NULL;
}
