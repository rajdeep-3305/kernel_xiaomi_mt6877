/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
*/

#include "helio-dvfsrc-smc-control.h"
#include <linux/errno.h>
#include <linux/printk.h>

#define SPM_FLAG_RUN_COMMON_SCENARIO          (1U << 10)
#define SPM_FLAG_DIS_VCORE_DVS                (1U << 3)
#define SPM_FLAG_DIS_VCORE_DFS                (1U << 4)

int helio_dvfsrc_firmware_ready(void)
{
	int status = helio_dvfsrc_firmware_status();

	/* Ruby ATF: 1 = firmware loaded, 2 = firmware running. */
	switch (status) {
	case 1:
	case 2:
		return 0;
	case 0:
		return -EPROBE_DEFER;
	case SMCCC_RET_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	default:
		return -EIO;
	}
}

static int dvfsrc_smc_result(unsigned long result)
{
	if ((int)result == SMCCC_RET_NOT_SUPPORTED)
		return -EOPNOTSUPP;
	return result ? -EIO : 0;
}

int mtk_spmfw_init_checked(int dvfsrc_en, int skip_check)
{
	int dvfsrc_flag = dvfsrc_en >> 1;
	unsigned int spm_flags = SPM_FLAG_RUN_COMMON_SCENARIO;
	int ret;

	ret = helio_dvfsrc_firmware_ready();
	if (ret)
		return ret;
	if (!skip_check)
		return 0;

	if (dvfsrc_en & 1) {
		if (dvfsrc_flag & 0x1)
			spm_flags |= SPM_FLAG_DIS_VCORE_DVS;

		if (dvfsrc_flag & 0x2)
			spm_flags |= SPM_FLAG_DIS_VCORE_DFS;
	} else
		spm_flags |= (
			SPM_FLAG_DIS_VCORE_DVS | SPM_FLAG_DIS_VCORE_DFS);

	ret = dvfsrc_smc_result(helio_dvfsrc_smc(VCOREFS_SMC_CMD_0, 0, 0, 0));
	if (ret)
		return ret;
	return dvfsrc_smc_result(helio_dvfsrc_smc(VCOREFS_SMC_CMD_1,
						spm_flags, 0, 0));
}

void mtk_spmfw_init(int dvfsrc_en, int skip_check)
{
	int ret = mtk_spmfw_init_checked(dvfsrc_en, skip_check);

	if (ret)
		pr_err_ratelimited("DVFSRC firmware initialization failed: %d\n", ret);
}

int spm_dvfs_pwrap_cmd_checked(int pwrap_cmd, int pwrap_vcore)
{
	int ret;

	if (pwrap_cmd < 0 || pwrap_vcore < 0)
		return -EINVAL;
	if (IS_ENABLED(CONFIG_MACH_MT6877) &&
	    (pwrap_cmd > 18 || pwrap_vcore > 0x7f))
		return -EINVAL;
	ret = helio_dvfsrc_firmware_ready();
	if (ret)
		return ret;
	return dvfsrc_smc_result(helio_dvfsrc_smc(VCOREFS_SMC_CMD_3,
						pwrap_cmd, pwrap_vcore, 0));
}

void spm_dvfs_pwrap_cmd(int pwrap_cmd, int pwrap_vcore)
{
	int ret = spm_dvfs_pwrap_cmd_checked(pwrap_cmd, pwrap_vcore);

	if (ret)
		pr_err_ratelimited("DVFSRC voltage command failed: %d\n", ret);
}

inline int spm_load_firmware_status(void)
{
	return helio_dvfsrc_firmware_status();
}

inline int spm_get_spmfw_idx(void)
{
	return helio_dvfsrc_spmfw_type();
}
