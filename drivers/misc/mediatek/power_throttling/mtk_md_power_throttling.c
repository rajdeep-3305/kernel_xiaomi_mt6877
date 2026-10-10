// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/module.h>

#include <mtk_ccci_common.h>

#include "mtk_battery_oc_throttling.h"
#include "mtk_battery_percentage_throttling.h"
#include "mtk_low_battery_throttling.h"
#include "mtk_md_power_throttling.h"

static int md_send_throttle_cmd(u32 command)
{
#if IS_ENABLED(CONFIG_MTK_ECCCI_DRIVER)
	return exec_ccci_kern_func_by_md_id(MD_SYS1, ID_THROTTLING_CFG,
			(char *)&command, sizeof(command));
#else
	return -ENODEV;
#endif
}

#if IS_ENABLED(CONFIG_MTK_LOW_BATTERY_POWER_THROTTLING)
static void md_low_battery_cb(enum LOW_BATTERY_LEVEL_TAG level)
{
	u32 intensity, command;
	int ret;

	if (level < LOW_BATTERY_LEVEL_0 || level > LOW_BATTERY_LEVEL_2)
		return;

	intensity = level == LOW_BATTERY_LEVEL_0 ? 0 : LBAT_REDUCE_TX_POWER;
	command = TMC_CTRL_CMD_TX_POWER | level << 8 |
		PT_LOW_BATTERY_VOLTAGE << 16 | intensity << 24;
	ret = md_send_throttle_cmd(command);
	if (ret)
		pr_notice("%s: ret=%d cmd=0x%x level=%d\n",
			__func__, ret, command, level);
}
#endif

#if IS_ENABLED(CONFIG_MTK_BATTERY_OC_POWER_THROTTLING)
static void md_over_current_cb(enum BATTERY_OC_LEVEL_TAG level)
{
	u32 intensity, command;
	int ret;

	if (level < BATTERY_OC_LEVEL_0 || level > BATTERY_OC_LEVEL_1)
		return;

	intensity = level == BATTERY_OC_LEVEL_0 ? 0 : OC_REDUCE_TX_POWER;
	command = TMC_CTRL_CMD_TX_POWER | level << 8 |
		PT_OVER_CURRENT << 16 | intensity << 24;
	ret = md_send_throttle_cmd(command);
	if (ret)
		pr_notice("%s: ret=%d cmd=0x%x level=%d\n",
			__func__, ret, command, level);
}
#endif

#if IS_ENABLED(CONFIG_MTK_BATTERY_PERCENTAGE_POWER_THROTTLING)
static void md_battery_percent_cb(enum BATTERY_PERCENT_LEVEL_TAG level)
{
	u32 value, command;
	int ret;

	if (level < BATTERY_PERCENT_LEVEL_0 || level > BATTERY_PERCENT_LEVEL_1)
		return;

	value = level == BATTERY_PERCENT_LEVEL_0 ?
		TMC_CTRL_LOW_POWER_RECHARGE_BATTERY_EVENT :
		TMC_CTRL_LOW_POWER_LOW_BATTERY_EVENT;
	command = TMC_CTRL_CMD_LOW_POWER_IND | value << 8;
	ret = md_send_throttle_cmd(command);
	if (ret)
		pr_notice("%s: ret=%d cmd=0x%x level=%d\n",
			__func__, ret, command, level);
}
#endif

static int __init mtk_md_power_throttling_init(void)
{
#if IS_ENABLED(CONFIG_MTK_LOW_BATTERY_POWER_THROTTLING)
	register_low_battery_notify(md_low_battery_cb, LOW_BATTERY_PRIO_MD);
#endif
#if IS_ENABLED(CONFIG_MTK_BATTERY_OC_POWER_THROTTLING)
	register_battery_oc_notify(md_over_current_cb, BATTERY_OC_PRIO_MD);
#endif
#if IS_ENABLED(CONFIG_MTK_BATTERY_PERCENTAGE_POWER_THROTTLING)
	register_battery_percent_notify_ext(md_battery_percent_cb,
			BATTERY_PERCENT_PRIO_MD);
#endif
	return 0;
}
module_init(mtk_md_power_throttling_init);

MODULE_DESCRIPTION("MediaTek modem power throttling");
MODULE_LICENSE("GPL");
