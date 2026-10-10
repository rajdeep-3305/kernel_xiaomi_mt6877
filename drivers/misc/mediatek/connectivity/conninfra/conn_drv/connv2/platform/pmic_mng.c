// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/regmap.h>

#include "include/pmic_mng.h"
#include "osal.h"
#if COMMON_KERNEL_PMIC_SUPPORT
#include <linux/regulator/consumer.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#endif

/*******************************************************************************
*                         C O M P I L E R   F L A G S
********************************************************************************
*/

/*******************************************************************************
*                                 M A C R O S
********************************************************************************
*/


/*******************************************************************************
*                    E X T E R N A L   R E F E R E N C E S
********************************************************************************
*/

/*******************************************************************************
*                              C O N S T A N T S
********************************************************************************
*/

/*******************************************************************************
*                             D A T A   T Y P E S
********************************************************************************
*/

/*******************************************************************************
*                  F U N C T I O N   D E C L A R A T I O N S
********************************************************************************
*/

#if COMMON_KERNEL_PMIC_SUPPORT
static int consys_mt6363_probe(struct platform_device *pdev);
static int consys_mt6373_probe(struct platform_device *pdev);
static int consys_mt6368_probe(struct platform_device *pdev);
static int consys_mt6369_probe(struct platform_device *pdev);
#endif

static int pmic_mng_register_device(void);
static int pmic_mng_unregister_device(void);

/*******************************************************************************
*                            P U B L I C   D A T A
********************************************************************************
*/

const struct consys_platform_pmic_ops* consys_platform_pmic_ops = NULL;
#if COMMON_KERNEL_PMIC_SUPPORT
struct regmap *g_regmap;
struct regmap *g_regmap_mt6363;
struct regmap *g_regmap_mt6373;
struct regmap *g_regmap_mt6368;
struct regmap *g_regmap_mt6369;
#endif

/*******************************************************************************
*                           P R I V A T E   D A T A
********************************************************************************
*/

#if COMMON_KERNEL_PMIC_SUPPORT
#ifdef CONFIG_OF
const struct of_device_id consys_pmic_mt6363_of_ids[] = {
	{.compatible = "mediatek,mt6363-consys",},
	{}
};
const struct of_device_id consys_pmic_mt6373_of_ids[] = {
	{.compatible = "mediatek,mt6373-consys",},
	{}
};
const struct of_device_id consys_pmic_mt6368_of_ids[] = {
	{.compatible = "mediatek,mt6368-consys",},
	{}
};
const struct of_device_id consys_pmic_mt6369_of_ids[] = {
	{.compatible = "mediatek,mt6369-consys",},
	{}
};
#endif

static struct platform_driver consys_mt6363_dev_drv = {
	.probe = consys_mt6363_probe,
	.driver = {
		.name = "mt6363-consys",
#ifdef CONFIG_OF
		.of_match_table = consys_pmic_mt6363_of_ids,
#endif
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
		},
};
static struct platform_driver consys_mt6373_dev_drv = {
	.probe = consys_mt6373_probe,
	.driver = {
		.name = "mt6373-consys",
#ifdef CONFIG_OF
		.of_match_table = consys_pmic_mt6373_of_ids,
#endif
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
		},
};
static struct platform_driver consys_mt6368_dev_drv = {
	.probe = consys_mt6368_probe,
	.driver = {
		.name = "mt6368-consys",
#ifdef CONFIG_OF
		.of_match_table = consys_pmic_mt6368_of_ids,
#endif
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
		},
};
static struct platform_driver consys_mt6369_dev_drv = {
	.probe = consys_mt6369_probe,
	.driver = {
		.name = "mt6369-consys",
#ifdef CONFIG_OF
		.of_match_table = consys_pmic_mt6369_of_ids,
#endif
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
		},
};
#endif

/*******************************************************************************
*                              F U N C T I O N S
********************************************************************************
*/

#if COMMON_KERNEL_PMIC_SUPPORT
static int consys_mt6363_probe(struct platform_device *pdev)
{
	g_regmap_mt6363 = dev_get_regmap(pdev->dev.parent, NULL);

	if (!g_regmap_mt6363)
		pr_info("%s failed to get g_regmap_mt6363\n", __func__);
	else
		pr_info("%s get regmap_mt6363 success!!\n", __func__);

	return 0;
}

static int consys_mt6373_probe(struct platform_device *pdev)
{
	g_regmap_mt6373 = dev_get_regmap(pdev->dev.parent, NULL);

	if (!g_regmap_mt6373)
		pr_info("%s failed to get g_regmap_mt6373\n", __func__);
	else
		pr_info("%s get regmap_mt6373 success!!\n", __func__);

	return 0;
}

static int consys_mt6368_probe(struct platform_device *pdev)
{
	g_regmap_mt6368 = dev_get_regmap(pdev->dev.parent, NULL);

	if (!g_regmap_mt6368)
		pr_info("%s failed to get g_regmap_mt6368\n", __func__);
	else
		pr_info("%s get regmap_mt6368 success!!\n", __func__);

	return 0;
}

static int consys_mt6369_probe(struct platform_device *pdev)
{
	g_regmap_mt6369 = dev_get_regmap(pdev->dev.parent, NULL);

	if (!g_regmap_mt6369)
		pr_info("%s failed to get g_regmap_mt6369\n", __func__);
	else
		pr_info("%s get regmap_mt6369 success!!\n", __func__);

	return 0;
}

static int pmic_mng_get_regmap(struct platform_device *pdev)
{
	struct device_node *pmic_node;
	struct platform_device *pmic_pdev;
	struct mt6397_chip *chip;
	int ret = 0;

	pmic_node = of_parse_phandle(pdev->dev.of_node, "pmic", 0);
	if (!pmic_node) {
		dev_err(&pdev->dev, "failed to get PMIC node\n");
		return -ENODEV;
	}

	pmic_pdev = of_find_device_by_node(pmic_node);
	of_node_put(pmic_node);
	if (!pmic_pdev) {
		dev_err(&pdev->dev, "failed to get PMIC device\n");
		return -EPROBE_DEFER;
	}

	chip = dev_get_drvdata(&(pmic_pdev->dev));
	if (!chip) {
		dev_err(&pdev->dev, "failed to get PMIC driver data\n");
		ret = -EPROBE_DEFER;
		goto out_put_device;
	}

	g_regmap = chip->regmap;
	if (IS_ERR_OR_NULL(g_regmap)) {
		ret = g_regmap ? PTR_ERR(g_regmap) : -ENODEV;
		g_regmap = NULL;
		dev_err(&pdev->dev, "failed to get PMIC regmap: %d\n", ret);
	}

out_put_device:
	put_device(&pmic_pdev->dev);
	return ret;
}
#endif

int pmic_mng_init(
	struct platform_device *pdev,
	struct conninfra_dev_cb* dev_cb,
	const struct conninfra_plat_data* plat_data)
{
#if COMMON_KERNEL_PMIC_SUPPORT
	int ret;

	ret = pmic_mng_get_regmap(pdev);
	if (ret)
		return ret;
#endif

	if (consys_platform_pmic_ops == NULL)
		consys_platform_pmic_ops =
			(const struct consys_platform_pmic_ops*)plat_data->platform_pmic_ops;

	if (consys_platform_pmic_ops && consys_platform_pmic_ops->consys_pmic_get_from_dts)
		consys_platform_pmic_ops->consys_pmic_get_from_dts(pdev, dev_cb);

	pmic_mng_register_device();

	return 0;
}

int pmic_mng_deinit(void)
{
	pmic_mng_unregister_device();
#if COMMON_KERNEL_PMIC_SUPPORT
	g_regmap = NULL;
#endif
	consys_platform_pmic_ops = NULL;
	return 0;
}

int pmic_mng_common_power_ctrl(unsigned int enable, unsigned int curr_status,
					unsigned int next_status)
{
	int ret = 0;
	if (consys_platform_pmic_ops &&
		consys_platform_pmic_ops->consys_pmic_common_power_ctrl)
		ret = consys_platform_pmic_ops->consys_pmic_common_power_ctrl(enable,
					curr_status, next_status);
	return ret;
}

int pmic_mng_common_power_low_power_mode(unsigned int enable, unsigned int curr_status,
					unsigned int next_status)
{
	int ret = 0;
	if (consys_platform_pmic_ops &&
		consys_platform_pmic_ops->consys_pmic_common_power_low_power_mode)
		ret = consys_platform_pmic_ops->consys_pmic_common_power_low_power_mode(enable,
					curr_status, next_status);
	return ret;
}

int pmic_mng_wifi_power_ctrl(unsigned int enable)
{
	int ret = 0;
	if (consys_platform_pmic_ops &&
		consys_platform_pmic_ops->consys_pmic_wifi_power_ctrl)
		ret = consys_platform_pmic_ops->consys_pmic_wifi_power_ctrl(enable);
	return ret;

}

int pmic_mng_bt_power_ctrl(unsigned int enable)
{
	int ret = 0;
	if (consys_platform_pmic_ops &&
		consys_platform_pmic_ops->consys_pmic_bt_power_ctrl)
		ret = consys_platform_pmic_ops->consys_pmic_bt_power_ctrl(enable);
	return ret;
}

int pmic_mng_gps_power_ctrl(unsigned int enable)
{
	int ret = 0;
	if (consys_platform_pmic_ops &&
		consys_platform_pmic_ops->consys_pmic_gps_power_ctrl)
		ret = consys_platform_pmic_ops->consys_pmic_gps_power_ctrl(enable);
	return ret;
}

int pmic_mng_fm_power_ctrl(unsigned int enable)
{
	int ret = 0;
	if (consys_platform_pmic_ops &&
		consys_platform_pmic_ops->consys_pmic_fm_power_ctrl)
		ret = consys_platform_pmic_ops->consys_pmic_fm_power_ctrl(enable);
	return ret;
}


int pmic_mng_event_cb(unsigned int id, unsigned int event)
{
	if (consys_platform_pmic_ops &&
		consys_platform_pmic_ops->consys_pmic_event_notifier)
		consys_platform_pmic_ops->consys_pmic_event_notifier(id, event);
	return 0;
}

int pmic_mng_raise_voltage(unsigned int drv_type, bool raise, bool onoff)
{
	int ret = 0;

	if (consys_platform_pmic_ops &&
		consys_platform_pmic_ops->consys_pmic_raise_voltage)
		ret = consys_platform_pmic_ops->consys_pmic_raise_voltage(drv_type, raise, onoff);
	return ret;
}

bool pmic_mng_is_support_raise_voltage(void)
{
	if (consys_platform_pmic_ops &&
		consys_platform_pmic_ops->consys_pmic_raise_voltage)
		return true;
	return false;
}

int pmic_mng_register_device(void)
{
#if COMMON_KERNEL_PMIC_SUPPORT
	int ret;

	ret = platform_driver_register(&consys_mt6363_dev_drv);
	if (ret)
		pr_err("Conninfra pmic mt6363 driver registered failed(%d)\n", ret);
	else
		pr_info("%s mt6363 ok.\n", __func__);

	ret = platform_driver_register(&consys_mt6373_dev_drv);
	if (ret)
		pr_err("Conninfra pmic mt6373 driver registered failed(%d)\n", ret);
	else
		pr_info("%s mt6373 ok.\n", __func__);

	ret = platform_driver_register(&consys_mt6368_dev_drv);
	if (ret)
		pr_err("Conninfra pmic mt6368 driver registered failed(%d)\n", ret);
	else
		pr_info("%s mt6368 ok.\n", __func__);

	ret = platform_driver_register(&consys_mt6369_dev_drv);
	if (ret)
		pr_err("Conninfra pmic mt6369 driver registered failed(%d)\n", ret);
	else
		pr_info("%s mt6369 ok.\n", __func__);

#endif
	return 0;
}

int pmic_mng_unregister_device(void)
{
#if COMMON_KERNEL_PMIC_SUPPORT
	if (g_regmap_mt6368 != NULL) {
		platform_driver_unregister(&consys_mt6368_dev_drv);
		g_regmap_mt6368 = NULL;
	}
	if (g_regmap_mt6373 != NULL) {
		platform_driver_unregister(&consys_mt6373_dev_drv);
		g_regmap_mt6373 = NULL;
	}
	if (g_regmap_mt6363 != NULL) {
		platform_driver_unregister(&consys_mt6363_dev_drv);
		g_regmap_mt6363 = NULL;
	}
	if (g_regmap_mt6369 != NULL) {
		platform_driver_unregister(&consys_mt6369_dev_drv);
		g_regmap_mt6369 = NULL;
	}
#endif
	return 0;
}

int pmic_mng_common_power_leave_low_power_mode(void)
{
	if (consys_platform_pmic_ops->consys_pmic_leave_low_power_mode)
		consys_platform_pmic_ops->consys_pmic_leave_low_power_mode();
	return 0;
}
