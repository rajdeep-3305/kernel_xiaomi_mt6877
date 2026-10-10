// SPDX-License-Identifier: GPL-2.0
/* MT6877 CCU: retain the 4.19 MMQoS masters and CAM frequency-level ABI. */
#include <linux/device.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <dt-bindings/memory/mt6877-larb-port.h>
#include "ccu_reg.h"
#include "ccu_hw.h"
#include "ccu_qos.h"
#include "mmdvfs_pmqos.h"

static DEFINE_MUTEX(ccu_qos_lock);
static PLIST_HEAD(ccu_bw_list);
/* Input, output, and virtual common traffic, in userspace report order. */
static struct mm_qos_request ccu_bw_req[3];
static struct mtk_pm_qos_request ccu_cam_freq;
static u64 ccu_freq_steps[MAX_FREQ_STEP];
static u32 ccu_step_count;
static bool ccu_qos_ready;
static bool ccu_qos_active;
static const u32 ccu_bw_default[] = { 60, 60, 15 };

static int ccu_set_bw(const u32 *bw)
{
	u32 old[3], i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(ccu_bw_req); i++) {
		old[i] = ccu_bw_req[i].bw_value;
		ret = mm_qos_set_request(&ccu_bw_req[i], bw[i], bw[i], BW_COMP_NONE);
		if (ret) {
			while (i--)
				mm_qos_set_request(&ccu_bw_req[i], old[i], old[i], BW_COMP_NONE);
			return ret;
		}
	}
	return mt6877_mm_qos_update(&ccu_bw_list);
}

int ccu_qos_set_freq(u32 level)
{
	int ret;

	mutex_lock(&ccu_qos_lock);
	if (!ccu_qos_ready)
		ret = -EPROBE_DEFER;
	else if (level >= ccu_step_count)
		ret = -EINVAL;
	else
		/* Reference ABI: level zero releases; others index descending MHz. */
		ret = mt6877_mmdvfs_update_request(&ccu_cam_freq,
						  level ? ccu_freq_steps[level] : 0);
	mutex_unlock(&ccu_qos_lock);
	return ret;
}

int ccu_qos_init(struct ccu_device_s *ccu)
{
	int ret;

	mutex_lock(&ccu_qos_lock);
	ret = ccu_qos_ready ? ccu_set_bw(ccu_bw_default) : -EPROBE_DEFER;
	if (!ret)
		ccu_qos_active = true;
	mutex_unlock(&ccu_qos_lock);
	return ret;
}

int ccu_qos_update_req(struct ccu_device_s *ccu, uint32_t *bw)
{
	u32 value[3];
	int ret;

	if (!bw)
		return -EINVAL;
	mutex_lock(&ccu_qos_lock);
	if (!ccu_qos_ready || !ccu_qos_active) {
		ret = -ENODEV;
		goto out;
	}
	/* Keep the shipped 4.19 firmware mapping, despite the O/G macro names. */
	value[0] = ccu_read_reg(ccu_base, SPREG_12_CCU_BW_I_REG);
	value[2] = ccu_read_reg(ccu_base, SPREG_13_CCU_BW_O_REG);
	value[1] = ccu_read_reg(ccu_base, SPREG_14_CCU_BW_G_REG);
	if (value[0] > 60 || value[1] > 60 || value[2] > 15)
		memcpy(value, ccu_bw_default, sizeof(value));
	ret = ccu_set_bw(value);
	if (!ret)
		memcpy(bw, value, sizeof(value));
out:
	mutex_unlock(&ccu_qos_lock);
	return ret;
}

void ccu_qos_uninit(struct ccu_device_s *ccu)
{
	const u32 zero[3] = { 0 };
	int ret;

	mutex_lock(&ccu_qos_lock);
	if (ccu_qos_ready) {
		ret = ccu_set_bw(zero);
		if (ret)
			dev_warn(ccu->dev, "CCU bandwidth release failed: %d\n", ret);
	}
	ccu_qos_active = false;
	mutex_unlock(&ccu_qos_lock);
}

static void ccu_qos_release(void *data)
{
	const u32 zero[3] = { 0 };

	mutex_lock(&ccu_qos_lock);
	if (ccu_qos_ready) {
		ccu_set_bw(zero);
		mtk_pm_qos_remove_request(&ccu_cam_freq);
		mm_qos_remove_all_request(&ccu_bw_list);
		ccu_qos_ready = false;
		ccu_qos_active = false;
	}
	mutex_unlock(&ccu_qos_lock);
}

int ccu_qos_prepare(struct device *dev)
{
	u32 masters[] = { M4U_PORT_L13_CAM_CCUI, M4U_PORT_L13_CAM_CCUO, 0 };
	int ret, virtual_port;
	u32 i;

	ret = mt6877_mmdvfs_ready();
	if (ret)
		return ret;
	mutex_lock(&ccu_qos_lock);
	if (ccu_qos_ready) {
		ret = -EBUSY;
		goto out;
	}
	ret = mmdvfs_qos_get_freq_steps(PM_QOS_CAM_FREQ, ccu_freq_steps, &ccu_step_count);
	if (ret)
		goto out;
	if (!ccu_step_count || ccu_step_count > MAX_FREQ_STEP) {
		ret = -EINVAL;
		goto out;
	}
	virtual_port = get_virtual_port(VIRTUAL_CCU_COMMON);
	if (virtual_port < 0) {
		ret = virtual_port;
		goto out;
	}
	masters[2] = virtual_port;
	for (i = 0; i < ARRAY_SIZE(masters); i++) {
		ret = mm_qos_add_request(&ccu_bw_list, &ccu_bw_req[i], masters[i]);
		if (ret) {
			mm_qos_remove_all_request(&ccu_bw_list);
			goto out;
		}
	}
	mtk_pm_qos_add_request(&ccu_cam_freq, PM_QOS_CAM_FREQ, PM_QOS_MM_FREQ_DEFAULT_VALUE);
	ccu_qos_ready = true;
out:
	mutex_unlock(&ccu_qos_lock);
	if (ret)
		return ret;
	/* Action runs on failed probe too; do not leak votes across deferral. */
	return devm_add_action_or_reset(dev, ccu_qos_release, NULL);
}
