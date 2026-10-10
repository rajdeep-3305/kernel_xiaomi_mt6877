// SPDX-License-Identifier: GPL-2.0
/* MediaTek 6.6 IOMMU secure-world bridge. */

#define pr_fmt(fmt) "mtk-iommu-pseudo: " fmt

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "../pseudo_m4u/pseudo_m4u_gz_sec.h"

#define MTK_IOMMU_MAX_LARBS 32
#define MTK_IOMMU_SEC_ID_COUNT 4

struct mtk_iommu_pseudo_data {
	struct device *larbs[MTK_IOMMU_MAX_LARBS];
	unsigned int nr_larbs;
};

static DEFINE_MUTEX(mtk_iommu_sec_lock);
static bool mtk_iommu_sec_ready[MTK_IOMMU_SEC_ID_COUNT];
static struct mtk_iommu_pseudo_data *mtk_iommu_pseudo;

static int mtk_iommu_gz_init(int sec_id)
{
	struct m4u_gz_sec_context *ctx;
	unsigned int powered = 0;
	int ret = 0;

	ctx = m4u_gz_sec_ctx_get();
	if (!ctx)
		return -EFAULT;

	while (powered < mtk_iommu_pseudo->nr_larbs) {
		ret = pm_runtime_resume_and_get(mtk_iommu_pseudo->larbs[powered]);
		if (ret < 0)
			goto out;
		powered++;
	}

	ctx->gz_m4u_msg->cmd = CMD_M4UTY_INIT;
	ctx->gz_m4u_msg->iommu_sec_id = sec_id;
	ctx->gz_m4u_msg->init_param.nonsec_pt_pa = 0;
	ctx->gz_m4u_msg->init_param.l2_en = 1;
	ctx->gz_m4u_msg->init_param.sec_pt_pa = 0;
	ret = m4u_gz_exec_cmd(ctx);

out:
	while (powered)
		pm_runtime_put_sync(mtk_iommu_pseudo->larbs[--powered]);
	m4u_gz_sec_ctx_put(ctx);
	return ret;
}

int m4u_gz_sec_init(int sec_id)
{
	int ret;

	if (sec_id < 0 || sec_id >= MTK_IOMMU_SEC_ID_COUNT)
		return -EINVAL;
	if (!mtk_iommu_pseudo)
		return -EPROBE_DEFER;

	mutex_lock(&mtk_iommu_sec_lock);
	if (!mtk_iommu_sec_ready[sec_id]) {
		m4u_gz_sec_set_context();
		ret = m4u_gz_sec_context_init();
		if (ret)
			goto out;
		mtk_iommu_sec_ready[sec_id] = true;
	}

	ret = mtk_iommu_gz_init(sec_id);
	if (ret) {
		mtk_iommu_sec_ready[sec_id] = false;
		m4u_gz_sec_context_deinit();
	}

out:
	mutex_unlock(&mtk_iommu_sec_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(m4u_gz_sec_init);

static int mtk_iommu_pseudo_probe(struct platform_device *pdev)
{
	struct mtk_iommu_pseudo_data *data;
	struct device_node *node;
	int count, i;

	count = of_count_phandle_with_args(pdev->dev.of_node,
					   "mediatek,larbs", NULL);
	if (count <= 0)
		return count ? count : -EINVAL;
	if (count > MTK_IOMMU_MAX_LARBS)
		return -E2BIG;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct platform_device *larb;

		node = of_parse_phandle(pdev->dev.of_node, "mediatek,larbs", i);
		if (!node)
			return -EINVAL;
		if (!of_device_is_available(node)) {
			of_node_put(node);
			continue;
		}

		larb = of_find_device_by_node(node);
		of_node_put(node);
		if (!larb)
			return -EPROBE_DEFER;
		data->larbs[data->nr_larbs++] = &larb->dev;
	}

	mtk_iommu_pseudo = data;
	platform_set_drvdata(pdev, data);
	dev_info(&pdev->dev, "secure IOMMU bridge ready with %u larbs\n",
		 data->nr_larbs);
	return 0;
}

static const struct of_device_id mtk_iommu_pseudo_of_match[] = {
	{ .compatible = "mediatek,mt6877-iommu-pseudo" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_iommu_pseudo_of_match);

static struct platform_driver mtk_iommu_pseudo_driver = {
	.probe = mtk_iommu_pseudo_probe,
	.driver = {
		.name = "mtk-iommu-pseudo",
		.of_match_table = mtk_iommu_pseudo_of_match,
	},
};
module_platform_driver(mtk_iommu_pseudo_driver);

MODULE_LICENSE("GPL");
