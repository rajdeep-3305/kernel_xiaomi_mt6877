// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/atomic.h>
#include <linux/cpuidle.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include <linux/cpu_pm.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/syscore_ops.h>
#include <linux/suspend.h>
#include <linux/rtc.h>
#include <linux/hrtimer.h>
#include <linux/timer.h>
#include <linux/completion.h>
#include <linux/context_tracking_irq.h>
#include <linux/jiffies.h>
#include <asm/cpuidle.h>
#include <asm/suspend.h>

#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <mt6877_spm_comm.h>
#include <mtk_lpm.h>
#include <mtk_lpm_module.h>
#include <mtk_lpm_call.h>
#include <mtk_lpm_type.h>
#include <mtk_lpm_call_type.h>
#include <mtk_dbg_common_v1.h>
#include <mtk_cpuidle_status.h>
#include <mt-plat/mtk_ccci_common.h>
#include <uapi/linux/sched/types.h>
#include "mt6877.h"
#include "mt6877_suspend.h"

unsigned int mt6877_suspend_status;
struct md_sleep_status before_md_sleep_status;
struct md_sleep_status after_md_sleep_status;
struct cpumask s2idle_cpumask;
struct mtk_lpm_model mt6877_model_suspend;


void __attribute__((weak)) subsys_if_on(void)
{
	printk_deferred("[name:spm&]NO %s !!!\n", __func__);
}
void __attribute__((weak)) pll_if_on(void)
{
	printk_deferred("[name:spm&]NO %s !!!\n", __func__);
}
void __attribute__((weak)) gpio_dump_regs(void)
{
	printk_deferred("[name:spm&]NO %s !!!\n", __func__);
}

void mtk_suspend_gpio_dbg(void)
{
#if !defined(CONFIG_FPGA_EARLY_PORTING)
	gpio_dump_regs();
#endif
}
EXPORT_SYMBOL(mtk_suspend_gpio_dbg);

void mtk_suspend_clk_dbg(void){}
EXPORT_SYMBOL(mtk_suspend_clk_dbg);

#define MD_SLEEP_INFO_SMEM_OFFEST (4)
static void get_md_sleep_time(struct md_sleep_status *md_data)
{
	/* dump subsystem sleep info */
#if defined(CONFIG_MTK_ECCCI_DRIVER)
	u32 *share_mem = NULL;

	if (!md_data)
		return;

	share_mem = (u32 *)get_smem_start_addr(SMEM_USER_LOW_POWER, NULL);
	if (share_mem == NULL) {
		printk_deferred("[name:spm&][%s:%d] - No MD share mem\n",
			 __func__, __LINE__);
		return;
	}
	share_mem = share_mem + MD_SLEEP_INFO_SMEM_OFFEST;
	memset(md_data, 0, sizeof(struct md_sleep_status));
	memcpy(md_data, share_mem, sizeof(struct md_sleep_status));
#else
	return;
#endif
}

static void log_md_sleep_info(void)
{
#define LOG_BUF_SIZE	256
	char log_buf[LOG_BUF_SIZE] = { 0 };
	int log_size = 0;

	if (after_md_sleep_status.sleep_time >= before_md_sleep_status.sleep_time) {
		printk_deferred("[name:spm&][SPM] md_slp_duration = %llu (32k)\n",
			after_md_sleep_status.sleep_time - before_md_sleep_status.sleep_time);

		log_size += scnprintf(log_buf + log_size,
		LOG_BUF_SIZE - log_size, "[name:spm&][SPM] ");
		log_size += scnprintf(log_buf + log_size,
		LOG_BUF_SIZE - log_size, "MD/2G/3G/4G/5G_FR1 = ");
		log_size += scnprintf(log_buf + log_size,
		LOG_BUF_SIZE - log_size, "%llu.%03llu/%llu.%03llu/%llu.%03llu/%llu.%03llu/%llu.%03llu seconds",
			(after_md_sleep_status.md_sleep_time -
				before_md_sleep_status.md_sleep_time) / 1000000,
			(after_md_sleep_status.md_sleep_time -
				before_md_sleep_status.md_sleep_time) % 1000000 / 1000,
			(after_md_sleep_status.gsm_sleep_time -
				before_md_sleep_status.gsm_sleep_time) / 1000000,
			(after_md_sleep_status.gsm_sleep_time -
				before_md_sleep_status.gsm_sleep_time) % 1000000 / 1000,
			(after_md_sleep_status.wcdma_sleep_time -
				before_md_sleep_status.wcdma_sleep_time) / 1000000,
			(after_md_sleep_status.wcdma_sleep_time -
				before_md_sleep_status.wcdma_sleep_time) % 1000000 / 1000,
			(after_md_sleep_status.lte_sleep_time -
				before_md_sleep_status.lte_sleep_time) / 1000000,
			(after_md_sleep_status.lte_sleep_time -
				before_md_sleep_status.lte_sleep_time) % 1000000 / 1000,
			(after_md_sleep_status.nr_sleep_time -
				before_md_sleep_status.nr_sleep_time) / 1000000,
			(after_md_sleep_status.nr_sleep_time -
				before_md_sleep_status.nr_sleep_time) % 10000000 / 1000);

		WARN_ON(strlen(log_buf) >= LOG_BUF_SIZE);
		printk_deferred("[name:spm&][SPM] %s", log_buf);
	}
}

static inline int mt6877_suspend_common_enter(unsigned int *susp_status)
{
	unsigned int status = PLAT_VCORE_LP_MODE
				| PLAT_PMIC_VCORE_SRCLKEN0
				| PLAT_SUSPEND;

	/* maybe need to stop sspm/mcupm mcdi task here */
	if (susp_status)
		*susp_status = status;

	return 0;
}


static inline int mt6877_suspend_common_resume(unsigned int susp_status)
{
	/* Implement suspend common flow here */
	return 0;
}

static int __mt6877_suspend_prompt(int type, int cpu,
				   const struct mtk_lpm_issuer *issuer)
{
	int ret = 0;
	unsigned int spm_res = 0;

	mt6877_suspend_status = 0;

	printk_deferred("[name:spm&][%s:%d] - prepare suspend enter\n",
			__func__, __LINE__);

	ret = mt6877_suspend_common_enter(&mt6877_suspend_status);

	if (ret)
		goto PLAT_LEAVE_SUSPEND;

	/* Legacy SSPM flow, spm sw resource request flow */
	mt6877_do_mcusys_prepare_pdn(mt6877_suspend_status, &spm_res);

	printk_deferred("[name:spm&][%s:%d] - suspend enter\n",
			__func__, __LINE__);

	/* Record md sleep time */
	get_md_sleep_time(&before_md_sleep_status);


PLAT_LEAVE_SUSPEND:
	return ret;
}

static void __mt6877_suspend_reflect(int type, int cpu,
					const struct mtk_lpm_issuer *issuer)
{
	printk_deferred("[name:spm&][%s:%d] - prepare suspend resume\n",
			__func__, __LINE__);

	mt6877_suspend_common_resume(mt6877_suspend_status);
	mt6877_do_mcusys_prepare_on();

	printk_deferred("[name:spm&][%s:%d] - resume\n",
			__func__, __LINE__);

	/* do not call issuer when prepare fail */
	if (mt6877_model_suspend.flag & MTK_LP_PREPARE_FAIL)
		return;

	if (issuer)
		issuer->log(MT_LPM_ISSUER_SUSPEND, "suspend", NULL);

	/* show md sleep duration during AP suspend */
	get_md_sleep_time(&after_md_sleep_status);
	log_md_sleep_info();
}
int mt6877_suspend_system_prompt(int cpu,
					const struct mtk_lpm_issuer *issuer)
{
	return __mt6877_suspend_prompt(MTK_LPM_SUSPEND_S2IDLE,
				       cpu, issuer);
}

void mt6877_suspend_system_reflect(int cpu,
					const struct mtk_lpm_issuer *issuer)
{
	return __mt6877_suspend_reflect(MTK_LPM_SUSPEND_S2IDLE,
					cpu, issuer);
}

int mt6877_suspend_s2idle_prompt(int cpu,
					const struct mtk_lpm_issuer *issuer)
{
	int ret = 0;

	cpumask_set_cpu(cpu, &s2idle_cpumask);
	if (cpumask_weight(&s2idle_cpumask) == num_online_cpus()) {

#ifdef CONFIG_PM_SLEEP
		ct_irq_enter_irqson();
		ret = syscore_suspend();
		ct_irq_exit_irqson();
#endif
		if (ret < 0) {
			mt6877_model_suspend.flag |= MTK_LP_PREPARE_FAIL;
			return ret;
		}

		ret = __mt6877_suspend_prompt(MTK_LPM_SUSPEND_S2IDLE,
					      cpu, issuer);
	}
	return ret;
}

int mt6877_suspend_s2idle_prepare_enter(int prompt, int cpu,
					const struct mtk_lpm_issuer *issuer)
{
	int ret = 0;

	if (mt6877_model_suspend.flag & MTK_LP_PREPARE_FAIL)
		ret = -1;

	return ret;
}
void mt6877_suspend_s2idle_reflect(int cpu,
					const struct mtk_lpm_issuer *issuer)
{
	if (cpumask_weight(&s2idle_cpumask) == num_online_cpus()) {
		__mt6877_suspend_reflect(MTK_LPM_SUSPEND_S2IDLE,
					 cpu, issuer);


#ifdef CONFIG_PM_SLEEP
		if (!(mt6877_model_suspend.flag & MTK_LP_PREPARE_FAIL)) {
			ct_irq_enter_irqson();
			syscore_resume();
			ct_irq_exit_irqson();
		}

		if (mt6877_model_suspend.flag & MTK_LP_PREPARE_FAIL)
			mt6877_model_suspend.flag &= (~MTK_LP_PREPARE_FAIL);
#endif
	}
	cpumask_clear_cpu(cpu, &s2idle_cpumask);
}

#define MT6877_SUSPEND_OP_INIT(_prompt, _enter, _resume, _reflect) ({\
	mt6877_model_suspend.op.prompt = _prompt;\
	mt6877_model_suspend.op.prepare_enter = _enter;\
	mt6877_model_suspend.op.prepare_resume = _resume;\
	mt6877_model_suspend.op.reflect = _reflect; })


struct mtk_lpm_model mt6877_model_suspend = {
	.flag = MTK_LP_REQ_NONE,
	.op = {
		.prompt = mt6877_suspend_system_prompt,
		.reflect = mt6877_suspend_system_reflect,
	}
};

#ifdef CONFIG_PM
static int mt6877_spm_suspend_pm_event(struct notifier_block *notifier,
			unsigned long pm_event, void *unused)
{
	struct rtc_time tm;
	struct timespec64 tv = { 0 };
	/* android time */
	struct rtc_time tm_android;
	struct timespec64 tv_android = { 0 };

	ktime_get_real_ts64(&tv);
	tv_android = tv;
	rtc_time64_to_tm(tv.tv_sec, &tm);
	tv_android.tv_sec -= (uint64_t)sys_tz.tz_minuteswest * 60;
	rtc_time64_to_tm(tv_android.tv_sec, &tm_android);

	switch (pm_event) {
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
	case PM_POST_HIBERNATION:
		return NOTIFY_DONE;
	case PM_SUSPEND_PREPARE:
		/* Allow the s2idle cpuidle state for the suspend window. */
		mtk_s2idle_state_enable(1);
		pr_info("[name:spm&][SPM] suspend start %d-%02d-%02d %02d:%02d:%02d.%u UTC;"
			"android time %d-%02d-%02d %02d:%02d:%02d.%03d\n",
			tm.tm_year + 1900, tm.tm_mon + 1,
			tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
			(unsigned int)(tv.tv_nsec / 1000), tm_android.tm_year + 1900,
			tm_android.tm_mon + 1, tm_android.tm_mday, tm_android.tm_hour,
			tm_android.tm_min, tm_android.tm_sec,
			(unsigned int)(tv_android.tv_nsec / 1000));
		cpu_hotplug_disable();
		return NOTIFY_DONE;
	case PM_POST_SUSPEND:
		pr_info("[name:spm&][SPM] suspend end %d-%02d-%02d %02d:%02d:%02d.%u UTC;"
			"android time %d-%02d-%02d %02d:%02d:%02d.%03d\n",
			tm.tm_year + 1900, tm.tm_mon + 1,
			tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
			(unsigned int)(tv.tv_nsec / 1000), tm_android.tm_year + 1900,
			tm_android.tm_mon + 1, tm_android.tm_mday, tm_android.tm_hour,
			tm_android.tm_min, tm_android.tm_sec,
			(unsigned int)(tv_android.tv_nsec / 1000));
		cpu_hotplug_enable();
		/* Keep s2idle unavailable during normal runtime idle selection. */
		mtk_s2idle_state_enable(0);
		return NOTIFY_DONE;
	}
	return NOTIFY_OK;
}

static struct notifier_block mt6877_spm_suspend_pm_notifier_func = {
	.notifier_call = mt6877_spm_suspend_pm_event,
	.priority = 0,
};

#endif /* CONFIG_PM */

int __init mt6877_model_suspend_init(void)
{
	int ret;

	int suspend_type = mtk_lpm_suspend_type_get();

	if (suspend_type == MTK_LPM_SUSPEND_S2IDLE) {
		MT6877_SUSPEND_OP_INIT(mt6877_suspend_s2idle_prompt,
					mt6877_suspend_s2idle_prepare_enter,
					NULL,
					mt6877_suspend_s2idle_reflect);
		ret = mtk_lpm_suspend_registry("s2idle",
					       &mt6877_model_suspend);
		if (ret)
			return ret;
		mtk_s2idle_state_enable(0);
	} else {
		MT6877_SUSPEND_OP_INIT(mt6877_suspend_system_prompt,
					NULL,
					NULL,
					mt6877_suspend_system_reflect);
		ret = mtk_lpm_suspend_registry("suspend",
					       &mt6877_model_suspend);
		if (ret)
			return ret;
	}

	cpumask_clear(&s2idle_cpumask);

#ifdef CONFIG_PM
	ret = register_pm_notifier(&mt6877_spm_suspend_pm_notifier_func);
	if (ret) {
		pr_debug("[name:spm&][SPM] Failed to register PM notifier.\n");
		return ret;
	}
#endif /* CONFIG_PM */

#ifdef CONFIG_PM_SLEEP_DEBUG
	pm_print_times_enabled = false;
#endif
	return 0;
}
