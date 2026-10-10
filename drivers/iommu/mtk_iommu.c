// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2016 MediaTek Inc.
 * Author: Yong Wu <yong.wu@mediatek.com>
 */
#include <linux/bitfield.h>
#include <linux/bug.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/dma-direct.h>
#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/dma-map-ops.h>
#include <linux/mtk_apu_iommu.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/soc/mediatek/infracfg.h>
#include <asm/barrier.h>
#include <soc/mediatek/smi.h>

#include "mtk_iommu.h"

#if IS_REACHABLE(CONFIG_MTK_APUSYS_SUPPORT) && IS_ENABLED(CONFIG_MACH_MT6877)
#include "../misc/mediatek/apusys/power/2.0/apusys_power.h"
#endif

#define REG_MMU_PT_BASE_ADDR			0x000
#define MMU_PT_ADDR_MASK			GENMASK(31, 7)

#define REG_MMU_INVALIDATE			0x020
#define F_ALL_INVLD				0x2
#define F_MMU_INV_RANGE				0x1

#define REG_MMU_INVLD_START_A			0x024
#define REG_MMU_INVLD_END_A			0x028

#define REG_MMU_INV_SEL_GEN2			0x02c
#define REG_MMU_INV_SEL_GEN1			0x038
#define F_INVLD_EN0				BIT(0)
#define F_INVLD_EN1				BIT(1)

#define REG_MMU_MISC_CTRL			0x048
#define F_MMU_COHERENCE_EN_MASK		(BIT(0) | BIT(16))
#define F_MMU_IN_ORDER_WR_EN_MASK		(BIT(1) | BIT(17))
#define F_MMU_STANDARD_AXI_MODE_MASK		(BIT(3) | BIT(19))

#define REG_MMU_DCM_DIS				0x050
#define F_MMU_DCM				BIT(8)
#define REG_MMU_WR_LEN_CTRL			0x054
#define F_MMU_WR_THROT_DIS_MASK			(BIT(5) | BIT(21))

#define REG_MMU_CTRL_REG			0x110
#define F_MMU_TF_PROT_TO_PROGRAM_ADDR		(2 << 4)
#define F_MMU_PREFETCH_RT_REPLACE_MOD		BIT(4)
#define F_MMU_TF_PROT_TO_PROGRAM_ADDR_MT8173	(2 << 5)

#define REG_MMU_IVRP_PADDR			0x114

#define REG_MMU_VLD_PA_RNG			0x118
#define F_MMU_VLD_PA_RNG(EA, SA)		(((EA) << 8) | (SA))

#define REG_MMU_INT_CONTROL0			0x120
#define F_L2_MULIT_HIT_EN			BIT(0)
#define F_TABLE_WALK_FAULT_INT_EN		BIT(1)
#define F_PREETCH_FIFO_OVERFLOW_INT_EN		BIT(2)
#define F_MISS_FIFO_OVERFLOW_INT_EN		BIT(3)
#define F_PREFETCH_FIFO_ERR_INT_EN		BIT(5)
#define F_MISS_FIFO_ERR_INT_EN			BIT(6)
#define F_INT_CLR_BIT				BIT(12)

#define REG_MMU_INT_MAIN_CONTROL		0x124
						/* mmu0 | mmu1 */
#define F_INT_TRANSLATION_FAULT			(BIT(0) | BIT(7))
#define F_INT_MAIN_MULTI_HIT_FAULT		(BIT(1) | BIT(8))
#define F_INT_INVALID_PA_FAULT			(BIT(2) | BIT(9))
#define F_INT_ENTRY_REPLACEMENT_FAULT		(BIT(3) | BIT(10))
#define F_INT_TLB_MISS_FAULT			(BIT(4) | BIT(11))
#define F_INT_MISS_TRANSACTION_FIFO_FAULT	(BIT(5) | BIT(12))
#define F_INT_PRETETCH_TRANSATION_FIFO_FAULT	(BIT(6) | BIT(13))

#define REG_MMU_CPE_DONE			0x12C

#define REG_MMU_FAULT_ST1			0x134
#define F_REG_MMU0_FAULT_MASK			GENMASK(6, 0)
#define F_REG_MMU1_FAULT_MASK			GENMASK(13, 7)

#define REG_MMU0_FAULT_VA			0x13c
#define F_MMU_INVAL_VA_31_12_MASK		GENMASK(31, 12)
#define F_MMU_INVAL_VA_34_32_MASK		GENMASK(11, 9)
#define F_MMU_INVAL_PA_34_32_MASK		GENMASK(8, 6)
#define F_MMU_FAULT_VA_WRITE_BIT		BIT(1)
#define F_MMU_FAULT_VA_LAYER_BIT		BIT(0)

#define REG_MMU0_INVLD_PA			0x140
#define REG_MMU1_FAULT_VA			0x144
#define REG_MMU1_INVLD_PA			0x148
#define REG_MMU0_INT_ID				0x150
#define REG_MMU1_INT_ID				0x154
#define F_MMU_INT_ID_COMM_ID(a)			(((a) >> 9) & 0x7)
#define F_MMU_INT_ID_SUB_COMM_ID(a)		(((a) >> 7) & 0x3)
#define F_MMU_INT_ID_LARB_ID(a)			(((a) >> 7) & 0x7)
#define F_MMU_INT_ID_PORT_ID(a)			(((a) >> 2) & 0x1f)

#define MTK_PROTECT_PA_ALIGN			256

#define HAS_4GB_MODE			BIT(0)
/* HW will use the EMI clock if there isn't the "bclk". */
#define HAS_BCLK			BIT(1)
#define HAS_VLD_PA_RNG			BIT(2)
#define RESET_AXI			BIT(3)
#define OUT_ORDER_WR_EN			BIT(4)
#define HAS_SUB_COMM			BIT(5)
#define WR_THROT_EN			BIT(6)
#define HAS_LEGACY_IVRP_PADDR		BIT(7)
#define IOVA_34_EN			BIT(8)
#define APU_IOMMU			BIT(9)

#define MTK_IOMMU_HAS_FLAG(pdata, _x) \
		((((pdata)->flags) & (_x)) == (_x))

struct mtk_iommu_domain {
	struct io_pgtable_cfg		cfg;
	struct io_pgtable_ops		*iop;

	struct mtk_iommu_data		*data;
	struct iommu_domain		domain;
};

static const struct iommu_ops mtk_iommu_ops;

static int mtk_iommu_hw_init(const struct mtk_iommu_data *data);

#define MTK_IOMMU_TLB_ADDR(iova) ({					\
	dma_addr_t _addr = iova;					\
	((lower_32_bits(_addr) & GENMASK(31, 12)) | upper_32_bits(_addr));\
})

/*
 * In M4U 4GB mode, the physical address is remapped as below:
 *
 * CPU Physical address:
 * ====================
 *
 * 0      1G       2G     3G       4G     5G
 * |---A---|---B---|---C---|---D---|---E---|
 * +--I/O--+------------Memory-------------+
 *
 * IOMMU output physical address:
 *  =============================
 *
 *                                 4G      5G     6G      7G      8G
 *                                 |---E---|---B---|---C---|---D---|
 *                                 +------------Memory-------------+
 *
 * The Region 'A'(I/O) can NOT be mapped by M4U; For Region 'B'/'C'/'D', the
 * bit32 of the CPU physical address always is needed to set, and for Region
 * 'E', the CPU physical address keep as is.
 * Additionally, The iommu consumers always use the CPU phyiscal address.
 */
#define MTK_IOMMU_4GB_MODE_REMAP_BASE	 0x140000000UL

static LIST_HEAD(m4ulist);	/* List all the M4U HWs */

#define for_each_m4u(data)	list_for_each_entry(data, &m4ulist, list)

struct mtk_iommu_iova_region {
	dma_addr_t		iova_base;
	unsigned long long	size;
};

static const struct mtk_iommu_iova_region single_domain[] = {
	{.iova_base = 0,		.size = SZ_4G},
};

static const struct mtk_iommu_iova_region mt8192_multi_dom[] = {
	{ .iova_base = 0x0,		.size = SZ_4G},		/* disp: 0 ~ 4G */
	#if IS_ENABLED(CONFIG_ARCH_DMA_ADDR_T_64BIT)
	{ .iova_base = SZ_4G,		.size = SZ_4G},		/* vdec: 4G ~ 8G */
	{ .iova_base = SZ_4G * 2,	.size = SZ_4G},		/* CAM/MDP: 8G ~ 12G */
	{ .iova_base = 0x240000000ULL,	.size = 0x4000000},	/* CCU0 */
	{ .iova_base = 0x244000000ULL,	.size = 0x4000000},	/* CCU1 */
	#endif
};

#if IS_REACHABLE(CONFIG_MTK_APUSYS_SUPPORT) && IS_ENABLED(CONFIG_MACH_MT6877)
/* Ruby 4.19 MT6877 firmware contract; A34x's VLM/code apertures differ. */
static const struct mtk_iommu_iova_region mt6877_apu_domains[] = {
	{ .iova_base = 0x300000000ULL, .size = SZ_4G }, /* data */
	{ .iova_base = 0x370000000ULL, .size = 0x12600000 }, /* VPU code */
	{ .iova_base = 0x308000000ULL, .size = SZ_32M }, /* reviser VLM */
	{ .iova_base = 0x310000000ULL, .size = SZ_256M }, /* reserved data */
};

static const u32 mt6877_apu_ports[4][MTK_LARB_NR_MAX] = {
	[0] = { [21] = BIT(1) },
	[1] = { [21] = BIT(0) },
	[2] = { [21] = BIT(2) },
};
#endif

/*
 * MT6877 assigns display, video and MDP masters to separate 4 GiB IOVA
 * regions.  The DT port IDs identify the larb and port; use that topology
 * instead of relying on dma-ranges on leaf devices.
 */
static const u32 mt6877_larb_region_msk[5][MTK_LARB_NR_MAX] = {
	[0] = { ~0U, ~0U },			/* display: larb0/1 */
	[1] = { [4] = ~0U, [7] = ~0U },	/* video: larb4/7 */
	[2] = {
		[2] = ~0U,
		[9] = ~0U,
		[11] = ~0U,
		[13] = (u32)~(BIT(9) | BIT(10)),
		[14] = (u32)~(BIT(4) | BIT(5)),
		[16] = ~0U,
		[17] = ~0U,
		[18] = ~0U,
		[19] = ~0U,
		[20] = ~0U,
	},
	[3] = { [13] = BIT(9) | BIT(10) },	/* CCU0 */
	[4] = { [14] = BIT(4) | BIT(5) },	/* CCU1 */
};

/*
 * There may be 1 or 2 M4U HWs, But we always expect they are in the same domain
 * for the performance.
 *
 * Return the first probed M4U sharing this page table. The multimedia
 * and APU IOMMUs have independent tables and power lifetimes.
 */
static struct mtk_iommu_data *mtk_iommu_get_m4u_data(struct mtk_iommu_data *match)
{
	struct mtk_iommu_data *data;

	for_each_m4u(data)
		if ((data->plat_data->flags & APU_IOMMU) ==
		    (match->plat_data->flags & APU_IOMMU))
			return data;

	return NULL;
}

static struct mtk_iommu_domain *to_mtk_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct mtk_iommu_domain, domain);
}

/* The caller must already have this instance's power and clocks enabled. */
static void mtk_iommu_tlb_flush_local(struct mtk_iommu_data *data)
{
	unsigned long flags;

	spin_lock_irqsave(&data->tlb_lock, flags);
	writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0,
		       data->base + data->plat_data->inv_sel_reg);
	writel_relaxed(F_ALL_INVLD, data->base + REG_MMU_INVALIDATE);
	wmb(); /* Order the invalidate before releasing the lock/power reference. */
	spin_unlock_irqrestore(&data->tlb_lock, flags);
}

static void mtk_iommu_tlb_flush_all(struct mtk_iommu_data *data)
{
	bool apu = MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU);
	unsigned long flags;

	for_each_m4u(data) {
		if (apu != MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
			continue;
		if (apu) {
			spin_lock_irqsave(&data->tlb_lock, flags);
			if (data->apu_powered) {
				writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0,
					       data->base + data->plat_data->inv_sel_reg);
				writel_relaxed(F_ALL_INVLD, data->base + REG_MMU_INVALIDATE);
				wmb(); /* Order invalidation before power-off can take the lock. */
			}
			spin_unlock_irqrestore(&data->tlb_lock, flags);
			continue;
		}
		if (pm_runtime_get_if_in_use(data->dev) <= 0)
			continue;

		mtk_iommu_tlb_flush_local(data);

		pm_runtime_put(data->dev);
	}
}

static void mtk_iommu_tlb_flush_range_sync(unsigned long iova, size_t size,
					   size_t granule,
					   struct mtk_iommu_data *data)
{
	unsigned long flags;
	int ret;
	u32 tmp;
	bool apu = MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU);

	/* Power callbacks restore/flush APU tables; never wake APU for unmap. */
	if (apu) {
		mtk_iommu_tlb_flush_all(data);
		return;
	}

	for_each_m4u(data) {
		if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
			continue;
		/* Even without a genpd, runtime suspend can gate the bus clock. */
		if (pm_runtime_get_if_in_use(data->dev) <= 0)
			continue;

		spin_lock_irqsave(&data->tlb_lock, flags);
		writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0,
			       data->base + data->plat_data->inv_sel_reg);

		writel_relaxed(MTK_IOMMU_TLB_ADDR(iova),
			       data->base + REG_MMU_INVLD_START_A);
		writel_relaxed(MTK_IOMMU_TLB_ADDR(iova + size - 1),
			       data->base + REG_MMU_INVLD_END_A);
		writel_relaxed(F_MMU_INV_RANGE,
			       data->base + REG_MMU_INVALIDATE);

		/* tlb sync */
		ret = readl_poll_timeout_atomic(data->base + REG_MMU_CPE_DONE,
						tmp, tmp != 0, 10, 1000);
		/* Clear the CPE status before releasing the range-flush lock. */
		writel_relaxed(0, data->base + REG_MMU_CPE_DONE);
		spin_unlock_irqrestore(&data->tlb_lock, flags);

		if (ret) {
			dev_warn(data->dev,
				 "Partial TLB flush timed out, falling back to full flush\n");
			mtk_iommu_tlb_flush_local(data);
		}

		pm_runtime_put(data->dev);
	}
}

static irqreturn_t mtk_iommu_isr(int irq, void *dev_id)
{
	struct mtk_iommu_data *data = dev_id;
	struct mtk_iommu_domain *dom = data->m4u_dom;
	unsigned int fault_larb, fault_port, sub_comm = 0;
	unsigned long flags;
	u32 int_state, regval, va34_32, pa34_32;
	u64 fault_iova, fault_pa;
	bool layer, write;

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU) &&
	    !READ_ONCE(data->apu_powered))
		return IRQ_NONE;

	/* Read error info from registers */
	int_state = readl_relaxed(data->base + REG_MMU_FAULT_ST1);
	if (int_state & F_REG_MMU0_FAULT_MASK) {
		regval = readl_relaxed(data->base + REG_MMU0_INT_ID);
		fault_iova = readl_relaxed(data->base + REG_MMU0_FAULT_VA);
		fault_pa = readl_relaxed(data->base + REG_MMU0_INVLD_PA);
	} else {
		regval = readl_relaxed(data->base + REG_MMU1_INT_ID);
		fault_iova = readl_relaxed(data->base + REG_MMU1_FAULT_VA);
		fault_pa = readl_relaxed(data->base + REG_MMU1_INVLD_PA);
	}
	layer = fault_iova & F_MMU_FAULT_VA_LAYER_BIT;
	write = fault_iova & F_MMU_FAULT_VA_WRITE_BIT;
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, IOVA_34_EN)) {
		va34_32 = FIELD_GET(F_MMU_INVAL_VA_34_32_MASK, fault_iova);
		pa34_32 = FIELD_GET(F_MMU_INVAL_PA_34_32_MASK, fault_iova);
		fault_iova = fault_iova & F_MMU_INVAL_VA_31_12_MASK;
		fault_iova |= (u64)va34_32 << 32;
		fault_pa |= (u64)pa34_32 << 32;
	}

	fault_port = F_MMU_INT_ID_PORT_ID(regval);
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, HAS_SUB_COMM)) {
		fault_larb = F_MMU_INT_ID_COMM_ID(regval);
		sub_comm = F_MMU_INT_ID_SUB_COMM_ID(regval);
	} else {
		fault_larb = F_MMU_INT_ID_LARB_ID(regval);
	}
	fault_larb = data->plat_data->larbid_remap[fault_larb][sub_comm];

	if (!dom || report_iommu_fault(&dom->domain, data->dev, fault_iova,
			       write ? IOMMU_FAULT_WRITE : IOMMU_FAULT_READ)) {
		dev_err_ratelimited(
			data->dev,
			"fault type=0x%x iova=0x%llx pa=0x%llx larb=%d port=%d layer=%d %s\n",
			int_state, fault_iova, fault_pa, fault_larb, fault_port,
			layer, write ? "write" : "read");
	}

	/* Serialize the enable-mask RMW with register restore. */
	spin_lock_irqsave(&data->tlb_lock, flags);
	regval = readl_relaxed(data->base + REG_MMU_INT_CONTROL0);
	regval |= F_INT_CLR_BIT;
	writel_relaxed(regval, data->base + REG_MMU_INT_CONTROL0);
	spin_unlock_irqrestore(&data->tlb_lock, flags);

	mtk_iommu_tlb_flush_all(data);

	return IRQ_HANDLED;
}

static int mtk_iommu_get_domain_id(struct device *dev,
				   const struct mtk_iommu_plat_data *plat_data)
{
	const struct mtk_iommu_iova_region *rgn = plat_data->iova_region;
	const struct bus_dma_region *dma_rgn = dev->dma_range_map;
	struct iommu_fwspec *fwspec;
	const u32 *rgn_larb_msk;
	unsigned int larbid, portidmsk = 0;
	int i, candidate = -1;
	dma_addr_t dma_end;

	if (plat_data->iova_region_nr == 1)
		return 0;

	if (plat_data->iova_region_larb_msk) {
		fwspec = dev_iommu_fwspec_get(dev);
		if (!fwspec || !fwspec->num_ids)
			return -EINVAL;

		larbid = MTK_M4U_TO_LARB(fwspec->ids[0]);
		if (larbid >= MTK_LARB_NR_MAX)
			return -EINVAL;
		for (i = 0; i < fwspec->num_ids; i++) {
			if (MTK_M4U_TO_LARB(fwspec->ids[i]) != larbid)
				return -EINVAL;
			portidmsk |= BIT(MTK_M4U_TO_PORT(fwspec->ids[i]));
		}

		for (i = 0; i < plat_data->iova_region_nr; i++) {
			rgn_larb_msk = plat_data->iova_region_larb_msk[i];
			if ((rgn_larb_msk[larbid] & portidmsk) == portidmsk)
				return i;
		}

		dev_err(dev, "Can NOT find the IOVA region for larb(%u-%x).\n",
			larbid, portidmsk);
		return -EINVAL;
	}

	if (!dma_rgn)
		return 0;

	dma_end = dma_rgn->dma_start + dma_rgn->size - 1;
	for (i = 0; i < plat_data->iova_region_nr; i++, rgn++) {
		/* Best fit. */
		if (dma_rgn->dma_start == rgn->iova_base &&
		    dma_end == rgn->iova_base + rgn->size - 1)
			return i;
		/* ok if it is inside this region. */
		if (dma_rgn->dma_start >= rgn->iova_base &&
		    dma_end < rgn->iova_base + rgn->size)
			candidate = i;
	}

	if (candidate >= 0)
		return candidate;
	dev_err(dev, "Can NOT find the iommu domain id(%pad 0x%llx).\n",
		&dma_rgn->dma_start, dma_rgn->size);
	return -EINVAL;
}

static void mtk_iommu_config(struct mtk_iommu_data *data, struct device *dev,
			     bool enable, unsigned int domid)
{
	struct mtk_smi_larb_iommu    *larb_mmu;
	unsigned int                 larbid, portid;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	const struct mtk_iommu_iova_region *region;
	int i;

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		return;

	for (i = 0; i < fwspec->num_ids; ++i) {
		larbid = MTK_M4U_TO_LARB(fwspec->ids[i]);
		portid = MTK_M4U_TO_PORT(fwspec->ids[i]);

		larb_mmu = &data->larb_imu[larbid];

		region = data->plat_data->iova_region + domid;
		larb_mmu->bank[portid] = upper_32_bits(region->iova_base);

		dev_dbg(dev, "%s iommu for larb(%s) port %d dom %d bank %d.\n",
			enable ? "enable" : "disable", dev_name(larb_mmu->dev),
			portid, domid, larb_mmu->bank[portid]);

		if (enable)
			larb_mmu->mmu |= MTK_SMI_MMU_EN(portid);
		else
			larb_mmu->mmu &= ~MTK_SMI_MMU_EN(portid);
	}
}

static int mtk_iommu_map_boot_fb(struct mtk_iommu_domain *dom,
			       struct mtk_iommu_data *data)
{
	struct reserved_mem *fb = data->boot_fb;
	size_t mapped = 0;
	int ret;

	if (!fb)
		return 0;

	/*
	 * The bootloader can leave both OVL0 and OVL0_2L scanning physical
	 * framebuffer addresses. Populate the new, unpublished page table
	 * before switching TTBR. Keep inherited scanout running until DRM
	 * replaces it, without resetting only part of the active pipeline.
	 * No TLB invalidation is needed before this table is installed.
	 */
	ret = dom->iop->map_pages(dom->iop, fb->base, fb->base, SZ_4K,
				 fb->size / SZ_4K, IOMMU_READ, GFP_KERNEL, &mapped);
	if (ret || mapped != fb->size)
		return ret ?: -EIO;

	if (dom->iop->iova_to_phys(dom->iop, fb->base) != fb->base ||
	    dom->iop->iova_to_phys(dom->iop, fb->base + fb->size - 1) !=
				  fb->base + fb->size - 1)
		return -EIO;

	dev_info(data->dev, "LK framebuffer identity mapped: base=%pa size=%pa\n",
		 &fb->base, &fb->size);
	return 0;
}

static int mtk_iommu_domain_finalise(struct mtk_iommu_domain *dom,
				     struct mtk_iommu_data *data,
				     unsigned int domid)
{
	const struct mtk_iommu_iova_region *region;

	/* Use the exist domain as there is only one pgtable here. */
	if (data->m4u_dom) {
		dom->iop = data->m4u_dom->iop;
		dom->cfg = data->m4u_dom->cfg;
		dom->domain.pgsize_bitmap = data->m4u_dom->cfg.pgsize_bitmap;
		goto update_iova_region;
	}

	dom->cfg = (struct io_pgtable_cfg) {
		.quirks = IO_PGTABLE_QUIRK_ARM_NS |
			IO_PGTABLE_QUIRK_NO_PERMS |
			IO_PGTABLE_QUIRK_ARM_MTK_EXT,
		.pgsize_bitmap = mtk_iommu_ops.pgsize_bitmap,
		.ias = MTK_IOMMU_HAS_FLAG(data->plat_data, IOVA_34_EN) ? 34 : 32,
		.iommu_dev = data->dev,
	};

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, HAS_4GB_MODE))
		dom->cfg.oas = data->enable_4GB ? 33 : 32;
	else
		dom->cfg.oas = 35;

	/* MT6877 APU uses the v7s shareable bit as ACP, disabled in stock. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		dom->cfg.quirks |= IO_PGTABLE_QUIRK_ARM_MTK_NO_SHARE;

	dom->iop = alloc_io_pgtable_ops(ARM_V7S, &dom->cfg, data);
	if (!dom->iop) {
		dev_err(data->dev, "Failed to alloc io pgtable\n");
		return -EINVAL;
	}

	if (mtk_iommu_map_boot_fb(dom, data)) {
		dev_err(data->dev, "Failed to preserve LK framebuffer mapping\n");
		free_io_pgtable_ops(dom->iop);
		dom->iop = NULL;
		return -EIO;
	}

	/* Update our support page sizes bitmap */
	dom->domain.pgsize_bitmap = dom->cfg.pgsize_bitmap;

update_iova_region:
	/* Update the iova region for this domain */
	region = data->plat_data->iova_region + domid;
	dom->domain.geometry.aperture_start = region->iova_base;
	dom->domain.geometry.aperture_end = region->iova_base + region->size - 1;
	dom->domain.geometry.force_aperture = true;
	return 0;
}

static struct iommu_domain *mtk_iommu_domain_alloc(unsigned type)
{
	struct mtk_iommu_domain *dom;

	if (type != IOMMU_DOMAIN_DMA)
		return NULL;

	dom = kzalloc(sizeof(*dom), GFP_KERNEL);
	if (!dom)
		return NULL;

	return &dom->domain;
}

static void mtk_iommu_domain_free(struct iommu_domain *domain)
{
	kfree(to_mtk_domain(domain));
}

static int mtk_iommu_attach_device(struct iommu_domain *domain,
				   struct device *dev)
{
	struct mtk_iommu_data *data = dev_iommu_priv_get(dev), *frstdata;
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);
	struct device *m4udev = data->dev;
	int ret, domid;
	unsigned long flags;

	domid = mtk_iommu_get_domain_id(dev, data->plat_data);
	if (domid < 0)
		return domid;

	if (!dom->data) {
		/* Data is in the frstdata in sharing pgtable case. */
		frstdata = mtk_iommu_get_m4u_data(data);

		if (mtk_iommu_domain_finalise(dom, frstdata, domid))
			return -ENODEV;
		dom->data = data;
	}

	mutex_lock(&data->mutex);
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU)) {
		spin_lock_irqsave(&data->tlb_lock, flags);
		if (!data->m4u_dom) {
			data->m4u_dom = dom;
			/* Registration may find APU already powered by another user. */
			if (data->apu_powered) {
				writel(dom->cfg.arm_v7s_cfg.ttbr,
				       data->base + REG_MMU_PT_BASE_ADDR);
				writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0,
					       data->base + data->plat_data->inv_sel_reg);
				writel_relaxed(F_ALL_INVLD,
					       data->base + REG_MMU_INVALIDATE);
				wmb(); /* Order the new TTBR and invalidate before unlock. */
			}
		}
		spin_unlock_irqrestore(&data->tlb_lock, flags);
		mutex_unlock(&data->mutex);
		return dma_set_mask_and_coherent(dev, DMA_BIT_MASK(34));
	}
	if (!data->m4u_dom) { /* Initialize the M4U HW */
		ret = pm_runtime_resume_and_get(m4udev);
		if (ret < 0)
			goto err_unlock;

		ret = mtk_iommu_hw_init(data);
		if (ret) {
			pm_runtime_put(m4udev);
			goto err_unlock;
		}
		data->m4u_dom = dom;
		writel(dom->cfg.arm_v7s_cfg.ttbr & MMU_PT_ADDR_MASK,
		       data->base + REG_MMU_PT_BASE_ADDR);
		/* Drop any translations inherited from the bootloader's table. */
		mtk_iommu_tlb_flush_local(data);

		pm_runtime_put(m4udev);
	}
	mutex_unlock(&data->mutex);

	if (domid > 0) {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(34));
		if (ret)
			return dev_err_probe(dev, ret,
				"failed to set 34-bit DMA mask\n");
	}

	mtk_iommu_config(data, dev, true, domid);
	return 0;

err_unlock:
	mutex_unlock(&data->mutex);
	return ret;
}

static int mtk_iommu_map(struct iommu_domain *domain, unsigned long iova,
			 phys_addr_t paddr, size_t size, int prot, gfp_t gfp)
{
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);
	size_t mapped = 0;

	/* The "4GB mode" M4U physically can not use the lower remap of Dram. */
	if (dom->data->enable_4GB)
		paddr |= BIT_ULL(32);

	/* Synchronize with the tlb_lock */
	return dom->iop->map_pages(dom->iop, iova, paddr, size, 1,
				  prot, gfp, &mapped);
}

static size_t mtk_iommu_unmap(struct iommu_domain *domain,
			      unsigned long iova, size_t size,
			      struct iommu_iotlb_gather *gather)
{
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);

	iommu_iotlb_gather_add_range(gather, iova, size);
	return dom->iop->unmap_pages(dom->iop, iova, size, 1, gather);
}

static void mtk_iommu_flush_iotlb_all(struct iommu_domain *domain)
{
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);

	/* Reserved-region setup can flush before the first device attaches. */
	if (dom->data)
		mtk_iommu_tlb_flush_all(dom->data);
}

static void mtk_iommu_iotlb_sync(struct iommu_domain *domain,
				 struct iommu_iotlb_gather *gather)
{
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);
	size_t length = gather->end - gather->start + 1;

	mtk_iommu_tlb_flush_range_sync(gather->start, length, gather->pgsize,
				       dom->data);
}

static void mtk_iommu_sync_map(struct iommu_domain *domain, unsigned long iova,
			       size_t size)
{
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);

	mtk_iommu_tlb_flush_range_sync(iova, size, size, dom->data);
}

static phys_addr_t mtk_iommu_iova_to_phys(struct iommu_domain *domain,
					  dma_addr_t iova)
{
	struct mtk_iommu_domain *dom = to_mtk_domain(domain);
	phys_addr_t pa;

	pa = dom->iop->iova_to_phys(dom->iop, iova);
	if (IS_ENABLED(CONFIG_PHYS_ADDR_T_64BIT) &&
	    dom->data->enable_4GB &&
	    pa >= MTK_IOMMU_4GB_MODE_REMAP_BASE)
		pa &= ~BIT_ULL(32);

	return pa;
}

static struct iommu_device *mtk_iommu_probe_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct mtk_iommu_data *data;
	struct device_link *link;
	struct device *larbdev;
	unsigned int larbid, larbidx, i;

	if (!fwspec || fwspec->ops != &mtk_iommu_ops)
		return ERR_PTR(-ENODEV); /* Not a iommu client device */

	data = dev_iommu_priv_get(dev);
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU)) {
		int domid = mtk_iommu_get_domain_id(dev, data->plat_data);

		return domid < 0 ? ERR_PTR(domid) : &data->iommu;
	}

	/*
	 * Link the consumer device with the smi-larb device(supplier).
	 * The device that connects with each a larb is a independent HW.
	 * All the ports in each a device should be in the same larbs.
	 */
	larbid = MTK_M4U_TO_LARB(fwspec->ids[0]);
	if (larbid >= MTK_LARB_NR_MAX)
		return ERR_PTR(-EINVAL);

	for (i = 1; i < fwspec->num_ids; i++) {
		larbidx = MTK_M4U_TO_LARB(fwspec->ids[i]);
		if (larbid != larbidx) {
			dev_err(dev, "Can only use one larb. Fail@larb%d-%d.\n",
				larbid, larbidx);
			return ERR_PTR(-EINVAL);
		}
	}
	larbdev = data->larb_imu[larbid].dev;
	if (!larbdev)
		return ERR_PTR(-EINVAL);

	link = device_link_add(dev, larbdev,
			       DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS);
	if (!link)
		dev_err(dev, "Unable to link %s\n", dev_name(larbdev));
	return &data->iommu;
}

static void mtk_iommu_release_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct mtk_iommu_data *data;
	struct device *larbdev;
	unsigned int larbid;

	if (!fwspec || fwspec->ops != &mtk_iommu_ops)
		return;

	data = dev_iommu_priv_get(dev);
	mtk_iommu_config(data, dev, false, 0);
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU)) {
		iommu_fwspec_free(dev);
		return;
	}
	larbid = MTK_M4U_TO_LARB(fwspec->ids[0]);
	larbdev = data->larb_imu[larbid].dev;
	device_link_remove(dev, larbdev);

	iommu_fwspec_free(dev);
}

static struct iommu_group *mtk_iommu_device_group(struct device *dev)
{
	struct mtk_iommu_data *data = dev_iommu_priv_get(dev);
	struct iommu_group *group;
	int domid;

	if (!data)
		return ERR_PTR(-ENODEV);
	data = mtk_iommu_get_m4u_data(data);

	domid = mtk_iommu_get_domain_id(dev, data->plat_data);
	if (domid < 0)
		return ERR_PTR(domid);

	mutex_lock(&data->mutex);
	group = data->m4u_group[domid];
	if (!group) {
		group = iommu_group_alloc();
		if (!IS_ERR(group))
			data->m4u_group[domid] = group;
	} else {
		iommu_group_ref_get(group);
	}
	mutex_unlock(&data->mutex);
	return group;
}

static int mtk_iommu_of_xlate(struct device *dev, struct of_phandle_args *args)
{
	struct platform_device *m4updev;

	if (args->args_count != 1) {
		dev_err(dev, "invalid #iommu-cells(%d) property for IOMMU\n",
			args->args_count);
		return -EINVAL;
	}

	if (!dev_iommu_priv_get(dev)) {
		/* Get the m4u device */
		m4updev = of_find_device_by_node(args->np);
		if (WARN_ON(!m4updev))
			return -EINVAL;

		dev_iommu_priv_set(dev, platform_get_drvdata(m4updev));

		put_device(&m4updev->dev);
	}

	return iommu_fwspec_add_ids(dev, args->args, 1);
}

static void mtk_iommu_get_resv_regions(struct device *dev,
				       struct list_head *head)
{
	struct mtk_iommu_data *data = dev_iommu_priv_get(dev);
	unsigned int domid = mtk_iommu_get_domain_id(dev, data->plat_data), i;
	const struct mtk_iommu_iova_region *resv, *curdom;
	struct iommu_resv_region *region;
	int prot = IOMMU_WRITE | IOMMU_READ;

	if ((int)domid < 0)
		return;

	/*
	 * domain_finalise() installs this identity mapping before HW attach.
	 * Reserve its IOVA as well, so the DMA allocator cannot reuse it.
	 * The NO_LK display path keeps this non-reusable carveout reserved.
	 */
	if (!domid && data->boot_fb) {
		region = iommu_alloc_resv_region(data->boot_fb->base,
						data->boot_fb->size, IOMMU_READ,
						IOMMU_RESV_RESERVED, GFP_KERNEL);
		if (!region)
			return;
		list_add_tail(&region->list, head);
	}

	/* VPU/reviser use the explicit allocator, not the generic DMA allocator. */
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU) && domid) {
		curdom = data->plat_data->iova_region + domid;
		region = iommu_alloc_resv_region(curdom->iova_base, curdom->size,
						prot, IOMMU_RESV_RESERVED, GFP_KERNEL);
		if (region)
			list_add_tail(&region->list, head);
		return;
	}

	curdom = data->plat_data->iova_region + domid;
	for (i = 0; i < data->plat_data->iova_region_nr; i++) {
		resv = data->plat_data->iova_region + i;

		/* Only reserve when the region is inside the current domain */
		if (resv->iova_base < curdom->iova_base ||
		    resv->iova_base + resv->size >= curdom->iova_base + curdom->size)
			continue;

		region = iommu_alloc_resv_region(resv->iova_base, resv->size,
						 prot, IOMMU_RESV_RESERVED,
						 GFP_KERNEL);
		if (!region)
			return;

		list_add_tail(&region->list, head);
	}
}

static int mtk_iommu_def_domain_type(struct device *dev)
{
	(void)dev;
	return IOMMU_DOMAIN_DMA;
}

static const struct iommu_domain_ops mtk_iommu_domain_ops = {
	.attach_dev	= mtk_iommu_attach_device,
	.map		= mtk_iommu_map,
	.unmap		= mtk_iommu_unmap,
	.flush_iotlb_all = mtk_iommu_flush_iotlb_all,
	.iotlb_sync	= mtk_iommu_iotlb_sync,
	.iotlb_sync_map	= mtk_iommu_sync_map,
	.iova_to_phys	= mtk_iommu_iova_to_phys,
	.free		= mtk_iommu_domain_free,
};

static const struct iommu_ops mtk_iommu_ops = {
	.domain_alloc	= mtk_iommu_domain_alloc,
	.probe_device	= mtk_iommu_probe_device,
	.release_device	= mtk_iommu_release_device,
	.device_group	= mtk_iommu_device_group,
	.of_xlate	= mtk_iommu_of_xlate,
	.get_resv_regions = mtk_iommu_get_resv_regions,
	.def_domain_type = mtk_iommu_def_domain_type,
	.default_domain_ops = &mtk_iommu_domain_ops,
	.pgsize_bitmap	= SZ_4K | SZ_64K | SZ_1M | SZ_16M,
	.owner		= THIS_MODULE,
};

static void mtk_iommu_hw_program(const struct mtk_iommu_data *data)
{
	u32 regval;

	if (data->plat_data->m4u_plat == M4U_MT8173) {
		regval = F_MMU_PREFETCH_RT_REPLACE_MOD |
			 F_MMU_TF_PROT_TO_PROGRAM_ADDR_MT8173;
	} else {
		regval = readl_relaxed(data->base + REG_MMU_CTRL_REG);
		regval |= F_MMU_TF_PROT_TO_PROGRAM_ADDR;
	}
	writel_relaxed(regval, data->base + REG_MMU_CTRL_REG);

	regval = F_L2_MULIT_HIT_EN |
		F_TABLE_WALK_FAULT_INT_EN |
		F_PREETCH_FIFO_OVERFLOW_INT_EN |
		F_MISS_FIFO_OVERFLOW_INT_EN |
		F_PREFETCH_FIFO_ERR_INT_EN |
		F_MISS_FIFO_ERR_INT_EN;
	writel_relaxed(regval, data->base + REG_MMU_INT_CONTROL0);

	regval = F_INT_TRANSLATION_FAULT |
		F_INT_MAIN_MULTI_HIT_FAULT |
		F_INT_INVALID_PA_FAULT |
		F_INT_ENTRY_REPLACEMENT_FAULT |
		F_INT_TLB_MISS_FAULT |
		F_INT_MISS_TRANSACTION_FIFO_FAULT |
		F_INT_PRETETCH_TRANSATION_FIFO_FAULT;
	writel_relaxed(regval, data->base + REG_MMU_INT_MAIN_CONTROL);

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, HAS_LEGACY_IVRP_PADDR))
		regval = (data->protect_base >> 1) | (data->enable_4GB << 31);
	else
		regval = lower_32_bits(data->protect_base) |
			 upper_32_bits(data->protect_base);
	writel_relaxed(regval, data->base + REG_MMU_IVRP_PADDR);

	if (data->enable_4GB &&
	    MTK_IOMMU_HAS_FLAG(data->plat_data, HAS_VLD_PA_RNG)) {
		/*
		 * If 4GB mode is enabled, the validate PA range is from
		 * 0x1_0000_0000 to 0x1_ffff_ffff. here record bit[32:30].
		 */
		regval = F_MMU_VLD_PA_RNG(7, 4);
		writel_relaxed(regval, data->base + REG_MMU_VLD_PA_RNG);
	}
	/* Ruby's APU IOMMU uses the 4.19 bus settings after every power loss. */
	writel_relaxed(MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU) ?
		       F_MMU_DCM : 0, data->base + REG_MMU_DCM_DIS);
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, WR_THROT_EN)) {
		/* write command throttling mode */
		regval = readl_relaxed(data->base + REG_MMU_WR_LEN_CTRL);
		regval &= ~F_MMU_WR_THROT_DIS_MASK;
		writel_relaxed(regval, data->base + REG_MMU_WR_LEN_CTRL);
	}

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, RESET_AXI)) {
		/* The register is called STANDARD_AXI_MODE in this case */
		regval = 0;
	} else {
		regval = readl_relaxed(data->base + REG_MMU_MISC_CTRL);
		if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU)) {
			regval |= F_MMU_COHERENCE_EN_MASK |
				  F_MMU_IN_ORDER_WR_EN_MASK;
		} else {
			regval &= ~F_MMU_STANDARD_AXI_MODE_MASK;
			if (MTK_IOMMU_HAS_FLAG(data->plat_data, OUT_ORDER_WR_EN))
				regval &= ~F_MMU_IN_ORDER_WR_EN_MASK;
		}
	}
	writel_relaxed(regval, data->base + REG_MMU_MISC_CTRL);
}

static int mtk_iommu_hw_init(const struct mtk_iommu_data *data)
{
	mtk_iommu_hw_program(data);
	if (devm_request_irq(data->dev, data->irq, mtk_iommu_isr, 0,
			     dev_name(data->dev), (void *)data)) {
		writel_relaxed(0, data->base + REG_MMU_PT_BASE_ADDR);
		dev_err(data->dev, "Failed @ IRQ-%d Request\n", data->irq);
		return -ENODEV;
	}

	return 0;
}

static const struct component_master_ops mtk_iommu_com_ops = {
	.bind		= mtk_iommu_bind,
	.unbind		= mtk_iommu_unbind,
};

static int mtk_iommu_find_boot_fb(struct mtk_iommu_data *data)
{
	struct device_node *parent, *node;
	struct reserved_mem *fb;
	const char *suffix;
	int ret = 0;

	if (data->plat_data->m4u_plat != M4U_MT6877 ||
	    MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		return 0;

	/*
	 * IOMMU attachment precedes DRM's LK framebuffer adoption as well as
	 * cold modesetting. Protect inherited physical scanout in both paths.
	 *
	 * Ruby LK adds mblock-<index>-framebuffer to /reserved-memory.
	 * Its index and physical address are bootloader-selected, not fixed
	 * DT addresses. Accept only the actual, non-reusable reservation;
	 * never turn an unchecked videolfb tag into a DMA mapping.
	 */
	parent = of_find_node_by_path("/reserved-memory");
	if (!parent)
		return 0;

	for_each_available_child_of_node(parent, node) {
		if (!str_has_prefix(node->name, "mblock-"))
			continue;
		suffix = strstr(node->name, "-framebuffer");
		if (!suffix || (suffix[12] && suffix[12] != '@'))
			continue;

		fb = of_reserved_mem_lookup(node);
		if (data->boot_fb || !fb || !fb->base || !fb->size ||
		    of_property_read_bool(node, "reusable") ||
		    !IS_ALIGNED(fb->base, SZ_4K) ||
		    !IS_ALIGNED(fb->size, SZ_4K) ||
		    fb->base >= SZ_4G || fb->size > SZ_4G - fb->base) {
			dev_err(data->dev, "Invalid or ambiguous LK framebuffer: %pOF\n",
				node);
			ret = -EINVAL;
			of_node_put(node);
			break;
		}
		data->boot_fb = fb;
	}
	of_node_put(parent);
	return ret;
}

/*
 * Explicit mappings follow A34x vpu/4.5's allocator + iommu_map_sg path.
 * Each firmware domain is reserved from generic DMA allocation, so static
 * and dynamic firmware mappings share one allocator and cannot overlap.
 */
static struct mtk_iommu_data *mtk_apu_data(struct device *dev, int *domid)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct mtk_iommu_data *data;

	if (!fwspec || fwspec->ops != &mtk_iommu_ops)
		return ERR_PTR(-ENODEV);
	data = dev_iommu_priv_get(dev);
	if (!data || !MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		return ERR_PTR(-ENODEV);
	*domid = mtk_iommu_get_domain_id(dev, data->plat_data);
	if (*domid < 1 || *domid > 2)
		return ERR_PTR(-EINVAL);
	return data;
}

/* Prefer the firmware heap at 0x380000000 for dynamic VPU allocations. */
static unsigned long mtk_apu_heap_fit(unsigned long *map, unsigned long size,
		unsigned long start, unsigned int nr, void *unused,
		struct gen_pool *pool, unsigned long base)
{
	unsigned long heap = (0x380000000ULL - base) >> PAGE_SHIFT;

	return bitmap_find_next_zero_area(map, size, max(start, heap), nr, 0);
}

int mtk_apu_iommu_map(struct device *dev, struct scatterlist *sg,
		     unsigned int nents, size_t size, dma_addr_t requested,
		     dma_addr_t *iova)
{
	const struct mtk_iommu_iova_region *region;
	struct mtk_iommu_data *data;
	struct iommu_domain *domain;
	struct genpool_data_fixed fixed;
	struct gen_pool *pool;
	struct scatterlist *entry;
	unsigned long address;
	size_t length = 0;
	ssize_t mapped;
	unsigned int j;
	int domid;

	if (!sg || !nents || !size || !IS_ALIGNED(size, PAGE_SIZE) || !iova)
		return -EINVAL;
	/* Do not map beyond the address range reserved in the allocator. */
	for_each_sg(sg, entry, nents, j) {
		if (!IS_ALIGNED(sg_phys(entry), PAGE_SIZE) ||
		    !IS_ALIGNED(entry->length, PAGE_SIZE) ||
		    entry->length > size - length)
			return -EINVAL;
		length += entry->length;
	}
	if (length != size)
		return -EINVAL;
	data = mtk_apu_data(dev, &domid);
	if (IS_ERR(data))
		return PTR_ERR(data);
	domain = iommu_get_domain_for_dev(dev);
	if (!domain)
		return -ENODEV;
	region = data->plat_data->iova_region + domid;
	pool = data->apu_pools[domid];
	if (requested) {
		if (!IS_ALIGNED(requested, PAGE_SIZE) ||
		    requested < region->iova_base ||
		    requested - region->iova_base >= region->size ||
		    size > region->size - (requested - region->iova_base))
			return -ERANGE;
		fixed.offset = requested - region->iova_base;
		address = gen_pool_alloc_algo(pool, size, gen_pool_fixed_alloc, &fixed);
		if (address && address != requested) {
			gen_pool_free(pool, address, size);
			return -EIO;
		}
	} else {
		address = domid == 1 ? gen_pool_alloc_algo(pool, size,
							mtk_apu_heap_fit, NULL) : 0;
		if (!address)
			address = gen_pool_alloc(pool, size);
	}
	if (!address)
		return -ENOSPC;

	mapped = iommu_map_sg(domain, address, sg, nents,
			     IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (mapped < 0 || mapped != size) {
		/* iommu_map_sg already unwinds mappings when it returns an errno. */
		if (mapped > 0)
			iommu_unmap(domain, address, mapped);
		gen_pool_free(pool, address, size);
		return mapped < 0 ? mapped : -EIO;
	}
	*iova = address;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_apu_iommu_map);

void mtk_apu_iommu_unmap(struct device *dev, dma_addr_t iova, size_t size)
{
	struct mtk_iommu_data *data;
	struct iommu_domain *domain;
	int domid;

	data = mtk_apu_data(dev, &domid);
	if (WARN_ON(IS_ERR(data)))
		return;
	domain = iommu_get_domain_for_dev(dev);
	if (WARN_ON(!domain))
		return;
	if (WARN_ON(iommu_unmap(domain, iova, size) != size))
		return; /* Never reallocate an address whose unmap failed. */
	gen_pool_free(data->apu_pools[domid], iova, size);
}
EXPORT_SYMBOL_GPL(mtk_apu_iommu_unmap);

void mtk_apu_iommu_sync(struct device *dev, struct scatterlist *sg,
		      unsigned int nents, enum dma_data_direction direction,
		      bool for_cpu)
{
	struct scatterlist *entry;
	unsigned int i;

	if (dev_is_dma_coherent(dev))
		return;
	/* Explicit iommu_map_sg mappings do not belong to dma_map_sg's allocator. */
	for_each_sg(sg, entry, nents, i) {
		if (for_cpu)
			arch_sync_dma_for_cpu(sg_phys(entry), entry->length, direction);
		else
			arch_sync_dma_for_device(sg_phys(entry), entry->length, direction);
	}
}
EXPORT_SYMBOL_GPL(mtk_apu_iommu_sync);

#if IS_REACHABLE(CONFIG_MTK_APUSYS_SUPPORT) && IS_ENABLED(CONFIG_MACH_MT6877)
static struct mtk_iommu_data *mt6877_apu;

static void mtk_iommu_apu_restore_locked(struct mtk_iommu_data *data)
{
	struct mtk_iommu_domain *dom = data->m4u_dom;

	mtk_iommu_hw_program(data);
	/* io-pgtable encodes PA[34:32] in TTBR[2:0], as in A34x. */
	if (dom)
		writel(dom->cfg.arm_v7s_cfg.ttbr, data->base + REG_MMU_PT_BASE_ADDR);
	writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0,
		       data->base + data->plat_data->inv_sel_reg);
	writel_relaxed(F_ALL_INVLD, data->base + REG_MMU_INVALIDATE);
	wmb(); /* Install the restored table before clients resume APU work. */
}

static bool mtk_iommu_apu_hw_ready(struct mtk_iommu_data *data, u32 ttbr)
{
	u32 misc_mask = F_MMU_COHERENCE_EN_MASK | F_MMU_IN_ORDER_WR_EN_MASK;
	u32 int_mask = F_L2_MULIT_HIT_EN | F_TABLE_WALK_FAULT_INT_EN |
		F_PREETCH_FIFO_OVERFLOW_INT_EN | F_MISS_FIFO_OVERFLOW_INT_EN |
		F_PREFETCH_FIFO_ERR_INT_EN | F_MISS_FIFO_ERR_INT_EN;
	u32 main_mask = F_INT_TRANSLATION_FAULT | F_INT_MAIN_MULTI_HIT_FAULT |
		F_INT_INVALID_PA_FAULT | F_INT_ENTRY_REPLACEMENT_FAULT |
		F_INT_TLB_MISS_FAULT | F_INT_MISS_TRANSACTION_FIFO_FAULT |
		F_INT_PRETETCH_TRANSATION_FIFO_FAULT;
	u32 protect = lower_32_bits(data->protect_base) |
		      upper_32_bits(data->protect_base);

	return readl(data->base + REG_MMU_PT_BASE_ADDR) == ttbr &&
		readl(data->base + REG_MMU_IVRP_PADDR) == protect &&
		readl(data->base + REG_MMU_DCM_DIS) == F_MMU_DCM &&
		(readl(data->base + REG_MMU_MISC_CTRL) & misc_mask) == misc_mask &&
		(readl(data->base + REG_MMU_CTRL_REG) & GENMASK(5, 4)) ==
		 F_MMU_TF_PROT_TO_PROGRAM_ADDR &&
		!(readl(data->base + REG_MMU_WR_LEN_CTRL) & F_MMU_WR_THROT_DIS_MASK) &&
		(readl(data->base + REG_MMU_INT_CONTROL0) & int_mask) == int_mask &&
		(readl(data->base + REG_MMU_INT_MAIN_CONTROL) & main_mask) == main_mask;
}

int mtk_apu_iommu_prepare(struct device *dev)
{
	struct mtk_iommu_data *data;
	struct mtk_iommu_domain *dom;
	unsigned long flags;
	u32 observed_ttbr, expected;
	int domid, ret = 0;

	data = mtk_apu_data(dev, &domid);
	if (IS_ERR(data))
		return PTR_ERR(data);

	spin_lock_irqsave(&data->tlb_lock, flags);
	dom = data->m4u_dom;
	if (!data->apu_powered || !dom) {
		ret = -EHOSTDOWN;
		goto unlock;
	}

	expected = dom->cfg.arm_v7s_cfg.ttbr;
	observed_ttbr = readl(data->base + REG_MMU_PT_BASE_ADDR);
	if (!observed_ttbr) {
		/* Recover the existing table after hardware lost its register state. */
		dev_warn_ratelimited(dev, "APU IOMMU lost its table; restoring 0x%x\n",
				     expected);
		mtk_iommu_apu_restore_locked(data);
	} else if (observed_ttbr != expected) {
		/* Never replace an unexpected live table used by another engine. */
		dev_err(dev, "APU IOMMU table mismatch: 0x%x, expected 0x%x\n",
			observed_ttbr, expected);
		ret = -EIO;
		goto unlock;
	}

	if (!mtk_iommu_apu_hw_ready(data, expected)) {
		dev_err(dev, "APU IOMMU restore verification failed\n");
		ret = -EIO;
	}
unlock:
	spin_unlock_irqrestore(&data->tlb_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_apu_iommu_prepare);

static void mtk_iommu_apu_power_on(void *unused)
{
	struct mtk_iommu_data *data = mt6877_apu;
	unsigned long flags;

	spin_lock_irqsave(&data->tlb_lock, flags);
	mtk_iommu_apu_restore_locked(data);
	data->apu_powered = true;
	spin_unlock_irqrestore(&data->tlb_lock, flags);
	enable_irq(data->irq);
}

static void mtk_iommu_apu_power_off(void *unused)
{
	struct mtk_iommu_data *data = mt6877_apu;
	unsigned long flags;

	disable_irq(data->irq);
	spin_lock_irqsave(&data->tlb_lock, flags);
	data->apu_powered = false;
	spin_unlock_irqrestore(&data->tlb_lock, flags);
}
#endif

static int mtk_iommu_apu_register(struct mtk_iommu_data *data)
{
#if IS_REACHABLE(CONFIG_MTK_APUSYS_SUPPORT) && IS_ENABLED(CONFIG_MACH_MT6877)
	const struct mtk_iommu_iova_region *region;
	const char *pool_name;
	int i, ret;

	if (mt6877_apu)
		return -EBUSY;
	for (i = 1; i <= 2; i++) {
		region = data->plat_data->iova_region + i;
		/* Managed pool names must be unique within this IOMMU device. */
		pool_name = i == 1 ? "vpu-code" : "reviser-vlm";
		data->apu_pools[i] = devm_gen_pool_create(data->dev, PAGE_SHIFT,
							NUMA_NO_NODE, pool_name);
		if (IS_ERR(data->apu_pools[i]))
			return dev_err_probe(data->dev, PTR_ERR(data->apu_pools[i]),
					     "failed to create %s IOVA pool\n", pool_name);
		ret = gen_pool_add(data->apu_pools[i], region->iova_base,
				   region->size, NUMA_NO_NODE);
		if (ret)
			return dev_err_probe(data->dev, ret,
					     "failed to add %s IOVA range\n", pool_name);
	}
	ret = devm_request_irq(data->dev, data->irq, mtk_iommu_isr, 0,
			       dev_name(data->dev), data);
	if (ret)
		return ret;
	disable_irq(data->irq);
	mt6877_apu = data;
	ret = apu_power_callback_device_register(IOMMU,
		mtk_iommu_apu_power_on, mtk_iommu_apu_power_off);
	if (ret) {
		mt6877_apu = NULL;
		return -ENOMEM;
	}
	return 0;
#else
	return -ENODEV;
#endif
}

static void mtk_iommu_apu_unregister(struct mtk_iommu_data *data)
{
#if IS_REACHABLE(CONFIG_MTK_APUSYS_SUPPORT) && IS_ENABLED(CONFIG_MACH_MT6877)
	apu_power_callback_device_unregister(IOMMU);
	if (data->apu_powered)
		mtk_iommu_apu_power_off(NULL);
	mt6877_apu = NULL;
#endif
}

static int mtk_iommu_probe(struct platform_device *pdev)
{
	struct mtk_iommu_data   *data;
	struct device           *dev = &pdev->dev;
	struct device_node	*larbnode, *smicomm_node;
	struct platform_device	*plarbdev;
	struct device_link	*link;
	struct resource         *res;
	resource_size_t		ioaddr;
	struct component_match  *match = NULL;
	struct regmap		*infracfg;
	void                    *protect;
	int                     i, larb_nr, ret;
	u32			val __maybe_unused;
	char                    *p;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->dev = dev;
	data->plat_data = of_device_get_match_data(dev);
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU)) {
		struct device_node *node;
		struct platform_device *power;
		bool ready;

		node = of_parse_phandle(dev->of_node, "mediatek,apu-power", 0);
		if (!node)
			return -EINVAL;
		power = of_find_device_by_node(node);
		of_node_put(node);
		if (!power)
			return -EPROBE_DEFER;
		ready = device_is_bound(&power->dev);
		put_device(&power->dev);
		if (!ready)
			return -EPROBE_DEFER;
	}
	spin_lock_init(&data->tlb_lock);

	ret = mtk_iommu_find_boot_fb(data);
	if (ret)
		return ret;

	/* Protect memory. HW will access here while translation fault.*/
	protect = devm_kzalloc(dev, MTK_PROTECT_PA_ALIGN * 2, GFP_KERNEL);
	if (!protect)
		return -ENOMEM;
	data->protect_base = ALIGN(virt_to_phys(protect), MTK_PROTECT_PA_ALIGN);

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, HAS_4GB_MODE)) {
		switch (data->plat_data->m4u_plat) {
		case M4U_MT2712:
			p = "mediatek,mt2712-infracfg";
			break;
		case M4U_MT8173:
			p = "mediatek,mt8173-infracfg";
			break;
		default:
			p = NULL;
		}

		if (!p)
			return -EINVAL;

		infracfg = syscon_regmap_lookup_by_compatible(p);

		if (IS_ERR(infracfg))
			return PTR_ERR(infracfg);

#if defined(REG_INFRA_MISC) && defined(F_DDR_4GB_SUPPORT_EN)
		ret = regmap_read(infracfg, REG_INFRA_MISC, &val);
		if (ret)
			return ret;
		data->enable_4GB = !!(val & F_DDR_4GB_SUPPORT_EN);
#else
		data->enable_4GB = false;
#endif
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	data->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(data->base))
		return PTR_ERR(data->base);
	ioaddr = res->start;

	data->irq = platform_get_irq(pdev, 0);
	if (data->irq < 0)
		return data->irq;

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, HAS_BCLK)) {
		data->bclk = devm_clk_get(dev, "bclk");
		if (IS_ERR(data->bclk))
			return PTR_ERR(data->bclk);
	}

	if (!MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU)) {
		data->power_clk = devm_clk_get_optional(dev, "power");
		if (IS_ERR(data->power_clk))
			return PTR_ERR(data->power_clk);
	} else {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(35));
		if (ret)
			return ret;
	}

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		goto skip_smi;

	larb_nr = of_count_phandle_with_args(dev->of_node,
					     "mediatek,larbs", NULL);
	if (larb_nr < 0)
		return larb_nr;

	for (i = 0; i < larb_nr; i++) {
		u32 id;

		larbnode = of_parse_phandle(dev->of_node, "mediatek,larbs", i);
		if (!larbnode)
			return -EINVAL;

		if (!of_device_is_available(larbnode)) {
			of_node_put(larbnode);
			continue;
		}

		ret = of_property_read_u32(larbnode, "mediatek,larb-id", &id);
		if (ret)/* The id is consecutive if there is no this property */
			id = i;

		plarbdev = of_find_device_by_node(larbnode);
		if (!plarbdev) {
			of_node_put(larbnode);
			return -ENODEV;
		}
		data->larb_imu[id].dev = &plarbdev->dev;

		component_match_add_release(dev, &match, release_of,
					    compare_of, larbnode);
	}

	/* Get smi-common dev from the last larb. */
	smicomm_node = of_parse_phandle(larbnode, "mediatek,smi", 0);
	if (!smicomm_node)
		return -EINVAL;

	plarbdev = of_find_device_by_node(smicomm_node);
	of_node_put(smicomm_node);
	data->smicomm_dev = &plarbdev->dev;

	pm_runtime_enable(dev);

	link = device_link_add(data->smicomm_dev, dev,
			DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME);
	if (!link) {
		dev_err(dev, "Unable to link %s.\n", dev_name(data->smicomm_dev));
		ret = -EINVAL;
		goto out_runtime_disable;
	}

skip_smi:
	platform_set_drvdata(pdev, data);
	mutex_init(&data->mutex);

	ret = iommu_device_sysfs_add(&data->iommu, dev, NULL,
				     "mtk-iommu.%pa", &ioaddr);
	if (ret)
		goto out_link_remove;

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU)) {
		ret = mtk_iommu_apu_register(data);
		if (ret)
			goto out_sysfs_remove;
	}
	/* Registration can immediately probe existing clients and their groups. */
	list_add_tail(&data->list, &m4ulist);
	ret = iommu_device_register(&data->iommu, &mtk_iommu_ops, dev);
	if (ret)
		goto out_list_del;

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		return 0;

	ret = component_master_add_with_match(dev, &mtk_iommu_com_ops, match);
	if (ret)
		goto out_iommu_unregister;
	return ret;

out_iommu_unregister:
	iommu_device_unregister(&data->iommu);
out_list_del:
	list_del(&data->list);
	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		mtk_iommu_apu_unregister(data);
out_sysfs_remove:
	iommu_device_sysfs_remove(&data->iommu);
out_link_remove:
	if (data->smicomm_dev)
		device_link_remove(data->smicomm_dev, dev);
out_runtime_disable:
	pm_runtime_disable(dev);
	return ret;
}

static int mtk_iommu_remove(struct platform_device *pdev)
{
	struct mtk_iommu_data *data = platform_get_drvdata(pdev);

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		mtk_iommu_apu_unregister(data);

	iommu_device_sysfs_remove(&data->iommu);
	iommu_device_unregister(&data->iommu);

	list_del(&data->list);

	if (data->smicomm_dev)
		device_link_remove(data->smicomm_dev, &pdev->dev);
	if (!MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		pm_runtime_disable(&pdev->dev);
	devm_free_irq(&pdev->dev, data->irq, data);
	if (!MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		component_master_del(&pdev->dev, &mtk_iommu_com_ops);
	return 0;
}

static int __maybe_unused mtk_iommu_runtime_suspend(struct device *dev)
{
	struct mtk_iommu_data *data = dev_get_drvdata(dev);
	struct mtk_iommu_suspend_reg *reg = &data->reg;
	void __iomem *base = data->base;

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		return 0; /* APUSYS power callbacks own save/restore. */

	reg->wr_len_ctrl = readl_relaxed(base + REG_MMU_WR_LEN_CTRL);
	reg->misc_ctrl = readl_relaxed(base + REG_MMU_MISC_CTRL);
	reg->dcm_dis = readl_relaxed(base + REG_MMU_DCM_DIS);
	reg->ctrl_reg = readl_relaxed(base + REG_MMU_CTRL_REG);
	reg->int_control0 = readl_relaxed(base + REG_MMU_INT_CONTROL0);
	reg->int_main_control = readl_relaxed(base + REG_MMU_INT_MAIN_CONTROL);
	reg->ivrp_paddr = readl_relaxed(base + REG_MMU_IVRP_PADDR);
	reg->vld_pa_rng = readl_relaxed(base + REG_MMU_VLD_PA_RNG);
	clk_disable_unprepare(data->bclk);
	clk_disable_unprepare(data->power_clk);
	return 0;
}

static int __maybe_unused mtk_iommu_runtime_resume(struct device *dev)
{
	struct mtk_iommu_data *data = dev_get_drvdata(dev);
	struct mtk_iommu_suspend_reg *reg = &data->reg;
	struct mtk_iommu_domain *m4u_dom = data->m4u_dom;
	void __iomem *base = data->base;
	int ret;

	if (MTK_IOMMU_HAS_FLAG(data->plat_data, APU_IOMMU))
		return 0; /* APUSYS power callbacks own save/restore. */

	ret = clk_prepare_enable(data->power_clk);
	if (ret) {
		dev_err(data->dev, "Failed to enable power clock(%d) in resume\n",
			ret);
		return ret;
	}

	ret = clk_prepare_enable(data->bclk);
	if (ret) {
		dev_err(data->dev, "Failed to enable clk(%d) in resume\n", ret);
		clk_disable_unprepare(data->power_clk);
		return ret;
	}

	/*
	 * Uppon first resume, only enable the clk and return, since the values of the
	 * registers are not yet set.
	 */
	if (!m4u_dom)
		return 0;

	writel_relaxed(reg->wr_len_ctrl, base + REG_MMU_WR_LEN_CTRL);
	writel_relaxed(reg->misc_ctrl, base + REG_MMU_MISC_CTRL);
	writel_relaxed(reg->dcm_dis, base + REG_MMU_DCM_DIS);
	writel_relaxed(reg->ctrl_reg, base + REG_MMU_CTRL_REG);
	writel_relaxed(reg->int_control0, base + REG_MMU_INT_CONTROL0);
	writel_relaxed(reg->int_main_control, base + REG_MMU_INT_MAIN_CONTROL);
	writel_relaxed(reg->ivrp_paddr, base + REG_MMU_IVRP_PADDR);
	writel_relaxed(reg->vld_pa_rng, base + REG_MMU_VLD_PA_RNG);
	writel(m4u_dom->cfg.arm_v7s_cfg.ttbr & MMU_PT_ADDR_MASK, base + REG_MMU_PT_BASE_ADDR);

	/*
	 * Map/unmap may skip invalidation while runtime suspended. The SMI
	 * power domain can remain on for other consumers, retaining stale
	 * translations. Flush this powered instance before resuming its users;
	 * a PM-status-gated helper would skip us while still RPM_RESUMING.
	 */
	mtk_iommu_tlb_flush_local(data);
	return 0;
}

static const struct dev_pm_ops mtk_iommu_pm_ops = {
	SET_RUNTIME_PM_OPS(mtk_iommu_runtime_suspend, mtk_iommu_runtime_resume, NULL)
	SET_LATE_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				     pm_runtime_force_resume)
};

static const struct mtk_iommu_plat_data mt2712_data = {
	.m4u_plat     = M4U_MT2712,
	.flags        = HAS_4GB_MODE | HAS_BCLK | HAS_VLD_PA_RNG,
	.inv_sel_reg  = REG_MMU_INV_SEL_GEN1,
	.iova_region  = single_domain,
	.iova_region_nr = ARRAY_SIZE(single_domain),
	.larbid_remap = {{0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}},
};

static const struct mtk_iommu_plat_data mt6779_data = {
	.m4u_plat      = M4U_MT6779,
	.flags         = HAS_SUB_COMM | OUT_ORDER_WR_EN | WR_THROT_EN,
	.inv_sel_reg   = REG_MMU_INV_SEL_GEN2,
	.iova_region   = single_domain,
	.iova_region_nr = ARRAY_SIZE(single_domain),
	.larbid_remap  = {{0}, {1}, {2}, {3}, {5}, {7, 8}, {10}, {9}},
};

static const struct mtk_iommu_plat_data mt6877_iommu0_data = {
	.m4u_plat       = M4U_MT6877,
	.flags          = HAS_BCLK | HAS_SUB_COMM | OUT_ORDER_WR_EN |
			  WR_THROT_EN | IOVA_34_EN,
	.inv_sel_reg    = REG_MMU_INV_SEL_GEN2,
	.iova_region    = mt8192_multi_dom,
	.iova_region_nr = ARRAY_SIZE(mt8192_multi_dom),
	.iova_region_larb_msk = mt6877_larb_region_msk,
	.larbid_remap   = {{0}, {1}, {4, 5}, {7}, {2}, {9, 11, 19, 20},
			   {0, 14, 16}, {0, 13, 18, 17}},
};

#if IS_REACHABLE(CONFIG_MTK_APUSYS_SUPPORT) && IS_ENABLED(CONFIG_MACH_MT6877)
static const struct mtk_iommu_plat_data mt6877_iommu1_data = {
	.m4u_plat = M4U_MT6877,
	.flags = IOVA_34_EN | APU_IOMMU | WR_THROT_EN,
	.inv_sel_reg = REG_MMU_INV_SEL_GEN2,
	.iova_region = mt6877_apu_domains,
	.iova_region_nr = ARRAY_SIZE(mt6877_apu_domains),
	.iova_region_larb_msk = mt6877_apu_ports,
};
#endif

static const struct mtk_iommu_plat_data mt8167_data = {
	.m4u_plat     = M4U_MT8167,
	.flags        = RESET_AXI | HAS_LEGACY_IVRP_PADDR,
	.inv_sel_reg  = REG_MMU_INV_SEL_GEN1,
	.iova_region  = single_domain,
	.iova_region_nr = ARRAY_SIZE(single_domain),
	.larbid_remap = {{0}, {1}, {2}}, /* Linear mapping. */
};

static const struct mtk_iommu_plat_data mt8173_data = {
	.m4u_plat     = M4U_MT8173,
	.flags	      = HAS_4GB_MODE | HAS_BCLK | RESET_AXI |
			HAS_LEGACY_IVRP_PADDR,
	.inv_sel_reg  = REG_MMU_INV_SEL_GEN1,
	.iova_region  = single_domain,
	.iova_region_nr = ARRAY_SIZE(single_domain),
	.larbid_remap = {{0}, {1}, {2}, {3}, {4}, {5}}, /* Linear mapping. */
};

static const struct mtk_iommu_plat_data mt8183_data = {
	.m4u_plat     = M4U_MT8183,
	.flags        = RESET_AXI,
	.inv_sel_reg  = REG_MMU_INV_SEL_GEN1,
	.iova_region  = single_domain,
	.iova_region_nr = ARRAY_SIZE(single_domain),
	.larbid_remap = {{0}, {4}, {5}, {6}, {7}, {2}, {3}, {1}},
};

static const struct mtk_iommu_plat_data mt8192_data = {
	.m4u_plat       = M4U_MT8192,
	.flags          = HAS_BCLK | HAS_SUB_COMM | OUT_ORDER_WR_EN |
			  WR_THROT_EN | IOVA_34_EN,
	.inv_sel_reg    = REG_MMU_INV_SEL_GEN2,
	.iova_region    = mt8192_multi_dom,
	.iova_region_nr = ARRAY_SIZE(mt8192_multi_dom),
	.larbid_remap   = {{0}, {1}, {4, 5}, {7}, {2}, {9, 11, 19, 20},
			   {0, 14, 16}, {0, 13, 18, 17}},
};

static const struct of_device_id mtk_iommu_of_ids[] = {
	{ .compatible = "mediatek,mt2712-m4u", .data = &mt2712_data},
	{ .compatible = "mediatek,mt6779-m4u", .data = &mt6779_data},
	{ .compatible = "mediatek,mt6877-iommu0", .data = &mt6877_iommu0_data},
#if IS_REACHABLE(CONFIG_MTK_APUSYS_SUPPORT) && IS_ENABLED(CONFIG_MACH_MT6877)
	{ .compatible = "mediatek,mt6877-iommu1", .data = &mt6877_iommu1_data},
#endif
	{ .compatible = "mediatek,mt8167-m4u", .data = &mt8167_data},
	{ .compatible = "mediatek,mt8173-m4u", .data = &mt8173_data},
	{ .compatible = "mediatek,mt8183-m4u", .data = &mt8183_data},
	{ .compatible = "mediatek,mt8192-m4u", .data = &mt8192_data},
	{}
};
MODULE_DEVICE_TABLE(of, mtk_iommu_of_ids);

static struct platform_driver mtk_iommu_driver = {
	.probe	= mtk_iommu_probe,
	.remove	= mtk_iommu_remove,
	.driver	= {
		.name = "mtk-iommu",
		.of_match_table = mtk_iommu_of_ids,
		.pm = &mtk_iommu_pm_ops,
	}
};
module_platform_driver(mtk_iommu_driver);

MODULE_DESCRIPTION("IOMMU API for MediaTek M4U implementations");
MODULE_LICENSE("GPL v2");
