/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
*/

#include <linux/device.h>
#include <linux/kernel.h>
#include <mt-plat/mtk_devinfo.h>
#include <helio-dvfsrc-opp.h>
#include <helio-dvfsrc-qos.h>

#ifdef CONFIG_MEDIATEK_DRAMC
#include <dramc.h>
#endif

#define V_VMODE_SHIFT 0
/*#define V_CT_SHIFT 5*/
#define V_CT_TEST_SHIFT 6
#define V_CT_V2_SHIFT 7
#define V_CONSYS_SHIFT 16
#define V_OPP_TYPE_SHIFT 20

static int dvfsrc_rsrv;

static const char vcore_750_bin_tst[14] = {0, 0, 0, 2, 2, 4, 4, 6, 6, 8, 8, 10, 10, 10};
static const char vcore_725_bin_tst[10] = {0, 0, 0, 2, 2, 4, 4, 6, 6, 6};
static const char vcore_550_bin_tst[10] = {0, 0, 0, 0, 0, 2, 2, 4, 4, 4};

static const char vcore_750_bin_mp[14] = {0, 0, 0, 0, 0, 2, 2, 4, 4, 6, 6, 8, 8, 8};
static const char vcore_725_bin_mp[10] = {0, 0, 0, 0, 0, 2, 2, 4, 4, 4};
static const char vcore_550_bin_mp[10] = {0, 0, 0, 0, 0, 0, 2, 2, 4, 4};

#ifndef CONFIG_MEDIATEK_DRAMC
static int mtk_dramc_get_steps_freq(unsigned int step)
{
	pr_info("get dram steps_freq fail\n");
	return 4266;
}
#endif

void dvfsrc_opp_level_mapping(void)
{
	set_vcore_opp(VCORE_DVFS_OPP_0, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_1, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_2, VCORE_OPP_1);
	set_vcore_opp(VCORE_DVFS_OPP_3, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_4, VCORE_OPP_1);
	set_vcore_opp(VCORE_DVFS_OPP_5, VCORE_OPP_2);
	set_vcore_opp(VCORE_DVFS_OPP_6, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_7, VCORE_OPP_1);
	set_vcore_opp(VCORE_DVFS_OPP_8, VCORE_OPP_2);
	set_vcore_opp(VCORE_DVFS_OPP_9, VCORE_OPP_3);
	set_vcore_opp(VCORE_DVFS_OPP_10, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_11, VCORE_OPP_1);
	set_vcore_opp(VCORE_DVFS_OPP_12, VCORE_OPP_2);
	set_vcore_opp(VCORE_DVFS_OPP_13, VCORE_OPP_3);
	set_vcore_opp(VCORE_DVFS_OPP_14, VCORE_OPP_4);
	set_vcore_opp(VCORE_DVFS_OPP_15, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_16, VCORE_OPP_1);
	set_vcore_opp(VCORE_DVFS_OPP_17, VCORE_OPP_2);
	set_vcore_opp(VCORE_DVFS_OPP_18, VCORE_OPP_3);
	set_vcore_opp(VCORE_DVFS_OPP_19, VCORE_OPP_4);
	set_vcore_opp(VCORE_DVFS_OPP_20, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_21, VCORE_OPP_1);
	set_vcore_opp(VCORE_DVFS_OPP_22, VCORE_OPP_2);
	set_vcore_opp(VCORE_DVFS_OPP_23, VCORE_OPP_3);
	set_vcore_opp(VCORE_DVFS_OPP_24, VCORE_OPP_4);
	set_vcore_opp(VCORE_DVFS_OPP_25, VCORE_OPP_0);
	set_vcore_opp(VCORE_DVFS_OPP_26, VCORE_OPP_1);
	set_vcore_opp(VCORE_DVFS_OPP_27, VCORE_OPP_2);
	set_vcore_opp(VCORE_DVFS_OPP_28, VCORE_OPP_3);
	set_vcore_opp(VCORE_DVFS_OPP_29, VCORE_OPP_4);

	set_ddr_opp(VCORE_DVFS_OPP_0, DDR_OPP_0);
	set_ddr_opp(VCORE_DVFS_OPP_1, DDR_OPP_1);
	set_ddr_opp(VCORE_DVFS_OPP_2, DDR_OPP_1);
	set_ddr_opp(VCORE_DVFS_OPP_3, DDR_OPP_2);
	set_ddr_opp(VCORE_DVFS_OPP_4, DDR_OPP_2);
	set_ddr_opp(VCORE_DVFS_OPP_5, DDR_OPP_2);
	set_ddr_opp(VCORE_DVFS_OPP_6, DDR_OPP_3);
	set_ddr_opp(VCORE_DVFS_OPP_7, DDR_OPP_3);
	set_ddr_opp(VCORE_DVFS_OPP_8, DDR_OPP_3);
	set_ddr_opp(VCORE_DVFS_OPP_9, DDR_OPP_3);
	set_ddr_opp(VCORE_DVFS_OPP_10, DDR_OPP_4);
	set_ddr_opp(VCORE_DVFS_OPP_11, DDR_OPP_4);
	set_ddr_opp(VCORE_DVFS_OPP_12, DDR_OPP_4);
	set_ddr_opp(VCORE_DVFS_OPP_13, DDR_OPP_4);
	set_ddr_opp(VCORE_DVFS_OPP_14, DDR_OPP_4);
	set_ddr_opp(VCORE_DVFS_OPP_15, DDR_OPP_5);
	set_ddr_opp(VCORE_DVFS_OPP_16, DDR_OPP_5);
	set_ddr_opp(VCORE_DVFS_OPP_17, DDR_OPP_5);
	set_ddr_opp(VCORE_DVFS_OPP_18, DDR_OPP_5);
	set_ddr_opp(VCORE_DVFS_OPP_19, DDR_OPP_5);
	set_ddr_opp(VCORE_DVFS_OPP_20, DDR_OPP_6);
	set_ddr_opp(VCORE_DVFS_OPP_21, DDR_OPP_6);
	set_ddr_opp(VCORE_DVFS_OPP_22, DDR_OPP_6);
	set_ddr_opp(VCORE_DVFS_OPP_23, DDR_OPP_6);
	set_ddr_opp(VCORE_DVFS_OPP_24, DDR_OPP_6);
	set_ddr_opp(VCORE_DVFS_OPP_25, DDR_OPP_7);
	set_ddr_opp(VCORE_DVFS_OPP_26, DDR_OPP_7);
	set_ddr_opp(VCORE_DVFS_OPP_27, DDR_OPP_7);
	set_ddr_opp(VCORE_DVFS_OPP_28, DDR_OPP_7);
	set_ddr_opp(VCORE_DVFS_OPP_29, DDR_OPP_7);
}

void dvfsrc_opp_table_init(void)
{
	int i;
	int vcore_opp, ddr_opp;

	for (i = 0; i < VCORE_DVFS_OPP_NUM; i++) {
		vcore_opp = get_vcore_opp(i);
		ddr_opp = get_ddr_opp(i);

		if (vcore_opp == VCORE_OPP_UNREQ || ddr_opp == DDR_OPP_UNREQ) {
			set_opp_table(i, 0, 0);
			continue;
		}
		set_opp_table(i, get_vcore_uv_table(vcore_opp),
			get_opp_ddr_freq(ddr_opp));
	}
}

static int get_vb_volt(int vcore_opp, int ct_test)
{
	unsigned int idx;
	int ret = 0;
	int ptpod = get_devinfo_with_index(210);

	pr_info("%s: PTPOD: 0x%x\n", __func__, ptpod);
	switch (vcore_opp) {
	case VCORE_OPP_0:
		idx = (ptpod >> 8) & 0xF;
		if (idx > 13)
			ret = 0;
		else if (ct_test)
			ret = vcore_750_bin_tst[idx];
		else
			ret = vcore_750_bin_mp[idx];
		pr_info("%s: VCORE_OPP_750 bin: 0x%x, %d\n", __func__, idx, ret);
		break;
	case VCORE_OPP_1:
		idx = (ptpod >> 4) & 0xF;
		if (idx > 9)
			ret = 0;
		else if (ct_test)
			ret = vcore_725_bin_tst[idx];
		else
			ret = vcore_725_bin_mp[idx];
		pr_info("%s: VCORE_OPP_725 bin: 0x%x, %d\n", __func__, idx, ret);
		break;
	case VCORE_OPP_4:
		idx = ptpod & 0xF;
		if (idx > 9)
			ret = 0;
		else if (ct_test)
			ret = vcore_550_bin_tst[idx];
		else
			ret = vcore_550_bin_mp[idx];
		pr_info("%s: VCORE_OPP_550 bin: 0x%x, %d\n", __func__, idx, ret);
		break;
	default:
		break;
	}

	return ret * 6250;
}

static int is_rising_need(void)
{
	int idx;
	int ptpod = get_devinfo_with_index(210);

	pr_info("%s: PTPOD: 0x%x\n", __func__, ptpod);
	idx = ptpod & 0xF;
	if (idx == 1 || idx == 2)
		return idx;

	return 0;
}

int dvfsrc_mt6877_init_opps(struct helio_dvfsrc *dvfsrc)
{
	int vcore_opp_0_uv, vcore_opp_1_uv, vcore_opp_2_uv, vcore_opp_3_uv;
	int vcore_opp_4_uv;
	int ddr_khz[DDR_OPP_NUM];
	int vcore_uv[VCORE_OPP_NUM];
	int i, ret;
	int ct_test = 0;
	int is_vcore_ct = 0;
	int is_consys = 0;
	int dvfs_v_mode = 0;
	int opp_type = 0;

	ret = helio_dvfsrc_firmware_ready();
	if (ret)
		return ret;

	/* Load the bootloader's devinfo before accepting the PTPOD entry. */
	get_devinfo_with_index(210);
	if (devinfo_get_size() <= 210)
		return -EPROBE_DEFER;
	if (!IS_ENABLED(CONFIG_MEDIATEK_DRAMC))
		return -ENODEV;
	for (i = 0; i < DDR_OPP_NUM; i++) {
		ret = mtk_dramc_get_steps_freq(i);
		if (ret <= 0)
			return -EPROBE_DEFER;
		if (ret > INT_MAX / 1000)
			return -ERANGE;
		ddr_khz[i] = ret * 1000;
	}

	set_pwrap_cmd(VCORE_OPP_0, 0);
	set_pwrap_cmd(VCORE_OPP_1, 1);
	set_pwrap_cmd(VCORE_OPP_2, 2);
	set_pwrap_cmd(VCORE_OPP_3, 3);
	set_pwrap_cmd(VCORE_OPP_4, 4);

	/* Probe has validated and mapped this exact device's register resource. */
	dvfsrc_rsrv = readl(dvfsrc->regs + 0x610);
	if (dvfsrc_rsrv == -1)
		return -EIO;
	pr_info("%s: vcore_arg = %08x\n", __func__, dvfsrc_rsrv);
	dvfs_v_mode = (dvfsrc_rsrv >> V_VMODE_SHIFT) & 0x3;
	is_vcore_ct = (dvfsrc_rsrv >> V_CT_V2_SHIFT) & 0x1;
	ct_test = (dvfsrc_rsrv >> V_CT_TEST_SHIFT) & 0x1;
	opp_type = (dvfsrc_rsrv >> V_OPP_TYPE_SHIFT) & 0x3;
	is_consys = (dvfsrc_rsrv >> V_CONSYS_SHIFT) & 0x1;

	if (opp_type == 0)
		vcore_opp_0_uv = 725000;
	else
		vcore_opp_0_uv = 750000;

	vcore_opp_1_uv = 725000;
	vcore_opp_2_uv = 650000;
	vcore_opp_3_uv = 600000;
	vcore_opp_4_uv = 550000;

	if (is_vcore_ct && (is_rising_need() == 0) && (opp_type == 1)) {
		/* disable opp1 volt bin */
		/*
		if (opp_type == 0)
			vcore_opp_0_uv -= get_vb_volt(VCORE_OPP_1, ct_test);
		else
			vcore_opp_0_uv -= get_vb_volt(VCORE_OPP_0, ct_test);
		*/
		vcore_opp_1_uv -= get_vb_volt(VCORE_OPP_1, ct_test);
		/* disable opp4 volt bin */
		/*
		vcore_opp_4_uv -= get_vb_volt(VCORE_OPP_4, ct_test);
		vcore_opp_0_uv = max(vcore_opp_0_uv, vcore_opp_1_uv);
		*/
	}

	if (is_rising_need() == 2)
		vcore_opp_4_uv = 575000;
	else if (is_rising_need() == 1)
		vcore_opp_4_uv = 600000;

	if (is_consys)
		vcore_opp_4_uv = 600000;

	if (dvfs_v_mode == 3) {
		/* LV */
		vcore_opp_0_uv = rounddown((vcore_opp_0_uv * 95) / 100, 6250);
		vcore_opp_1_uv = rounddown((vcore_opp_1_uv * 95) / 100, 6250);
		vcore_opp_2_uv = rounddown((vcore_opp_2_uv * 95) / 100, 6250);
		vcore_opp_3_uv = rounddown((vcore_opp_3_uv * 95) / 100, 6250);
		vcore_opp_4_uv = rounddown((vcore_opp_4_uv * 95) / 100, 6250);
	} else if (dvfs_v_mode == 1) {
		/* HV */
		vcore_opp_0_uv = roundup((vcore_opp_0_uv * 105) / 100, 6250);
		vcore_opp_1_uv = roundup((vcore_opp_1_uv * 105) / 100, 6250);
		vcore_opp_2_uv = roundup((vcore_opp_2_uv * 105) / 100, 6250);
		vcore_opp_3_uv = roundup((vcore_opp_3_uv * 105) / 100, 6250);
		vcore_opp_4_uv = roundup((vcore_opp_4_uv * 105) / 100, 6250);
	}

	pr_info("%s: VMODE=%d, RSV4=%x\n",
			__func__,
			dvfs_v_mode,
			dvfsrc_rsrv);

	pr_info("%s: FINAL vcore_opp_uv: %d, %d, %d, %d, %d\n",
		__func__,
		vcore_opp_0_uv,
		vcore_opp_1_uv,
		vcore_opp_2_uv,
		vcore_opp_3_uv,
		vcore_opp_4_uv);

	vcore_uv[0] = vcore_opp_0_uv;
	vcore_uv[1] = vcore_opp_1_uv;
	vcore_uv[2] = vcore_opp_2_uv;
	vcore_uv[3] = vcore_opp_3_uv;
	vcore_uv[4] = vcore_opp_4_uv;
	for (i = 0; i < VCORE_OPP_NUM; i++) {
		if (vcore_uv[i] < 400000 || vcore_uv[i] > 800000 ||
		    (vcore_uv[i] - 400000) % 6250 ||
		    (i && vcore_uv[i] > vcore_uv[i - 1]))
			return -EINVAL;
	}
	for (i = 0; i < VCORE_OPP_NUM; i++) {
		ret = dvfsrc_set_vcore_uv_table_checked(i, vcore_uv[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < DDR_OPP_NUM; i++)
		set_opp_ddr_freq(i, ddr_khz[i]);

	return 0;
}
