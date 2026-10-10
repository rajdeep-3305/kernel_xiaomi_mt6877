/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_MEDIATEK_MT6877_PM_DOMAINS_H
#define __SOC_MEDIATEK_MT6877_PM_DOMAINS_H

#include <dt-bindings/power/mt6877-power.h>

#include "mtk-pm-domains.h"

#define MT6877_CAM_PROT_STEP1_0_MASK	BIT(0)
#define MT6877_CAM_PROT_STEP1_1_MASK	(BIT(0) | BIT(2))
#define MT6877_CAM_PROT_STEP2_0_MASK	BIT(22)
#define MT6877_CAM_PROT_STEP2_1_MASK	(BIT(1) | BIT(3))

static const struct scpsys_domain_data scpsys_domain_data_mt6877_camera[] = {
	[MT6877_POWER_DOMAIN_CAM] = {
		.name = "cam",
		.sta_mask = BIT(23),
		.ctl_offs = 0x0e5c,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(12),
		.bp_infracfg = {
			BUS_PROT_WR_IGN(MT6877_CAM_PROT_STEP1_1_MASK,
					0x02d4, 0x02d8, 0x02ec),
			BUS_PROT_WR_IGN(MT6877_CAM_PROT_STEP1_0_MASK,
					0x0714, 0x0718, 0x0724),
			BUS_PROT_WR_IGN(MT6877_CAM_PROT_STEP2_1_MASK,
					0x02d4, 0x02d8, 0x02ec),
			BUS_PROT_WR_IGN(MT6877_CAM_PROT_STEP2_0_MASK,
					0x02a8, 0x02ac, 0x0258),
		},
		.pwr_sta_offs = 0x0ef0,
		.pwr_sta2nd_offs = 0x0ef4,
	},
	[MT6877_POWER_DOMAIN_CAM_RAWA] = {
		.name = "cam_rawa",
		.sta_mask = BIT(24),
		.ctl_offs = 0x0e60,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(12),
		.pwr_sta_offs = 0x0ef0,
		.pwr_sta2nd_offs = 0x0ef4,
	},
	[MT6877_POWER_DOMAIN_CAM_RAWB] = {
		.name = "cam_rawb",
		.sta_mask = BIT(25),
		.ctl_offs = 0x0e64,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(12),
		.pwr_sta_offs = 0x0ef0,
		.pwr_sta2nd_offs = 0x0ef4,
	},
	[MT6877_POWER_DOMAIN_CSI] = {
		.name = "csi",
		.sta_mask = BIT(30),
		.ctl_offs = 0x0e78,
		.pwr_sta_offs = 0x0ef0,
		.pwr_sta2nd_offs = 0x0ef4,
	},
};

static const struct scpsys_soc_data mt6877_camera_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6877_camera,
	.num_domains = MT6877_POWER_DOMAIN_NR,
};

#endif /* __SOC_MEDIATEK_MT6877_PM_DOMAINS_H */
