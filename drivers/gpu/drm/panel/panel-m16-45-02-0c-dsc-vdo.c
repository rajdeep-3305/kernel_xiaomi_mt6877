// SPDX-License-Identifier: GPL-2.0
/*
 * Panel driver for Xiaomi M16 45-02-0C DSC VDO panel
 * 1080x2400 @ 60/90/120 Hz, MIPI DSI, 4 lanes
 *
 * Copyright (c) 2019 MediaTek Inc.
 * Ported to upstream DRM panel framework.
 */

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>

#include "../mediatek/mtk_drm_graphics_base.h"
#include "../mediatek/mtk_panel_ext.h"
#if defined(CONFIG_MI_DISP) && defined(CONFIG_MI_DISP_ESD_CHECK)
extern void mi_dsi_panel_tigger_dimming_work(struct mtk_dsi *dsi);
#endif

#define PHYSICAL_WIDTH		69552
#define PHYSICAL_HEIGHT		154560

#define DATA_RATE		831
#define FRAME_WIDTH		1080
#define FRAME_HEIGHT		2400
#define DEFAULT_BL_LEVEL	511

static unsigned int rc_buf_thresh[14] = {
	896, 1792, 2688, 3584, 4480, 5376, 6272,
	6720, 7168, 7616, 7744, 7872, 8000, 8064
};

static unsigned int range_min_qp[15] = {
	0, 0, 1, 1, 3, 3, 3, 3, 3, 3, 5, 5, 5, 7, 13
};

static unsigned int range_max_qp[15] = {
	4, 4, 5, 6, 7, 7, 7, 8, 9, 10, 11, 12, 13, 13, 15
};

static int range_bpg_ofs[15] = {
	2, 0, 0, -2, -4, -6, -8, -8, -8, -10, -10, -12, -12, -12, -12
};

static const char *panel_name = "panel_name=dsi_m16_45_02_0c_dsc_vdo";

struct lcm {
	struct device *dev;
	struct drm_panel panel;
	struct drm_connector *connector;
	struct gpio_desc *reset_gpio;
	struct regulator *vci;
	struct gpio_desc *vddi_gpio;
	struct gpio_desc *vdd_gpio;
	bool vci_enabled;
	bool prepared;
	const char *panel_info;
	int dynamic_fps;
	unsigned int last_non_zero_bl_level;
	/* Protect panel command sequences. */
	struct mutex panel_lock;
};

static struct drm_panel *this_panel;

static inline struct lcm *panel_to_lcm(struct drm_panel *panel)
{
	return container_of(panel, struct lcm, panel);
}

#ifdef ENABLE_30HZ
static struct mtk_panel_params ext_params_30hz = {
	.lcm_index = 2,
	.pll_clk = DATA_RATE / 2,
	.data_rate = DATA_RATE,
#ifdef CONFIG_MI_DISP_FOD_SYNC
	.bl_sync_enable = 1,
	.aod_delay_enable = 1,
#endif
	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x91,
		.count = 2,
		.para_list[0] = 0x89,
		.para_list[1] = 0x28,
	},
	.is_cphy = 0,
	.lcm_color_mode = MTK_DRM_COLOR_MODE_DISPLAY_P3,
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 1,
		.rgb_swap = 0,
		.dsc_cfg = 34,
		.rct_on = 1,
		.bit_per_channel = 8,
		.dsc_line_buf_depth = 9,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = FRAME_HEIGHT,
		.pic_width = FRAME_WIDTH,
		.slice_height = 20,
		.slice_width = 540,
		.chunk_size = 540,
		.xmit_delay = 512,
		.dec_delay = 526,
		.scale_value = 32,
		.increment_interval = 488,
		.decrement_interval = 7,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 1294,
		.slice_bpg_offset = 1302,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 3,
		.flatness_maxqp = 12,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 11,
		.rc_quant_incr_limit1 = 11,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,
		.ext_pps_cfg = {
			.enable = 1,
			.rc_buf_thresh = rc_buf_thresh,
			.range_min_qp = range_min_qp,
			.range_max_qp = range_max_qp,
			.range_bpg_ofs = range_bpg_ofs,
		},
	},
	.dyn_fps = {
		.switch_en = 1,
		.vact_timing_fps = 30,
		.cmds_counts_switch_fps = 30,
		.short_dfps_cmds_counts = 4,
		.long_dfps_cmds_counts = 4,
		.dfps_cmd_table[0] = { 0, 2, { 0x2F, 0x00 } },
		.dfps_cmd_table[1] = { 0, 6, { 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01 } },
		.dfps_cmd_table[2] = { 0, 2, { 0xEA, 0x11 } },
		.dfps_cmd_table[3] = { 0, 6, { 0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00 } },
	},
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
};
#endif

static struct mtk_panel_params ext_params_60hz = {
	.lcm_index = 2,
	.pll_clk = DATA_RATE / 2,
	.data_rate = DATA_RATE,
#ifdef CONFIG_MI_DISP_FOD_SYNC
	.bl_sync_enable = 1,
	.aod_delay_enable = 1,
#endif
	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x91,
		.count = 2,
		.para_list[0] = 0x89,
		.para_list[1] = 0x28,
	},
	.is_cphy = 0,
	.lcm_color_mode = MTK_DRM_COLOR_MODE_DISPLAY_P3,
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 1,
		.rgb_swap = 0,
		.dsc_cfg = 34,
		.rct_on = 1,
		.bit_per_channel = 8,
		.dsc_line_buf_depth = 9,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = FRAME_HEIGHT,
		.pic_width = FRAME_WIDTH,
		.slice_height = 20,
		.slice_width = 540,
		.chunk_size = 540,
		.xmit_delay = 512,
		.dec_delay = 526,
		.scale_value = 32,
		.increment_interval = 488,
		.decrement_interval = 7,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 1294,
		.slice_bpg_offset = 1302,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 3,
		.flatness_maxqp = 12,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 11,
		.rc_quant_incr_limit1 = 11,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,
		.ext_pps_cfg = {
			.enable = 1,
			.rc_buf_thresh = rc_buf_thresh,
			.range_min_qp = range_min_qp,
			.range_max_qp = range_max_qp,
			.range_bpg_ofs = range_bpg_ofs,
		},
	},
	.dyn_fps = {
		.switch_en = 1,
		.vact_timing_fps = 60,
		.cmds_counts_switch_fps = 30,
		.short_dfps_cmds_counts = 1,
		.long_dfps_cmds_counts = 4,
		.dfps_cmd_table[0] = { 0, 2, { 0x2F, 0x02 } },
		.dfps_cmd_table[1] = { 0, 6, { 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01 } },
		.dfps_cmd_table[2] = { 0, 2, { 0xEA, 0x91 } },
		.dfps_cmd_table[3] = { 0, 6, { 0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00 } },
	},
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
};

static struct mtk_panel_params ext_params_90hz = {
	.lcm_index = 2,
	.pll_clk = DATA_RATE / 2,
	.data_rate = DATA_RATE,
#ifdef CONFIG_MI_DISP_FOD_SYNC
	.bl_sync_enable = 1,
	.aod_delay_enable = 1,
#endif
	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x91,
		.count = 2,
		.para_list[0] = 0x89,
		.para_list[1] = 0x28,
	},
	.is_cphy = 0,
	.lcm_color_mode = MTK_DRM_COLOR_MODE_DISPLAY_P3,
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 1,
		.rgb_swap = 0,
		.dsc_cfg = 34,
		.rct_on = 1,
		.bit_per_channel = 8,
		.dsc_line_buf_depth = 9,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = FRAME_HEIGHT,
		.pic_width = FRAME_WIDTH,
		.slice_height = 20,
		.slice_width = 540,
		.chunk_size = 540,
		.xmit_delay = 512,
		.dec_delay = 526,
		.scale_value = 32,
		.increment_interval = 488,
		.decrement_interval = 7,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 1294,
		.slice_bpg_offset = 1302,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 3,
		.flatness_maxqp = 12,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 11,
		.rc_quant_incr_limit1 = 11,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,
		.ext_pps_cfg = {
			.enable = 1,
			.rc_buf_thresh = rc_buf_thresh,
			.range_min_qp = range_min_qp,
			.range_max_qp = range_max_qp,
			.range_bpg_ofs = range_bpg_ofs,
		},
	},
	.dyn_fps = {
		.switch_en = 1,
		.vact_timing_fps = 90,
		.cmds_counts_switch_fps = 30,
		.short_dfps_cmds_counts = 1,
		.long_dfps_cmds_counts = 4,
		.dfps_cmd_table[0] = { 0, 2, { 0x2F, 0x01 } },
		.dfps_cmd_table[1] = { 0, 6, { 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01 } },
		.dfps_cmd_table[2] = { 0, 2, { 0xEA, 0x91 } },
		.dfps_cmd_table[3] = { 0, 6, { 0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00 } },
	},
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
};

static struct mtk_panel_params ext_params_120hz = {
	.lcm_index = 2,
	.pll_clk = DATA_RATE / 2,
	.data_rate = DATA_RATE,
#ifdef CONFIG_MI_DISP_FOD_SYNC
	.bl_sync_enable = 1,
	.aod_delay_enable = 1,
#endif
	.cust_esd_check = 0,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x91,
		.count = 2,
		.para_list[0] = 0x89,
		.para_list[1] = 0x28,
	},
	.is_cphy = 0,
	.lcm_color_mode = MTK_DRM_COLOR_MODE_DISPLAY_P3,
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable = 1,
		.ver = 17,
		.slice_mode = 1,
		.rgb_swap = 0,
		.dsc_cfg = 34,
		.rct_on = 1,
		.bit_per_channel = 8,
		.dsc_line_buf_depth = 9,
		.bp_enable = 1,
		.bit_per_pixel = 128,
		.pic_height = FRAME_HEIGHT,
		.pic_width = FRAME_WIDTH,
		.slice_height = 20,
		.slice_width = 540,
		.chunk_size = 540,
		.xmit_delay = 512,
		.dec_delay = 526,
		.scale_value = 32,
		.increment_interval = 488,
		.decrement_interval = 7,
		.line_bpg_offset = 12,
		.nfl_bpg_offset = 1294,
		.slice_bpg_offset = 1302,
		.initial_offset = 6144,
		.final_offset = 4336,
		.flatness_minqp = 3,
		.flatness_maxqp = 12,
		.rc_model_size = 8192,
		.rc_edge_factor = 6,
		.rc_quant_incr_limit0 = 11,
		.rc_quant_incr_limit1 = 11,
		.rc_tgt_offset_hi = 3,
		.rc_tgt_offset_lo = 3,
		.ext_pps_cfg = {
			.enable = 1,
			.rc_buf_thresh = rc_buf_thresh,
			.range_min_qp = range_min_qp,
			.range_max_qp = range_max_qp,
			.range_bpg_ofs = range_bpg_ofs,
		},
	},
	.dyn_fps = {
		.switch_en = 1,
		.vact_timing_fps = 120,
		.cmds_counts_switch_fps = 30,
		.short_dfps_cmds_counts = 1,
		.long_dfps_cmds_counts = 4,
		.dfps_cmd_table[0] = { 0, 2, { 0x2F, 0x00 } },
		.dfps_cmd_table[1] = { 0, 6, { 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01 } },
		.dfps_cmd_table[2] = { 0, 2, { 0xEA, 0x91 } },
		.dfps_cmd_table[3] = { 0, 6, { 0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00 } },
	},
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
};

#define lcm_dcs_write_seq(dsi, seq...)				\
	do {							\
		static const u8 d[] = { seq };			\
		int ret;					\
		ret = mipi_dsi_dcs_write_buffer(dsi, d,		\
						ARRAY_SIZE(d));	\
		if (ret < 0)					\
			return ret;				\
	} while (0)

#define lcm_gen_write_seq(dsi, seq...)				\
	do {							\
		static const u8 d[] = { seq };			\
		int ret;					\
		ret = mipi_dsi_generic_write(dsi, d,		\
					     ARRAY_SIZE(d));	\
		if (ret < 0)					\
			return ret;				\
	} while (0)

static int lcm_set_refresh_rate(struct lcm *ctx, int vrefresh)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	u8 cmd[] = { 0x2F, 0x00 };
	int ret;

	switch (vrefresh) {
	case 60:
		cmd[1] = 0x02;
		break;
	case 90:
		cmd[1] = 0x01;
		break;
	case 120:
		cmd[1] = 0x00;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&ctx->panel_lock);
	ret = mipi_dsi_dcs_write_buffer(dsi, cmd, ARRAY_SIZE(cmd));
	mutex_unlock(&ctx->panel_lock);

	return ret;
}

static int lcm_init_sequence(struct lcm *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	lcm_dcs_write_seq(dsi, 0x3B, 0x00, 0x0C, 0x00, 0x34);

	lcm_gen_write_seq(dsi, 0xFF, 0xAA, 0x55, 0xA5, 0x80);
	lcm_dcs_write_seq(dsi, 0x6F, 0x19);
	lcm_gen_write_seq(dsi, 0xF2, 0x00);
	lcm_dcs_write_seq(dsi, 0x6F, 0x14);
	lcm_gen_write_seq(dsi, 0xF4, 0x40);
	lcm_dcs_write_seq(dsi, 0x6F, 0x17);
	lcm_gen_write_seq(dsi, 0xF4, 0x02);
	lcm_dcs_write_seq(dsi, 0x6F, 0x31);
	lcm_gen_write_seq(dsi, 0xFC, 0x30);

	lcm_gen_write_seq(dsi, 0xFF, 0xAA, 0x55, 0xA5, 0x81);
	lcm_dcs_write_seq(dsi, 0x6F, 0x3C);
	lcm_gen_write_seq(dsi, 0xF5, 0x81);
	lcm_dcs_write_seq(dsi, 0x6F, 0x13);
	lcm_gen_write_seq(dsi, 0xF9, 0x01);

	lcm_gen_write_seq(dsi, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	lcm_dcs_write_seq(dsi, 0x6F, 0x03);
	lcm_gen_write_seq(dsi, 0xC7, 0x07);
	lcm_dcs_write_seq(dsi, 0x6F, 0x05);
	lcm_gen_write_seq(dsi, 0xC7, 0x27, 0x08);
	lcm_dcs_write_seq(dsi, 0x6F, 0x0A);
	lcm_gen_write_seq(dsi, 0xE4, 0x90);

	lcm_dcs_write_seq(dsi, 0x17, 0x03);
	lcm_dcs_write_seq(dsi, 0x71, 0x00);
	lcm_dcs_write_seq(dsi, 0x8D,
			  0x00, 0x00, 0x04, 0x37, 0x00, 0x00, 0x09, 0x5F);

	ret = lcm_set_refresh_rate(ctx, ctx->dynamic_fps);
	if (ret < 0)
		return ret;

	lcm_dcs_write_seq(dsi, 0x2A, 0x00, 0x00, 0x04, 0x37);
	lcm_dcs_write_seq(dsi, 0x2B, 0x00, 0x00, 0x09, 0x5F);
	lcm_dcs_write_seq(dsi, 0x03, 0x00);

	/* PPS table setting */
	lcm_dcs_write_seq(dsi, 0x90, 0x03, 0x43);
	lcm_dcs_write_seq(dsi, 0x91,
			  0x89, 0x28, 0x00, 0x14, 0xC2, 0x00, 0x02, 0x0E,
			  0x01, 0xE8, 0x00, 0x07, 0x05, 0x0E, 0x05, 0x16,
			  0x10, 0xF0);
	lcm_dcs_write_seq(dsi, 0x93,
			  0x89, 0x28, 0x00, 0x14, 0xC2, 0x00, 0x02, 0x0E,
			  0x01, 0xE8, 0x00, 0x07, 0x05, 0x0E, 0x05, 0x16,
			  0x10, 0xF0);
	lcm_dcs_write_seq(dsi, 0x95,
			  0x89, 0x28, 0x00, 0x14, 0xC2, 0x00, 0x02, 0x0E,
			  0x01, 0xE8, 0x00, 0x07, 0x05, 0x0E, 0x05, 0x16,
			  0x10, 0xF0);
	lcm_dcs_write_seq(dsi, 0x97,
			  0x89, 0x28, 0x00, 0x14, 0xC2, 0x00, 0x02, 0x0E,
			  0x01, 0xE8, 0x00, 0x07, 0x05, 0x0E, 0x05, 0x16,
			  0x10, 0xF0);

	lcm_dcs_write_seq(dsi, 0x51, DEFAULT_BL_LEVEL >> 8,
			  DEFAULT_BL_LEVEL & 0xff, 0x00, 0x00, 0x00, 0x00);

	/* Dimming setting */
	lcm_gen_write_seq(dsi, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	lcm_gen_write_seq(dsi, 0xB2, 0x09);
	lcm_dcs_write_seq(dsi, 0x6F, 0x05);
	lcm_gen_write_seq(dsi, 0xB2, 0x04, 0x04);

	lcm_dcs_write_seq(dsi, 0x53, 0x20);

	/* GIR settings */
	lcm_gen_write_seq(dsi, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	lcm_dcs_write_seq(dsi, 0x6F, 0x03);
	lcm_gen_write_seq(dsi, 0xC0, 0x54);
	lcm_dcs_write_seq(dsi, 0x26, 0x03);
	lcm_dcs_write_seq(dsi, 0x5F, 0x01);

	/* cmd lock */
	lcm_gen_write_seq(dsi, 0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00);

	/* ESD config */
	lcm_gen_write_seq(dsi, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	lcm_dcs_write_seq(dsi, 0x6F, 0x01);
	lcm_gen_write_seq(dsi, 0xBE, 0x47);
	lcm_dcs_write_seq(dsi, 0x6F, 0x13);
	lcm_gen_write_seq(dsi, 0xBE, 0x00);
	lcm_dcs_write_seq(dsi, 0x6F, 0x05);
	lcm_gen_write_seq(dsi, 0xBE, 0x18);
	lcm_dcs_write_seq(dsi, 0x6F, 0x0F);
	lcm_gen_write_seq(dsi, 0xBE, 0xFB, 0xFB);

	/* Gamma code reload */
	lcm_gen_write_seq(dsi, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	lcm_gen_write_seq(dsi, 0xE8, 0x30);
	lcm_gen_write_seq(dsi, 0xFF, 0xAA, 0x55, 0xA5, 0x84);
	lcm_dcs_write_seq(dsi, 0x6F, 0x21);
	lcm_gen_write_seq(dsi, 0xF4, 0xFF, 0xFF, 0xF9, 0xFF, 0xFF, 0xFF);

	/* TE enable */
	lcm_dcs_write_seq(dsi, 0x35, 0x00);

	lcm_dcs_write_seq(dsi, 0x6F, 0x9C);
	lcm_gen_write_seq(dsi, 0xF4, 0xFF, 0xC0);

	lcm_dcs_write_seq(dsi, 0x57, 0x00);

	/* Sleep out */
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		return ret;
	msleep(80);

	/* Display on */
	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0)
		return ret;

	/* Post-on: LVDET ON */
	lcm_gen_write_seq(dsi, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01);
	lcm_dcs_write_seq(dsi, 0x6F, 0x03);
	lcm_gen_write_seq(dsi, 0xC7, 0x47);

	/* ESD CONFIG ON */
	lcm_gen_write_seq(dsi, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00);
	lcm_dcs_write_seq(dsi, 0x6F, 0x13);
	lcm_gen_write_seq(dsi, 0xBE, 0xC5);

	/* cmd 2/3 lock */
	lcm_gen_write_seq(dsi, 0xF0, 0x55, 0xAA, 0x52, 0x00, 0x00);
	lcm_gen_write_seq(dsi, 0xFF, 0xAA, 0x55, 0xA5, 0x00);

	return 0;
}

static int lcm_disable(struct drm_panel *panel)
{
	return 0;
}

static int lcm_unprepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	static const u8 esd_page[] = { 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00 };
	static const u8 esd_offset[] = { 0x6F, 0x13 };
	static const u8 esd_disable[] = { 0xBE, 0x00 };

	if (!ctx->prepared)
		return 0;

	mutex_lock(&ctx->panel_lock);
	mipi_dsi_generic_write(dsi, esd_page, ARRAY_SIZE(esd_page));
	mipi_dsi_dcs_write_buffer(dsi, esd_offset, ARRAY_SIZE(esd_offset));
	mipi_dsi_generic_write(dsi, esd_disable, ARRAY_SIZE(esd_disable));
	mipi_dsi_dcs_set_display_off(dsi);
	msleep(10);
	mipi_dsi_dcs_enter_sleep_mode(dsi);
	msleep(100);
	mutex_unlock(&ctx->panel_lock);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(1000, 1100);

	if (ctx->vci_enabled) {
		regulator_disable(ctx->vci);
		ctx->vci_enabled = false;
	}
	usleep_range(1000, 1100);

	ctx->vdd_gpio = devm_gpiod_get(ctx->dev, "vdd", GPIOD_OUT_HIGH);
	if (!IS_ERR(ctx->vdd_gpio)) {
		gpiod_set_value_cansleep(ctx->vdd_gpio, 0);
		devm_gpiod_put(ctx->dev, ctx->vdd_gpio);
		usleep_range(1000, 1100);
	}

	ctx->vddi_gpio = devm_gpiod_get(ctx->dev, "vddi", GPIOD_OUT_HIGH);
	if (!IS_ERR(ctx->vddi_gpio)) {
		gpiod_set_value_cansleep(ctx->vddi_gpio, 0);
		devm_gpiod_put(ctx->dev, ctx->vddi_gpio);
		usleep_range(1000, 1100);
	}

	ctx->prepared = false;
	return 0;
}

static int lcm_prepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret;

	if (ctx->prepared)
		return 0;

	ctx->vddi_gpio = devm_gpiod_get(ctx->dev, "vddi", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->vddi_gpio)) {
		dev_err(ctx->dev, "cannot get vddi_gpio %ld\n", PTR_ERR(ctx->vddi_gpio));
		return PTR_ERR(ctx->vddi_gpio);
	}
	gpiod_set_value_cansleep(ctx->vddi_gpio, 1);
	devm_gpiod_put(ctx->dev, ctx->vddi_gpio);
	usleep_range(1000, 1100);

	ctx->vdd_gpio = devm_gpiod_get(ctx->dev, "vdd", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->vdd_gpio)) {
		dev_err(ctx->dev, "cannot get vdd_gpio %ld\n", PTR_ERR(ctx->vdd_gpio));
		return PTR_ERR(ctx->vdd_gpio);
	}
	gpiod_set_value_cansleep(ctx->vdd_gpio, 1);
	devm_gpiod_put(ctx->dev, ctx->vdd_gpio);
	usleep_range(1000, 1100);

	if (!ctx->vci_enabled) {
		ret = regulator_enable(ctx->vci);
		if (ret < 0) {
			dev_err(ctx->dev, "Failed to enable vci: %d\n", ret);
			return ret;
		}
		ctx->vci_enabled = true;
	}
	usleep_range(10000, 10100);

	/* Reset sequence: high -> low -> high (longer delays for this panel) */
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(10);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(1000, 1100);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(11);

	ret = lcm_init_sequence(ctx);
	if (ret < 0) {
		dev_err(ctx->dev, "Panel init sequence failed: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		if (ctx->vci_enabled) {
			regulator_disable(ctx->vci);
			ctx->vci_enabled = false;
		}
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

#ifdef ENABLE_30HZ
static const struct drm_display_mode mode_30hz = {
	.clock = 186870,
	.hdisplay = 1080,
	.hsync_start = 1080 + 1424,
	.hsync_end = 1080 + 1424 + 8,
	.htotal = 1080 + 1424 + 8 + 16,
	.vdisplay = 2400,
	.vsync_start = 2400 + 52,
	.vsync_end = 2400 + 52 + 4,
	.vtotal = 2400 + 52 + 4 + 8,
	.width_mm = PHYSICAL_WIDTH / 1000,
	.height_mm = PHYSICAL_HEIGHT / 1000,
};
#endif

static const struct drm_display_mode mode_90hz = {
	.clock = 328316,
	.hdisplay = 1080,
	.hsync_start = 1080 + 20,
	.hsync_end = 1080 + 20 + 8,
	.htotal = 1080 + 20 + 8 + 16,
	.vdisplay = 2400,
	.vsync_start = 2400 + 822,
	.vsync_end = 2400 + 822 + 4,
	.vtotal = 2400 + 822 + 4 + 8,
	.width_mm = PHYSICAL_WIDTH / 1000,
	.height_mm = PHYSICAL_HEIGHT / 1000,
};

static const struct drm_display_mode mode_120hz = {
	.clock = 331162,
	.hdisplay = 1080,
	.hsync_start = 1080 + 20,
	.hsync_end = 1080 + 20 + 8,
	.htotal = 1080 + 20 + 8 + 16,
	.vdisplay = 2400,
	.vsync_start = 2400 + 52,
	.vsync_end = 2400 + 52 + 4,
	.vtotal = 2400 + 52 + 4 + 8,
	.width_mm = PHYSICAL_WIDTH / 1000,
	.height_mm = PHYSICAL_HEIGHT / 1000,
};

static const struct drm_display_mode mode_60hz = {
	.clock = 331162,
	.hdisplay = 1080,
	.hsync_start = 1080 + 20,
	.hsync_end = 1080 + 20 + 8,
	.htotal = 1080 + 20 + 8 + 16,
	.vdisplay = 2400,
	.vsync_start = 2400 + 2516,
	.vsync_end = 2400 + 2516 + 4,
	.vtotal = 2400 + 2516 + 4 + 8,
	.width_mm = PHYSICAL_WIDTH / 1000,
	.height_mm = PHYSICAL_HEIGHT / 1000,
};

static const struct drm_display_mode *lcm_modes[] = {
#ifdef ENABLE_30HZ
	&mode_30hz,
#endif
	&mode_60hz,
	&mode_90hz,
	&mode_120hz,
};

static int lcm_get_modes(struct drm_panel *panel,
			 struct drm_connector *connector)
{
	struct lcm *ctx = panel_to_lcm(panel);
	struct drm_display_mode *mode;
	int i;

	ctx->connector = connector;

	for (i = 0; i < ARRAY_SIZE(lcm_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, lcm_modes[i]);
		if (!mode)
			return -ENOMEM;

		drm_mode_set_name(mode);
		mode->type = DRM_MODE_TYPE_DRIVER;
		if (lcm_modes[i] == &mode_60hz)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = mode_60hz.width_mm;
	connector->display_info.height_mm = mode_60hz.height_mm;

	return ARRAY_SIZE(lcm_modes);
}

static struct drm_display_mode *
get_mode_by_id(struct drm_connector *connector, unsigned int mode)
{
	struct drm_display_mode *m;
	unsigned int i = 0;

	if (!connector)
		return NULL;

	list_for_each_entry(m, &connector->modes, head) {
		if (i == mode)
			return m;
		i++;
	}

	return NULL;
}

static int lcm_ext_reset(struct drm_panel *panel, int on)
{
	struct lcm *ctx = panel_to_lcm(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, on);

	return 0;
}

static int lcm_setbacklight_cmdq(void *dsi, dcs_write_gce cb, void *handle,
				 unsigned int level)
{
	u8 payload[] = { 0x51, level >> 8, level & 0xff };
	struct lcm *ctx;

	if (!cb || !this_panel)
		return -EINVAL;

	ctx = panel_to_lcm(this_panel);
	mutex_lock(&ctx->panel_lock);
	cb(dsi, handle, payload, ARRAY_SIZE(payload));
	if (level)
		ctx->last_non_zero_bl_level = level;
	mutex_unlock(&ctx->panel_lock);

	return 0;
}

static int lcm_get_virtual_height(void)
{
	return FRAME_HEIGHT;
}

static int lcm_get_virtual_width(void)
{
	return FRAME_WIDTH;
}

static int lcm_ext_param_set(struct drm_panel *panel, unsigned int mode)
{
	struct mtk_panel_ext *ext = find_panel_ext(panel);
	struct lcm *ctx = panel_to_lcm(panel);
	struct drm_display_mode *m = get_mode_by_id(ctx->connector, mode);
	int vrefresh;

	if (!ext || !m)
		return -EINVAL;

	vrefresh = drm_mode_vrefresh(m);

#ifdef ENABLE_30HZ
	if (vrefresh == 30)
		ext->params = &ext_params_30hz;
	else if (vrefresh == 60)
#else
	if (vrefresh == 60)
#endif
		ext->params = &ext_params_60hz;
	else if (vrefresh == 90)
		ext->params = &ext_params_90hz;
	else if (vrefresh == 120)
		ext->params = &ext_params_120hz;
	else
		return -EINVAL;

	ctx->dynamic_fps = vrefresh;
	return 0;
}

static int lcm_mode_switch(struct drm_panel *panel, unsigned int cur_mode,
			   unsigned int dst_mode, enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	struct lcm *ctx = panel_to_lcm(panel);
	struct drm_display_mode *m = get_mode_by_id(ctx->connector, dst_mode);
	int ret, vrefresh;

	if (stage != AFTER_DSI_POWERON)
		return 0;

	if (!m)
		return -EINVAL;

	vrefresh = drm_mode_vrefresh(m);

	ret = lcm_set_refresh_rate(ctx, vrefresh);
	if (ret < 0)
		return ret;

	ctx->dynamic_fps = vrefresh;
	return 0;
}

static int lcm_get_panel_info(struct drm_panel *panel, char *buf)
{
	struct lcm *ctx = panel_to_lcm(panel);

	return scnprintf(buf, PAGE_SIZE, "%s\n", ctx->panel_info);
}

static bool lcm_get_panel_initialized(struct drm_panel *panel)
{
	if (!panel)
		return false;

	return panel_to_lcm(panel)->prepared;
}

static int lcm_get_panel_dynamic_fps(struct drm_panel *panel, u32 *fps)
{
	if (!panel || !fps)
		return -EINVAL;

	*fps = panel_to_lcm(panel)->dynamic_fps;
	return 0;
}

static int lcm_get_panel_thermal_dimming_enable(struct drm_panel *panel,
						 bool *enabled)
{
	if (!panel || !enabled)
		return -EINVAL;

	*enabled = true;
	return 0;
}

#if defined(CONFIG_MI_DISP) && defined(CONFIG_MI_DISP_ESD_CHECK)
static void lcm_esd_restore_backlight(struct mtk_dsi *dsi,
				      struct drm_panel *panel)
{
	struct lcm *ctx;
	struct mipi_dsi_device *mipi_dsi;
	u8 payload[3];
	int ret;

	if (!dsi || !panel)
		return;

	ctx = panel_to_lcm(panel);
	if (!ctx->prepared)
		return;

	mipi_dsi = to_mipi_dsi_device(ctx->dev);

	mutex_lock(&ctx->panel_lock);
	payload[0] = 0x51;
	payload[1] = ctx->last_non_zero_bl_level >> 8;
	payload[2] = ctx->last_non_zero_bl_level & 0xff;
	ret = mipi_dsi_dcs_write_buffer(mipi_dsi, payload,
					ARRAY_SIZE(payload));
	mutex_unlock(&ctx->panel_lock);
	if (ret < 0) {
		dev_err(ctx->dev, "failed to restore backlight: %d\n", ret);
		return;
	}

	mi_dsi_panel_tigger_dimming_work(dsi);
}
#endif

static struct mtk_panel_funcs lcm_ext_funcs = {
	.reset = lcm_ext_reset,
	.set_backlight_cmdq = lcm_setbacklight_cmdq,
	.get_virtual_heigh = lcm_get_virtual_height,
	.get_virtual_width = lcm_get_virtual_width,
	.ext_param_set = lcm_ext_param_set,
	.mode_switch = lcm_mode_switch,
	.get_panel_info = lcm_get_panel_info,
	.get_panel_initialized = lcm_get_panel_initialized,
	.get_panel_dynamic_fps = lcm_get_panel_dynamic_fps,
	.get_panel_thermal_dimming_enable =
		lcm_get_panel_thermal_dimming_enable,
#if defined(CONFIG_MI_DISP) && defined(CONFIG_MI_DISP_ESD_CHECK)
	.esd_restore_backlight = lcm_esd_restore_backlight,
#endif
};

static const struct drm_panel_funcs lcm_panel_funcs = {
	.disable = lcm_disable,
	.unprepare = lcm_unprepare,
	.prepare = lcm_prepare,
	.get_modes = lcm_get_modes,
};

static int lcm_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;
	struct lcm *ctx;
	int ret;

	dsi_node = of_get_parent(dev->of_node);
	if (dsi_node) {
		endpoint = of_graph_get_next_endpoint(dsi_node, NULL);
		if (endpoint) {
			remote_node = of_graph_get_remote_port_parent(endpoint);
			of_node_put(endpoint);
			if (!remote_node) {
				of_node_put(dsi_node);
				return -ENODEV;
			}
		}
	}
	of_node_put(dsi_node);
	if (remote_node != dev->of_node) {
		of_node_put(remote_node);
		return -ENODEV;
	}
	of_node_put(remote_node);

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = dev;

	mutex_init(&ctx->panel_lock);

	ctx->vci = devm_regulator_get(dev, "vci");
	if (IS_ERR(ctx->vci))
		return dev_err_probe(dev, PTR_ERR(ctx->vci),
				     "Failed to get vci regulator\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset GPIO\n");

	ret = regulator_enable(ctx->vci);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to enable vci\n");
	ctx->vci_enabled = true;

	/* LK leaves the panel initialized even when the AP pipeline is reset. */
	ctx->prepared = true;
	ctx->panel_info = panel_name;
	ctx->dynamic_fps = 60;
	ctx->last_non_zero_bl_level = DEFAULT_BL_LEVEL;

	ext_params_60hz.err_flag_irq_gpio = of_get_named_gpio(
		dev->of_node, "mi,esd-err-irq-gpio", 0);
	ext_params_60hz.err_flag_irq_flags =
		IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
#ifdef ENABLE_30HZ
	ext_params_30hz.err_flag_irq_gpio = ext_params_60hz.err_flag_irq_gpio;
	ext_params_30hz.err_flag_irq_flags = ext_params_60hz.err_flag_irq_flags;
#endif
	ext_params_90hz.err_flag_irq_gpio = ext_params_60hz.err_flag_irq_gpio;
	ext_params_90hz.err_flag_irq_flags = ext_params_60hz.err_flag_irq_flags;
	ext_params_120hz.err_flag_irq_gpio = ext_params_60hz.err_flag_irq_gpio;
	ext_params_120hz.err_flag_irq_flags = ext_params_60hz.err_flag_irq_flags;

	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST
			| MIPI_DSI_MODE_LPM
			| MIPI_DSI_CLOCK_NON_CONTINUOUS;

	drm_panel_init(&ctx->panel, dev, &lcm_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	this_panel = &ctx->panel;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		goto err_disable_vci;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		goto err_disable_vci;
	}

	ret = mtk_panel_ext_create(dev, &ext_params_60hz, &lcm_ext_funcs,
				   &ctx->panel);
	if (ret < 0) {
		mipi_dsi_detach(dsi);
		drm_panel_remove(&ctx->panel);
		goto err_disable_vci;
	}

	dev_info(dev, "Panel m16-45-02-0c probed successfully\n");
	return 0;

err_disable_vci:
	this_panel = NULL;
	if (ctx->vci_enabled) {
		regulator_disable(ctx->vci);
		ctx->vci_enabled = false;
	}
	ctx->prepared = false;
	return dev_err_probe(dev, ret, "Failed to probe panel\n");
}

static void lcm_remove(struct mipi_dsi_device *dsi)
{
	struct lcm *ctx = mipi_dsi_get_drvdata(dsi);

	this_panel = NULL;
	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
	if (ctx->vci_enabled) {
		regulator_disable(ctx->vci);
		ctx->vci_enabled = false;
	}
}

static const struct of_device_id lcm_of_match[] = {
	{ .compatible = "m16_45_02_0c_dsc_vdo,lcm" },
	{ }
};
MODULE_DEVICE_TABLE(of, lcm_of_match);

static struct mipi_dsi_driver lcm_driver = {
	.probe = lcm_probe,
	.remove = lcm_remove,
	.driver = {
		.name = "panel-m16-45-02-0c-dsc-vdo",
		.of_match_table = lcm_of_match,
	},
};
module_mipi_dsi_driver(lcm_driver);

MODULE_AUTHOR("Aeron-Aeron");
MODULE_DESCRIPTION("M16 45-02-0C DSC VDO Panel Driver");
MODULE_LICENSE("GPL");
