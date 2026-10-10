// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/kthread.h>

#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
#include <sspm_helper.h>
#include <sspm_mbox.h>
#endif

#include <mtk_lp_plat_apmcu_mbox.h>
#ifdef CONFIG_MTK_TINYSYS_MCUPM_SUPPORT
#include "mcupm_driver.h"
#include "mcupm_ipi_id.h"
#endif

struct mbox_ops {
	int (*write)(int id, int *buf, unsigned int len);
	int (*read)(int id, int *buf, unsigned int len);
};

static int apmcu_sspm_mailbox_write(int id, int *buf, unsigned int len)
{
#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
//	if (is_sspm_ready())
//		sspm_mbox_write(APMCU_SSPM_MBOX_ID, id, (void *)buf, len);
#endif
	return -EOPNOTSUPP;
}

static int apmcu_sspm_mailbox_read(int id, int *buf, unsigned int len)
{
#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
//	if (is_sspm_ready())
//		sspm_mbox_read(APMCU_SSPM_MBOX_ID, id, (void *)&buf, len);
#endif
	return -EOPNOTSUPP;
}

static int apmcu_mcupm_mailbox_write(int id, int *buf, unsigned int len)
{
#ifdef CONFIG_MTK_TINYSYS_MCUPM_SUPPORT
	int ret;

	if (!mcupm_ipi_ready())
		return -EPROBE_DEFER;
	ret = mcupm_mbox_write(APMCU_MCUPM_MBOX_ID, id, (void *)buf, len);
	return ret > 0 ? -EIO : ret;
#else
	return -EOPNOTSUPP;
#endif
}

static int apmcu_mcupm_mailbox_read(int id, int *buf, unsigned int len)
{
#ifdef CONFIG_MTK_TINYSYS_MCUPM_SUPPORT
	int ret;

	if (!mcupm_ipi_ready())
		return -EPROBE_DEFER;
	ret = mcupm_mbox_read(APMCU_MCUPM_MBOX_ID, id, (void *)buf, len);
	return ret > 0 ? -EIO : ret;
#else
	return -EOPNOTSUPP;
#endif
}

static struct mbox_ops mbox[NF_MBOX] = {
	[MBOX_SSPM] = {
		.write = apmcu_sspm_mailbox_write,
		.read = apmcu_sspm_mailbox_read
	},
	[MBOX_MCUPM] = {
		.write = apmcu_mcupm_mailbox_write,
		.read = apmcu_mcupm_mailbox_read
	},
};


void mtk_set_sspm_lp_cmd(void *buf)
{
	mbox[MBOX_SSPM].write(APMCU_SSPM_MBOX_SPM_CMD,
			(int *)buf,
			APMCU_SSPM_MBOX_SPM_CMD_SIZE);
}

void mtk_clr_sspm_lp_cmd(void)
{
	int buf[APMCU_SSPM_MBOX_SPM_CMD_SIZE] = {0};

	mbox[MBOX_SSPM].write(APMCU_SSPM_MBOX_SPM_CMD,
			(int *)buf,
			APMCU_SSPM_MBOX_SPM_CMD_SIZE);
}

bool mtk_mcupm_cm_is_notified(void)
{
	unsigned int en_mask = 0;

	mbox[MBOX_MCUPM].read(APMCU_MCUPM_MBOX_PWR_CTRL_EN, &en_mask, 1);

	return !!(en_mask & MCUPM_CM_CTRL);
}
EXPORT_SYMBOL(mtk_mcupm_cm_is_notified);


void mtk_set_mcupm_pll_mode(unsigned int mode)
{
	if (mode < NF_MCUPM_ARMPLL_MODE)
		mbox[MBOX_MCUPM].write(APMCU_MCUPM_MBOX_ARMPLL_MODE, &mode, 1);
}
EXPORT_SYMBOL(mtk_set_mcupm_pll_mode);

int mtk_get_mcupm_pll_mode(void)
{
	int mode = 0;

	mbox[MBOX_MCUPM].read(APMCU_MCUPM_MBOX_ARMPLL_MODE, &mode, 1);

	return mode;
}
EXPORT_SYMBOL(mtk_get_mcupm_pll_mode);

void mtk_set_mcupm_buck_mode(unsigned int mode)
{
	if (mode < NF_MCUPM_BUCK_MODE)
		mbox[MBOX_MCUPM].write(APMCU_MCUPM_MBOX_BUCK_MODE, &mode, 1);
}
EXPORT_SYMBOL(mtk_set_mcupm_buck_mode);

int mtk_get_mcupm_buck_mode(void)
{
	int mode = 0;

	mbox[MBOX_MCUPM].read(APMCU_MCUPM_MBOX_BUCK_MODE, &mode, 1);

	return mode;
}
EXPORT_SYMBOL(mtk_get_mcupm_buck_mode);

void mtk_set_preferred_cpu_wakeup(int cpu)
{
	if (cpu_online(cpu))
		mbox[MBOX_MCUPM].write(APMCU_MCUPM_MBOX_WAKEUP_CPU, &cpu, 1);
}

int mtk_get_preferred_cpu_wakeup(void)
{
	int cpu = -1;

	mbox[MBOX_MCUPM].read(APMCU_MCUPM_MBOX_WAKEUP_CPU, &cpu, 1);

	return cpu;
}

bool mtk_mcupm_is_ready(void)
{
	int sta = MCUPM_TASK_UNINIT;

	if (mbox[MBOX_MCUPM].read(APMCU_MCUPM_MBOX_TASK_STA, &sta, 1))
		return false;

	return sta == MCUPM_TASK_WAIT || sta == MCUPM_TASK_INIT_FINISH;
}

int mtk_wait_mbox_init_done(void)
{
	int sta = MCUPM_TASK_UNINIT;
	int ret, value;

	/* Leave CPU-off blocked if the provider or firmware never initializes. */
	for (;;) {
		if (kthread_should_stop())
			return -EINTR;
		ret = mbox[MBOX_MCUPM].read(APMCU_MCUPM_MBOX_TASK_STA,
					  &sta, 1);
		if (ret && ret != -EPROBE_DEFER)
			return ret;

		if (!ret && sta == MCUPM_TASK_INIT)
			break;

		msleep(1000);
	}

	value = MCUPM_ARMPLL_OFF;
	ret = mbox[MBOX_MCUPM].write(APMCU_MCUPM_MBOX_ARMPLL_MODE,
					   &value, 1);
	if (ret)
		return ret;
	value = MCUPM_BUCK_OFF_MODE;
	ret = mbox[MBOX_MCUPM].write(APMCU_MCUPM_MBOX_BUCK_MODE,
					   &value, 1);
	if (ret)
		return ret;

	value = MCUPM_MCUSYS_CTRL |
#ifdef CONFIG_MTK_CM_MGR_LEGACY
			 MCUPM_CM_CTRL |
#endif
			 MCUPM_BUCK_CTRL |
			 MCUPM_ARMPLL_CTRL;
	return mbox[MBOX_MCUPM].write(APMCU_MCUPM_MBOX_PWR_CTRL_EN,
					    &value, 1);
}

int mtk_notify_subsys_ap_ready(void)
{
	/* Preserve the MT6360 firmware token with either MFD implementation. */
#if IS_ENABLED(CONFIG_MFD_MT6360_PMU) || IS_ENABLED(CONFIG_MFD_MT6360)
	int ready = 0x6360;
#else
	int ready = 1;
#endif

	mbox[MBOX_SSPM].write(APMCU_SSPM_MBOX_AP_READY, &ready, 1);
	return mbox[MBOX_MCUPM].write(APMCU_MCUPM_MBOX_AP_READY, &ready, 1);
}
