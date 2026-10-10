/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
*/

#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk-pm-qos.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/sizes.h>
#include <linux/of.h>

#include <spm/mtk_spm.h>


#if defined(CONFIG_MTK_QOS_V2)
#include <mtk_qos_ipi.h>
#endif

#include <helio-dvfsrc-qos.h>
#ifdef CONFIG_MACH_MT6877
#include <helio-dvfsrc-opp.h>
#include <helio-dvfsrc-ip-v2.h>
#include <linux/soc/mediatek/mt6877-dvfsrc.h>
#endif

/* Serialize publication/removal and PM QoS replay against notifier updates. */
static struct helio_dvfsrc *dvfsrc;
static DEFINE_MUTEX(dvfsrc_probe_lock);
static DEFINE_MUTEX(dvfsrc_lock);
/* MMIO users include atomic CM-manager and IRQ/debug callers. */
static DEFINE_SPINLOCK(dvfsrc_mmio_lock);

static const int dvfsrc_qos_classes[DVFSRC_QOS_NUM_CLASSES] = {
	[DVFSRC_QOS_MEMORY_BANDWIDTH] = MTK_PM_QOS_MEMORY_BANDWIDTH_TEST,
	[DVFSRC_QOS_CPU_MEMORY_BANDWIDTH] = MTK_PM_QOS_CPU_MEMORY_BANDWIDTH,
	[DVFSRC_QOS_GPU_MEMORY_BANDWIDTH] = MTK_PM_QOS_GPU_MEMORY_BANDWIDTH,
	[DVFSRC_QOS_MM_MEMORY_BANDWIDTH] = PM_QOS_MM_MEMORY_BANDWIDTH,
	[DVFSRC_QOS_OTHER_MEMORY_BANDWIDTH] = MTK_PM_QOS_OTHER_MEMORY_BANDWIDTH,
	[DVFSRC_QOS_DDR_OPP] = MTK_PM_QOS_DDR_OPP,
	[DVFSRC_QOS_VCORE_OPP] = MTK_PM_QOS_VCORE_OPP,
	[DVFSRC_QOS_SCP_VCORE_REQUEST] = MTK_PM_QOS_SCP_VCORE_REQUEST,
	[DVFSRC_QOS_POWER_MODEL_DDR_REQUEST] = MTK_PM_QOS_POWER_MODEL_DDR_REQUEST,
	[DVFSRC_QOS_POWER_MODEL_VCORE_REQUEST] = MTK_PM_QOS_POWER_MODEL_VCORE_REQUEST,
	[DVFSRC_QOS_VCORE_DVFS_FORCE_OPP] = MTK_PM_QOS_VCORE_DVFS_FORCE_OPP,
	[DVFSRC_QOS_ISP_HRT_BANDWIDTH] = PM_QOS_ISP_HRT_BANDWIDTH,
	[DVFSRC_QOS_APU_MEMORY_BANDWIDTH] = PM_QOS_APU_MEMORY_BANDWIDTH,
};

#if defined(DVFSRC_SMC_CONTROL)
u32 spm_reg_read(u32 offset)
{
	unsigned long flags;
	u32 value = 0;

	spin_lock_irqsave(&dvfsrc_mmio_lock, flags);
	if (dvfsrc && READ_ONCE(dvfsrc->dvfsrc_enabled))
		value = readl(dvfsrc->spm_regs + offset);
	spin_unlock_irqrestore(&dvfsrc_mmio_lock, flags);
	return value;
}
#endif

u32 dvfsrc_read(u32 offset)
{
	unsigned long flags;
	u32 value = 0;

	spin_lock_irqsave(&dvfsrc_mmio_lock, flags);
	if (dvfsrc && READ_ONCE(dvfsrc->dvfsrc_enabled))
		value = readl(dvfsrc->regs + offset);
	spin_unlock_irqrestore(&dvfsrc_mmio_lock, flags);
	return value;
}

void dvfsrc_write(u32 offset, u32 val)
{
	unsigned long flags;

	spin_lock_irqsave(&dvfsrc_mmio_lock, flags);
	if (dvfsrc && READ_ONCE(dvfsrc->dvfsrc_enabled))
		writel(val, dvfsrc->regs + offset);
	spin_unlock_irqrestore(&dvfsrc_mmio_lock, flags);
}

static int dvfsrc_restore(void)
{
	int type, ret;

	lockdep_assert_held(&dvfsrc_lock);
	for (type = 1; type < DVFSRC_QOS_NUM_CLASSES; type++) {
		ret = commit_data(type,
				mtk_pm_qos_request(dvfsrc_qos_classes[type]), 0);
		if (ret < 0)
			return ret;
	}
	return 0;
}

#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
#if defined(CONFIG_MTK_QOS_V2)
static void helio_dvfsrc_sspm_init(int dvfsrc_en)
{
	struct qos_ipi_data qos_d;

	qos_d.cmd = QOS_IPI_DVFSRC_ENABLE;
	qos_d.u.dvfsrc_enable.dvfsrc_en = dvfsrc_en;
	qos_ipi_to_sspm_command(&qos_d, 2);
}
#endif
#endif

static int helio_dvfsrc_enable_locked(int enable)
{
	int command, ret;

	lockdep_assert_held(&dvfsrc_lock);
	if (enable < 0 || enable > 1)
		return -EINVAL;
	if (!dvfsrc)
		return -ENODEV;
	if (READ_ONCE(dvfsrc->dvfsrc_enabled) == enable)
		return 0;
	command = enable ? enable | (dvfsrc->dvfsrc_flag << 1) : 0;
#ifdef DVFSRC_SMC_CONTROL
	ret = helio_dvfsrc_firmware_ready();
	if (ret)
		return ret;
#endif
	/* Preserve the reference's votes-before-SPM-enable ordering. */
	if (enable) {
		WRITE_ONCE(dvfsrc->dvfsrc_enabled, true);
		ret = dvfsrc_restore();
		if (ret)
			goto fail_enable;
	}
#ifdef DVFSRC_SMC_CONTROL
	ret = mtk_spmfw_init_checked(command, 1);
	if (ret)
		goto fail_enable;
#else
	mtk_spmfw_init(command, 1);
#endif
#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
#if defined(CONFIG_MTK_QOS_V2)
	helio_dvfsrc_sspm_init(enable);
#endif
#endif
	WRITE_ONCE(dvfsrc->dvfsrc_enabled, enable);
	WRITE_ONCE(dvfsrc->qos_enabled, false);
	dvfsrc->opp_forced = false;
	strscpy(dvfsrc->force_start, "0", sizeof(dvfsrc->force_start));
	strscpy(dvfsrc->force_end, "0", sizeof(dvfsrc->force_end));
	return 0;

fail_enable:
	if (enable)
		WRITE_ONCE(dvfsrc->dvfsrc_enabled, false);
	return ret;
}

int helio_dvfsrc_enable(int enable)
{
	int ret;

	mutex_lock(&dvfsrc_lock);
#ifdef CONFIG_MACH_MT6877
	/* Runtime consumers require this voltage/DDR backend to remain active. */
	if (enable != 1) {
		ret = -EOPNOTSUPP;
		goto out;
	}
#endif
	ret = helio_dvfsrc_enable_locked(enable);
	if (!ret && dvfsrc)
		WRITE_ONCE(dvfsrc->qos_enabled, !!enable);
#ifdef CONFIG_MACH_MT6877
out:
#endif
	mutex_unlock(&dvfsrc_lock);
	return ret;
}

void helio_dvfsrc_flag_set(int flag)
{
	mutex_lock(&dvfsrc_lock);
	if (dvfsrc && flag >= 0 && flag <= 3)
		WRITE_ONCE(dvfsrc->dvfsrc_flag, flag);
	mutex_unlock(&dvfsrc_lock);
}

int helio_dvfsrc_flag_get(void)
{
	unsigned long flags;
	int value = 0;

	spin_lock_irqsave(&dvfsrc_mmio_lock, flags);
	if (dvfsrc)
		value = READ_ONCE(dvfsrc->dvfsrc_flag);
	spin_unlock_irqrestore(&dvfsrc_mmio_lock, flags);
	return value;
}

static void helio_dvfsrc_common_init(void)
{
	dvfsrc_opp_level_mapping();
	dvfsrc_opp_table_init();
}

int is_dvfsrc_enabled(void)
{
	unsigned long flags;
	int enabled = 0;

	spin_lock_irqsave(&dvfsrc_mmio_lock, flags);
	if (dvfsrc)
		enabled = READ_ONCE(dvfsrc->dvfsrc_enabled);
	spin_unlock_irqrestore(&dvfsrc_mmio_lock, flags);
	return enabled;
}

int is_dvfsrc_qos_enabled(void)
{
	unsigned long flags;
	int enabled = 0;

	spin_lock_irqsave(&dvfsrc_mmio_lock, flags);
	if (dvfsrc)
		enabled = READ_ONCE(dvfsrc->qos_enabled);
	spin_unlock_irqrestore(&dvfsrc_mmio_lock, flags);
	return enabled;
}

#ifdef CONFIG_MACH_MT6877
int mt6877_dvfsrc_ready(void)
{
	if (!is_dvfsrc_qos_enabled())
		return -EPROBE_DEFER;
	return helio_dvfsrc_firmware_ready();
}
EXPORT_SYMBOL_GPL(mt6877_dvfsrc_ready);

int mt6877_dvfsrc_sync(void)
{
	int type, ret;

	mutex_lock(&dvfsrc_lock);
	ret = mt6877_dvfsrc_ready();
	if (ret)
		goto out;
	for (type = 1; type < DVFSRC_QOS_NUM_CLASSES; type++) {
		ret = commit_data(type,
			mtk_pm_qos_request(dvfsrc_qos_classes[type]), 1);
		if (ret)
			break;
	}
out:
	mutex_unlock(&dvfsrc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6877_dvfsrc_sync);

int mt6877_dvfsrc_wait_vcore(unsigned int opp)
{
	int ret;

	if (opp >= VCORE_OPP_NUM)
		return -EINVAL;
	mutex_lock(&dvfsrc_lock);
	ret = mt6877_dvfsrc_ready();
	if (ret)
		goto out;
	/* Also retry a retained vote whose previous hardware commit failed. */
	ret = commit_data(DVFSRC_QOS_VCORE_OPP,
			  mtk_pm_qos_request(MTK_PM_QOS_VCORE_OPP), 1);
	if (ret)
		goto out;
	/* A forced OPP must not bypass voltage-before-clock validation. */
	ret = dvfsrc_wait_for_completion(
		!dvfsrc_read(DVFSRC_TARGET_LEVEL) && get_cur_vcore_opp() <= opp,
		DVFSRC_TIMEOUT);
	if (ret > 0)
		ret = 0;
out:
	mutex_unlock(&dvfsrc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6877_dvfsrc_wait_vcore);

void mt6877_dvfsrc_release_bootstrap(void)
{
	/* The clock consumer has established a validated frequency/voltage vote. */
	helio_dvfsrc_qos_init_done();
}
EXPORT_SYMBOL_GPL(mt6877_dvfsrc_release_bootstrap);
#endif

static void get_mtk_pm_qos_info(char *p)
{
	p += sprintf(p, "%-24s: %d\n",
			"MTK_PM_QOS_MEMORY_BW",
			mtk_pm_qos_request(MTK_PM_QOS_MEMORY_BANDWIDTH_TEST));
	p += sprintf(p, "%-24s: %d\n",
			"MTK_PM_QOS_CPU_MEMORY_BW",
			mtk_pm_qos_request(MTK_PM_QOS_CPU_MEMORY_BANDWIDTH));
	p += sprintf(p, "%-24s: %d\n",
			"MTK_PM_QOS_GPU_MEMORY_BW",
			mtk_pm_qos_request(MTK_PM_QOS_GPU_MEMORY_BANDWIDTH));
	p += sprintf(p, "%-24s: %d\n",
			"MTK_PM_QOS_MM_MEMORY_BW",
			mtk_pm_qos_request(PM_QOS_MM_MEMORY_BANDWIDTH));
	p += sprintf(p, "%-24s: %d\n",
			"MTK_PM_QOS_OTHER_MEMORY_BW",
			mtk_pm_qos_request(MTK_PM_QOS_OTHER_MEMORY_BANDWIDTH));
	p += sprintf(p, "%-24s: 0x%x\n",
			"MTK_PM_QOS_DDR_OPP",
			mtk_pm_qos_request(MTK_PM_QOS_DDR_OPP));
	p += sprintf(p, "%-24s: 0x%x\n",
			"MTK_PM_QOS_VCORE_OPP",
			mtk_pm_qos_request(MTK_PM_QOS_VCORE_OPP));
	p += sprintf(p, "%-24s: 0x%x\n",
			"MTK_PM_QOS_SCP_VCORE_REQ",
			mtk_pm_qos_request(MTK_PM_QOS_SCP_VCORE_REQUEST));
	p += sprintf(p, "%-24s: 0x%x\n",
			"MTK_PM_QOS_PM_DDR_REQ",
			mtk_pm_qos_request(MTK_PM_QOS_POWER_MODEL_DDR_REQUEST));
	p += sprintf(p, "%-24s: 0x%x\n",
			"MTK_PM_QOS_PM_VCORE_REQ",
			mtk_pm_qos_request(MTK_PM_QOS_POWER_MODEL_VCORE_REQUEST));
	p += sprintf(p, "%-24s: 0x%x\n",
			"MTK_PM_QOS_FORCE_OPP",
			mtk_pm_qos_request(MTK_PM_QOS_VCORE_DVFS_FORCE_OPP));
	p += sprintf(p, "%-24s: 0x%x\n",
			"MTK_PM_QOS_ISP_HRT",
			mtk_pm_qos_request(MTK_PM_QOS_ISP_HRT_BANDWIDTH));
}

u32 dvfsrc_dump_reg(char *ptr, u32 count)
{
	char buf[1024];
	u32 index = 0;

	memset(buf, '\0', sizeof(buf));
	get_opp_info(buf);
	if (ptr) {
		index += scnprintf(&ptr[index], (count - index - 1),
		"%s\n", buf);
	} else
		pr_info("%s\n", buf);

	memset(buf, '\0', sizeof(buf));
	get_dvfsrc_reg(buf);
	if (ptr) {
		index += scnprintf(&ptr[index], (count - index - 1),
		"%s\n", buf);
	} else
		pr_info("%s\n", buf);

	memset(buf, '\0', sizeof(buf));
	get_dvfsrc_record(buf);
	if (ptr) {
		index += scnprintf(&ptr[index], (count - index - 1),
		"%s\n", buf);
	} else
		pr_info("%s\n", buf);

	memset(buf, '\0', sizeof(buf));
	get_spm_reg(buf);
	if (ptr) {
		index += scnprintf(&ptr[index], (count - index - 1),
		"%s\n", buf);
	} else
		pr_info("%s\n", buf);

	memset(buf, '\0', sizeof(buf));
	get_mtk_pm_qos_info(buf);
	if (ptr) {
		index += scnprintf(&ptr[index], (count - index - 1),
		"%s\n", buf);
	} else
		pr_info("%s\n", buf);

	return index;
}

void dvfsrc_set_power_model_ddr_request(unsigned int level)
{
	commit_data(DVFSRC_QOS_POWER_MODEL_DDR_REQUEST, level, 1);
}

static int dvfsrc_qos_notify(struct notifier_block *nb,
			     unsigned long value, void *data)
{
	struct dvfsrc_qos_notifier *notifier =
		container_of(nb, struct dvfsrc_qos_notifier, nb);
	int ret = 0;

	mutex_lock(&dvfsrc_lock);
	if (dvfsrc && READ_ONCE(dvfsrc->qos_enabled))
		/* Replay and concurrent class updates must use the latest aggregate. */
		ret = commit_data(notifier->type,
			mtk_pm_qos_request(dvfsrc_qos_classes[notifier->type]), 1);
	mutex_unlock(&dvfsrc_lock);
	return notifier_from_errno(ret < 0 ? ret : 0);
}

static void dvfsrc_notifier_unregister(struct helio_dvfsrc *ctx)
{
	struct dvfsrc_qos_notifier *notifier;

	/* Do not hold dvfsrc_lock while draining a blocking notifier chain. */
	while (ctx->num_notifiers) {
		notifier = &ctx->notifiers[--ctx->num_notifiers];
		mtk_pm_qos_remove_notifier(dvfsrc_qos_classes[notifier->type],
					   &notifier->nb);
	}
}

static int dvfsrc_notifier_register(struct helio_dvfsrc *ctx)
{
	struct dvfsrc_qos_notifier *notifier;
	int type, ret;

	for (type = 1; type < DVFSRC_QOS_NUM_CLASSES; type++) {
		notifier = &ctx->notifiers[ctx->num_notifiers];
		notifier->type = type;
		notifier->nb.notifier_call = dvfsrc_qos_notify;
		ret = mtk_pm_qos_add_notifier(dvfsrc_qos_classes[type],
					     &notifier->nb);
		if (ret) {
			dvfsrc_notifier_unregister(ctx);
			return ret;
		}
		ctx->num_notifiers++;
	}
	return 0;
}

__weak void mtk_pm_qos_trace_dbg_show_request(int mtk_pm_qos_class)
{
}

static DEFINE_RATELIMIT_STATE(tracelimit, 5 * HZ, 1);

void vcorefs_trace_qos(void)
{
	if (__ratelimit(&tracelimit)) {
		mtk_pm_qos_trace_dbg_show_request(MTK_PM_QOS_DDR_OPP);
		mtk_pm_qos_trace_dbg_show_request(MTK_PM_QOS_VCORE_OPP);
	}
}

static void dvfsrc_unpublish(struct helio_dvfsrc *ctx)
{
	unsigned long flags;

	lockdep_assert_held(&dvfsrc_lock);
	spin_lock_irqsave(&dvfsrc_mmio_lock, flags);
	WRITE_ONCE(ctx->qos_enabled, false);
	WRITE_ONCE(ctx->dvfsrc_enabled, false);
	dvfsrc = NULL;
	spin_unlock_irqrestore(&dvfsrc_mmio_lock, flags);
}

static int helio_dvfsrc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct helio_dvfsrc *ctx;
	struct resource *res;
	unsigned long flags;
	u32 flag = 0;
	int ret;

	mutex_lock(&dvfsrc_probe_lock);
	if (dvfsrc) {
		ret = -EBUSY;
		goto out;
	}
#ifdef DVFSRC_SMC_CONTROL
	ret = helio_dvfsrc_firmware_ready();
	if (ret)
		goto out;
#else
	if (spm_load_firmware_status() <= 0) {
		ret = -EPROBE_DEFER;
		goto out;
	}
#endif
	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto out;
	}
	ctx->dev = dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < SZ_4K) {
		ret = -EINVAL;
		goto out;
	}
	ctx->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(ctx->regs)) {
		ret = PTR_ERR(ctx->regs);
		goto out;
	}
	of_property_read_u32(dev->of_node, "dvfsrc_flag", &flag);
	if (flag > 3) {
		ret = -EINVAL;
		goto out;
	}
	ctx->dvfsrc_flag = flag;
#ifdef CONFIG_MACH_MT6877
	ret = helio_dvfsrc_platform_pre_init(ctx);
	if (ret)
		goto out;
#else
	helio_dvfsrc_platform_pre_init(ctx);
#endif
	ret = helio_dvfsrc_prepare(ctx);
	if (ret)
		goto out;
	helio_dvfsrc_add_requests();
	ret = dvfsrc_notifier_register(ctx);
	if (ret)
		goto remove_requests;
#ifdef CONFIG_MACH_MT6877
	ret = dvfsrc_mt6877_init_opps(ctx);
	if (ret)
		goto unregister;
#endif
	helio_dvfsrc_common_init();

	platform_set_drvdata(pdev, ctx);
	mutex_lock(&dvfsrc_lock);
	spin_lock_irqsave(&dvfsrc_mmio_lock, flags);
	dvfsrc = ctx;
	spin_unlock_irqrestore(&dvfsrc_mmio_lock, flags);
	ret = helio_dvfsrc_enable_locked(1);
	if (!ret)
		ret = helio_dvfsrc_config(ctx);
	if (!ret)
		ret = dvfsrc_restore();
	if (ret)
		dvfsrc_unpublish(ctx);
	else
		WRITE_ONCE(ctx->qos_enabled, true);
	mutex_unlock(&dvfsrc_lock);
	if (ret)
		goto unregister;

	/* MT6877 retains its boot floor until MMDVFS completes clock handoff. */
#ifndef CONFIG_MACH_MT6877
	helio_dvfsrc_qos_init_done();
#endif
	ret = helio_dvfsrc_add_interface(dev);
	if (ret)
		dev_warn(dev, "control interface unavailable: %d\n", ret);
	else
		ctx->interface_created = true;
	dev_info(dev, "DVFSRC firmware, OPP tables and QoS backend initialized\n");
	ret = 0;
	goto out;

unregister:
	dvfsrc_notifier_unregister(ctx);
	platform_set_drvdata(pdev, NULL);
remove_requests:
	helio_dvfsrc_remove_requests();
	helio_dvfsrc_unprepare(ctx);
out:
	mutex_unlock(&dvfsrc_probe_lock);
	return ret ? dev_err_probe(dev, ret, "DVFSRC initialization failed\n") : 0;
}

static int helio_dvfsrc_remove(struct platform_device *pdev)
{
	struct helio_dvfsrc *ctx = platform_get_drvdata(pdev);

	mutex_lock(&dvfsrc_probe_lock);
	if (ctx->interface_created)
		helio_dvfsrc_remove_interface(&pdev->dev);
	dvfsrc_notifier_unregister(ctx);
	mutex_lock(&dvfsrc_lock);
	dvfsrc_unpublish(ctx);
	mutex_unlock(&dvfsrc_lock);
	helio_dvfsrc_unprepare(ctx);
	helio_dvfsrc_remove_requests();
	platform_set_drvdata(pdev, NULL);
	mutex_unlock(&dvfsrc_probe_lock);
	return 0;
}

static const struct of_device_id helio_dvfsrc_of_match[] = {
	{ .compatible = "mediatek,dvfsrc" },
	{ .compatible = "mediatek,mt6785-dvfsrc" },
	{ },
};

MODULE_DEVICE_TABLE(of, helio_dvfsrc_of_match);

static __maybe_unused int helio_dvfsrc_suspend(struct device *dev)
{
	int ret = 0;

	if (dvfsrc->suspend) {
		ret = dvfsrc->suspend(dvfsrc);
		if (ret)
			return ret;
	}

	return 0;
}

static __maybe_unused int helio_dvfsrc_resume(struct device *dev)
{
	int ret = 0;

	if (dvfsrc->resume) {
		ret = dvfsrc->resume(dvfsrc);
		if (ret)
			return ret;
	}

	return ret;
}

static SIMPLE_DEV_PM_OPS(helio_dvfsrc_pm, helio_dvfsrc_suspend,
			 helio_dvfsrc_resume);

static struct platform_driver helio_dvfsrc_driver = {
	.probe	= helio_dvfsrc_probe,
	.remove	= helio_dvfsrc_remove,
	.driver = {
		.name = "helio-dvfsrc",
		.suppress_bind_attrs = true,
		.pm	= &helio_dvfsrc_pm,
		.of_match_table = helio_dvfsrc_of_match,
	},
};

static int __init helio_dvfsrc_init(void)
{
	int ret = 0;

	ret = platform_driver_register(&helio_dvfsrc_driver);
	return ret;
}
late_initcall_sync(helio_dvfsrc_init)

static void __exit helio_dvfsrc_exit(void)
{
	platform_driver_unregister(&helio_dvfsrc_driver);
}
module_exit(helio_dvfsrc_exit)

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Helio dvfsrc driver");
