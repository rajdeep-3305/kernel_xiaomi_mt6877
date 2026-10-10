// SPDX-License-Identifier: GPL-2.0
/* Ruby ISP6S QoS: reference PM QoS clock votes and per-port MMQoS bandwidth. */
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <dt-bindings/memory/mt6877-larb-port.h>

#include "inc/cam_qos.h"
#include "mmdvfs_pmqos.h"

struct cam_qos_module {
	struct plist_head requests;
	struct mm_qos_request port[_cam_max_];
	u32 master[_cam_max_];
	u32 count;
};

static DEFINE_MUTEX(cam_qos_lock);
static struct cam_qos_module cam_modules[ISP_IRQ_TYPE_AMOUNT];
static struct mtk_pm_qos_request cam_clock;
static bool cam_qos_ready;
static u32 target_clk;

/* Several software DMA streams share one physical larb port in ISP6S. */
static const struct {
	const char *name;
	u8 port;
} raw_ports[_cam_max_] = {
	[_imgo_] = { "imgo_r1", 0 },
	[_ufeo_] = { "rsso_r1", 14 },
	[_rrzo_] = { "rrzo_r1", 1 },
	[_ufgo_] = { "rsso_r1", 14 },
	[_yuvo_] = { "yuvo_r1", 4 },
	[_yuvbo_] = { "yuvo_r1", 4 },
	[_yuvco_] = { "yuvo_r1", 4 },
	[_tsfso_] = { "ltmso_r1", 13 },
	[_aao_] = { "aao_r1", 8 },
	[_aaho_] = { "aaho_r1", 15 },
	[_afo_] = { "afo_r1", 9 },
	[_pdo_] = { "flko_r1", 10 },
	[_flko_] = { "flko_r1", 10 },
	[_lcso_] = { "lceso_r1", 11 },
	[_lcesho_] = { "ltmso_r1", 13 },
	[_ltmso_] = { "ltmso_r1", 13 },
	[_lmvo_] = { "afo_r1", 9 },
	[_rsso_] = { "rsso_r1", 14 },
	[_rsso_r2_] = { "rsso_r1", 14 },
	[_crzo_] = { "crzo_r1", 12 },
	[_crzbo_] = { "crzo_r1", 12 },
	[_crzo_r2_] = { "crzo_r1", 12 },
	[_rawi_] = { "rawi_r2", 6 },
	[_ufdi_r2_] = { "ufdi_r2", 5 },
	[_bpci_] = { "bpci_r1", 3 },
	[_lsci_] = { "lsci_r1", 16 },
	[_pdi_] = { "bpci_r1", 3 },
	[_bpci_r2_] = { "bpci_r1", 3 },
	[_rawi_r3_] = { "rawi_r3", 7 },
	[_bpci_r3_] = { "bpci_r1", 3 },
	[_cqi_r1_] = { "cqi_r1", 2 },
	[_cqi_r2_] = { "cqi_r1", 2 },
};

static int cam_parse_ports(struct device_node *node)
{
	u32 module, port, master;
	char property[48];
	int ret;

	for (module = ISP_IRQ_TYPE_INT_CAM_A_ST; module <= ISP_IRQ_TYPE_INT_CAM_B_ST;
	     module++) {
		struct cam_qos_module *qos = &cam_modules[module];

		qos->count = ARRAY_SIZE(raw_ports);
		for (port = 0; port < qos->count; port++) {
			snprintf(property, sizeof(property), "l%u_cam_%s_%c",
				 16 + module, raw_ports[port].name, 'a' + module);
			ret = of_property_read_u32(node, property, &master);
			if (ret)
				return ret;
			if (master != MTK_M4U_ID(16 + module, raw_ports[port].port))
				return -EINVAL;
			qos->master[port] = master;
		}
	}

	/*
	 * SV0 has its image output on larb14 and its optional UFE output on
	 * larb13.  SV1 has both outputs on larb13.  These are separate MMQoS
	 * masters in the MT6877 ISP6S ABI even though UFE is normally unused.
	 */
	ret = of_property_read_u32(node, "l14_cam_camsv0", &master);
	if (ret)
		return ret;
	if (master != MTK_M4U_ID(14, 3))
		return -EINVAL;
	cam_modules[ISP_IRQ_TYPE_INT_CAMSV_0_ST].master[_camsv_imgo_] = master;

	ret = of_property_read_u32(node, "l13_cam_camsv1", &master);
	if (ret)
		return ret;
	if (master != MTK_M4U_ID(13, 3))
		return -EINVAL;
	cam_modules[ISP_IRQ_TYPE_INT_CAMSV_0_ST].master[_camsv_ufeo_] = master;
	cam_modules[ISP_IRQ_TYPE_INT_CAMSV_0_ST].count = _camsv_max_;

	ret = of_property_read_u32(node, "l13_cam_camsv2", &master);
	if (ret)
		return ret;
	if (master != MTK_M4U_ID(13, 4))
		return -EINVAL;
	cam_modules[ISP_IRQ_TYPE_INT_CAMSV_1_ST].master[_camsv_imgo_] = master;

	ret = of_property_read_u32(node, "l13_cam_camsv3", &master);
	if (ret)
		return ret;
	if (master != MTK_M4U_ID(13, 5))
		return -EINVAL;
	cam_modules[ISP_IRQ_TYPE_INT_CAMSV_1_ST].master[_camsv_ufeo_] = master;
	cam_modules[ISP_IRQ_TYPE_INT_CAMSV_1_ST].count = _camsv_max_;

	/* MT6877 exposes MMQoS masters for logical SV2..7 in shared pairs. */
	for (module = ISP_IRQ_TYPE_INT_CAMSV_2_ST;
	     module <= ISP_IRQ_TYPE_INT_CAMSV_7_ST; module++) {
		struct cam_qos_module *qos = &cam_modules[module];

		port = 4 + (module - ISP_IRQ_TYPE_INT_CAMSV_2_ST) / 2;
		snprintf(property, sizeof(property), "l13_cam_camsv%u", port);
		ret = of_property_read_u32(node, property, &master);
		if (ret)
			return ret;
		if (master != MTK_M4U_ID(13, port + 2))
			return -EINVAL;
		qos->master[_camsv_imgo_] = master;
		qos->count = 1;
	}
	return 0;
}

static u32 cam_compression(u32 module, u32 port)
{
	if (module >= ISP_IRQ_TYPE_INT_CAMSV_START_ST)
		return port == _camsv_ufeo_ ? BW_COMP_DEFAULT : BW_COMP_NONE;
	if (module <= ISP_IRQ_TYPE_INT_CAM_B_ST &&
	    (port == _ufeo_ || port == _ufgo_ || port == _ufdi_r2_))
		return BW_COMP_DEFAULT;
	return BW_COMP_NONE;
}

static int cam_update_bw(u32 module, const struct ISP_BW *bw)
{
	struct cam_qos_module *qos = &cam_modules[module];
	struct { u32 avg, peak, comp; } old[_cam_max_];
	u32 i;
	int ret;

	for (i = 0; i < qos->count; i++) {
		old[i].avg = qos->port[i].bw_value;
		old[i].peak = qos->port[i].hrt_value;
		old[i].comp = qos->port[i].comp_type;
		ret = mm_qos_set_request(&qos->port[i], bw ? bw[i].avg : 0,
					bw ? bw[i].peak : 0, cam_compression(module, i));
		if (ret)
			goto restore;
	}
	return mt6877_mm_qos_update(&qos->requests);
restore:
	/* No hardware update has been sent yet; undo the partial staged vote. */
	while (i--)
		mm_qos_set_request(&qos->port[i], old[i].avg, old[i].peak, old[i].comp);
	return ret;
}

static int cam_qos_command(enum E_QOS_OP cmd, u32 module, unsigned int *value)
{
	u64 steps[MAX_FREQ_STEP];
	u32 count, i;
	int ret = 0;

	if (module >= ARRAY_SIZE(cam_modules))
		return -EINVAL;
	mutex_lock(&cam_qos_lock);
	if (!cam_qos_ready) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	if ((cmd == E_BW_UPDATE || cmd == E_BW_CLR || cmd == E_CLK_UPDATE ||
	     cmd == E_CLK_CUR || cmd == E_CLK_SUPPORTED) && !value) {
		ret = -EINVAL;
		goto out;
	}
	switch (cmd) {
	case E_BW_ADD:
	case E_CLK_ADD:
		/* Requests are owned by this provider, not early ISP initcalls. */
		ret = mt6877_mmdvfs_ready();
		break;
	case E_BW_UPDATE:
	case E_BW_CLR:
	case E_BW_REMOVE:
		if (!cam_modules[module].count) {
			/* MT6877 is a two-RAW platform; CAM-C is a valid no-op. */
			ret = 0;
			break;
		}
		if (cmd != E_BW_CLR || !value[0])
			ret = cam_update_bw(module, cmd == E_BW_UPDATE ?
					    (const struct ISP_BW *)value : NULL);
		break;
	case E_CLK_REMOVE:
	case E_CLK_CLR:
	case E_CLK_UPDATE:
		ret = mt6877_mmdvfs_update_request(&cam_clock,
						  cmd == E_CLK_UPDATE ? value[0] : 0);
		/*
		 * target_clk describes the vote owned by cam_clock.  The camera HAL
		 * compares it with the requested IQ level before issuing the next
		 * ISP_DFS_UPDATE.  Leaving a stale target behind after E_CLK_CLR made
		 * a photo-to-video reconfiguration look as if its 624 MHz vote was
		 * still active even though the PM QoS request had been released.
		 */
		if (cmd != E_CLK_UPDATE)
			target_clk = 0;
		else if (!ret)
			target_clk = value[0];
		break;
	case E_CLK_SUPPORTED:
		ret = mmdvfs_qos_get_freq_steps(PM_QOS_CAM_FREQ, steps, &count);
		if (ret)
			break;
		if (!count || count > ARRAY_SIZE(steps) || count >= ISP_CLK_LEVEL_CNT) {
			ret = -EINVAL;
			break;
		}
		for (i = 0; i < count; i++)
			value[i] = steps[i];
		ret = count;
		break;
	case E_CLK_CUR:
		ret = mt6877_mmdvfs_ready();
		if (!ret) {
			value[0] = mmdvfs_qos_get_freq(PM_QOS_CAM_FREQ);
			value[1] = target_clk;
		}
		break;
	default:
		ret = -EINVAL;
	}
out:
	mutex_unlock(&cam_qos_lock);
	return ret;
}

int ISP_SetPMQOS(enum E_QOS_OP cmd, enum ISP_IRQ_TYPE_ENUM module, unsigned int *value)
{
	if ((unsigned int)module > ISP_IRQ_TYPE_INT_CAM_C_ST)
		return -EINVAL;
	return cam_qos_command(cmd, module, value);
}
EXPORT_SYMBOL_GPL(ISP_SetPMQOS);

int SV_SetPMQOS(enum E_QOS_OP cmd, enum ISP_IRQ_TYPE_ENUM module, unsigned int *value)
{
	if (module < ISP_IRQ_TYPE_INT_CAMSV_START_ST || module > ISP_IRQ_TYPE_INT_CAMSV_END_ST)
		return -EINVAL;
	/* The legacy ABI exposes a shared RAW-owned clock vote to CAMSV. */
	if (cmd == E_CLK_ADD || cmd == E_CLK_CLR || cmd == E_CLK_REMOVE)
		return -EINVAL;
	return cam_qos_command(cmd, module, value);
}
EXPORT_SYMBOL_GPL(SV_SetPMQOS);

static void cam_remove_requests(void)
{
	u32 module;

	for (module = 0; module < ARRAY_SIZE(cam_modules); module++)
		mm_qos_remove_all_request(&cam_modules[module].requests);
}

static int cam_qos_probe(struct platform_device *pdev)
{
	u32 module, port;
	int ret;

	mutex_lock(&cam_qos_lock);
	if (cam_qos_ready) {
		ret = -EBUSY;
		goto out;
	}
	ret = mt6877_mmdvfs_ready();
	if (ret)
		goto out;
	ret = cam_parse_ports(pdev->dev.of_node);
	if (ret)
		goto out;
	for (module = 0; module < ARRAY_SIZE(cam_modules); module++)
		plist_head_init(&cam_modules[module].requests);
	for (module = 0; module < ARRAY_SIZE(cam_modules); module++) {
		struct cam_qos_module *qos = &cam_modules[module];

		for (port = 0; port < qos->count; port++) {
			ret = mm_qos_add_request(&qos->requests, &qos->port[port],
						 qos->master[port]);
			if (ret) {
				cam_remove_requests();
				goto out;
			}
		}
	}
	mtk_pm_qos_add_request(&cam_clock, PM_QOS_CAM_FREQ, PM_QOS_MM_FREQ_DEFAULT_VALUE);
	cam_qos_ready = true;
	dev_dbg(&pdev->dev, "RAW A/B and SV0..7 MMQoS ready; shared CAM frequency vote\n");
out:
	mutex_unlock(&cam_qos_lock);
	return ret ? dev_err_probe(&pdev->dev, ret, "camera QoS setup failed\n") : 0;
}

static void cam_qos_remove(struct platform_device *pdev)
{
	u32 module;

	mutex_lock(&cam_qos_lock);
	cam_qos_ready = false;
	for (module = 0; module < ARRAY_SIZE(cam_modules); module++)
		if (cam_modules[module].count)
			cam_update_bw(module, NULL);
	cam_remove_requests();
	mtk_pm_qos_remove_request(&cam_clock);
	mutex_unlock(&cam_qos_lock);
}

static const struct of_device_id cam_qos_match[] = {
	{ .compatible = "mediatek,cam_qos_legacy" },
	{ }
};
MODULE_DEVICE_TABLE(of, cam_qos_match);

static struct platform_driver cam_qos_driver = {
	.probe = cam_qos_probe,
	.remove_new = cam_qos_remove,
	.driver = {
		.name = "cam-qos-mt6877",
		.of_match_table = cam_qos_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(cam_qos_driver);
MODULE_LICENSE("GPL");
