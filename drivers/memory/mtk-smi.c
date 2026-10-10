// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2016 MediaTek Inc.
 * Author: Yong Wu <yong.wu@mediatek.com>
 */
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/rwsem.h>
#include <soc/mediatek/smi.h>
#include <dt-bindings/memory/mt2701-larb-port.h>
#include <dt-bindings/memory/mtk-memory-port.h>

/* mt8173 */
#define SMI_LARB_MMU_EN		0xf00

/* mt8167 */
#define MT8167_SMI_LARB_MMU_EN	0xfc0

/* mt2701 */
#define REG_SMI_SECUR_CON_BASE		0x5c0

/* every register control 8 port, register offset 0x4 */
#define REG_SMI_SECUR_CON_OFFSET(id)	(((id) >> 3) << 2)
#define REG_SMI_SECUR_CON_ADDR(id)	\
	(REG_SMI_SECUR_CON_BASE + REG_SMI_SECUR_CON_OFFSET(id))

/*
 * every port have 4 bit to control, bit[port + 3] control virtual or physical,
 * bit[port + 2 : port + 1] control the domain, bit[port] control the security
 * or non-security.
 */
#define SMI_SECUR_CON_VAL_MSK(id)	(~(0xf << (((id) & 0x7) << 2)))
#define SMI_SECUR_CON_VAL_VIRT(id)	BIT((((id) & 0x7) << 2) + 3)
/* mt2701 domain should be set to 3 */
#define SMI_SECUR_CON_VAL_DOMAIN(id)	(0x3 << ((((id) & 0x7) << 2) + 1))

/* mt2712 */
#define SMI_LARB_NONSEC_CON(id)	(0x380 + ((id) * 4))
#define F_MMU_EN		BIT(0)
#define BANK_SEL(id)		({			\
	u32 _id = (id) & 0x3;				\
	(_id << 8 | _id << 10 | _id << 12 | _id << 14);	\
})

#define SMI_LARB_CMD_THRT_CON		0x24
#define SMI_LARB_SW_FLAG		0x40
#define SMI_LARB_FORCE_ULTRA		0x78
#define SMI_LARB_OSTDL_PORT(id)		(0x200 + ((id) * 4))

/* SMI COMMON */
#define SMI_L1LEN			0x100
#define SMI_L1ARB(id)			(0x104 + ((id) * 4))
#define SMI_BUS_SEL			0x220
#define SMI_BUS_LARB_SHIFT(larbid)	((larbid) << 1)
/* All are MMU0 defaultly. Only specialize mmu1 here. */
#define F_MMU1_LARB(larbid)		(0x1 << SMI_BUS_LARB_SHIFT(larbid))
#define SMI_M4U_TH			0x234
#define SMI_FIFO_TH1			0x238
#define SMI_FIFO_TH2			0x23c
#define SMI_PREULTRA_MASK1		0x244
#define SMI_DCM				0x300
#define SMI_DUMMY			0x444

#define SMI_LARB_OSTD_MAX		0x3fU
#define SMI_COMMON_BWL_MAX		0xfffU
#define SMI_COMMON_PORT_NR		8
#define MTK_SMI_COMMON_NR_MAX		2

enum mtk_smi_gen {
	MTK_SMI_GEN1,
	MTK_SMI_GEN2
};

struct mtk_smi_reg_pair {
	u32 offset;
	u32 value;
};

struct mtk_smi_common_plat {
	enum mtk_smi_gen gen;
	bool             has_gals;
	u32              bus_sel; /* Balance some larbs to enter mmu0 or mmu1 */
	const struct mtk_smi_reg_pair *init;
	unsigned int init_num;
	const char * const *sub_common_compat;
	unsigned int sub_common_num;
	const struct mtk_smi_reg_pair *sub_common_init;
	unsigned int sub_common_init_num;
	bool			has_qos;
	unsigned int		qos_id;
};

struct mtk_smi_larb_gen {
	int port_in_larb[MTK_LARB_NR_MAX + 1];
	void (*config_port)(struct device *dev);
	unsigned int			larb_direct_to_common_mask;
	unsigned int			mmu_force_mask[MTK_LARB_NR_MAX];
	bool				has_gals;
	bool				has_qos;
};

struct mtk_smi {
	struct device			*dev;
	struct clk			*clk_power;
	struct clk			*clk_apb, *clk_smi;
	struct clk			*clk_gals0, *clk_gals1;
	struct clk			*clk_async; /*only needed by mt2701*/
	union {
		void __iomem		*smi_ao_base; /* only for gen1 */
		void __iomem		*base;	      /* only for gen2 */
	};
	const struct mtk_smi_common_plat *plat;
	void __iomem			**sub_common_base;
	struct mutex			qos_lock; /* protects QoS shadow state */
	u16				bwl[SMI_COMMON_PORT_NR];
	DECLARE_BITMAP(bwl_valid, SMI_COMMON_PORT_NR);
};

struct mtk_smi_larb { /* larb: local arbiter */
	struct mtk_smi			smi;
	void __iomem			*base;
	struct device			*smi_common_dev;
	const struct mtk_smi_larb_gen	*larb_gen;
	int				larbid;
	u32				*mmu;
	unsigned char			*bank;
	struct mutex			qos_lock; /* protects QoS shadow state */
	u8				ostd[MTK_LARB_NR_MAX];
	DECLARE_BITMAP(ostd_valid, MTK_LARB_NR_MAX);
};

static DECLARE_RWSEM(mtk_smi_qos_devices_lock);
static struct device *mtk_smi_qos_larbs[MTK_LARB_NR_MAX];
static struct device *mtk_smi_qos_commons[MTK_SMI_COMMON_NR_MAX];

static int mtk_smi_qos_register_device(struct device **devices,
				       unsigned int count, unsigned int id,
				       struct device *dev)
{
	int ret = 0;

	if (id >= count)
		return -EINVAL;

	down_write(&mtk_smi_qos_devices_lock);
	if (devices[id] && devices[id] != dev)
		ret = -EBUSY;
	else if (!devices[id])
		devices[id] = get_device(dev);
	up_write(&mtk_smi_qos_devices_lock);

	return ret;
}

static void mtk_smi_qos_unregister_device(struct device **devices,
					  unsigned int count, unsigned int id,
					  struct device *dev)
{
	if (id >= count)
		return;

	down_write(&mtk_smi_qos_devices_lock);
	if (devices[id] == dev) {
		devices[id] = NULL;
		put_device(dev);
	}
	up_write(&mtk_smi_qos_devices_lock);
}

static struct device *mtk_smi_qos_lock_device(struct device **devices,
					      unsigned int count,
					      unsigned int id)
{
	struct device *dev;

	if (id >= count)
		return NULL;

	down_read(&mtk_smi_qos_devices_lock);
	dev = devices[id];
	if (!dev)
		up_read(&mtk_smi_qos_devices_lock);

	return dev;
}

static void mtk_smi_qos_unlock_device(void)
{
	up_read(&mtk_smi_qos_devices_lock);
}

static int mtk_smi_clk_enable(const struct mtk_smi *smi)
{
	int ret;

	ret = clk_prepare_enable(smi->clk_power);
	if (ret)
		return ret;

	ret = clk_prepare_enable(smi->clk_apb);
	if (ret)
		goto err_disable_power;

	ret = clk_prepare_enable(smi->clk_smi);
	if (ret)
		goto err_disable_apb;

	ret = clk_prepare_enable(smi->clk_gals0);
	if (ret)
		goto err_disable_smi;

	ret = clk_prepare_enable(smi->clk_gals1);
	if (ret)
		goto err_disable_gals0;

	return 0;

err_disable_gals0:
	clk_disable_unprepare(smi->clk_gals0);
err_disable_smi:
	clk_disable_unprepare(smi->clk_smi);
err_disable_apb:
	clk_disable_unprepare(smi->clk_apb);
err_disable_power:
	clk_disable_unprepare(smi->clk_power);
	return ret;
}

static void mtk_smi_clk_disable(const struct mtk_smi *smi)
{
	clk_disable_unprepare(smi->clk_gals1);
	clk_disable_unprepare(smi->clk_gals0);
	clk_disable_unprepare(smi->clk_smi);
	clk_disable_unprepare(smi->clk_apb);
	clk_disable_unprepare(smi->clk_power);
}

static void mtk_smi_larb_apply_qos(struct mtk_smi_larb *larb)
{
	unsigned int port;

	mutex_lock(&larb->qos_lock);
	for_each_set_bit(port, larb->ostd_valid, MTK_LARB_NR_MAX)
		writel(larb->ostd[port],
		       larb->base + SMI_LARB_OSTDL_PORT(port));
	mutex_unlock(&larb->qos_lock);
}

static void mtk_smi_common_apply_qos(struct mtk_smi *common)
{
	unsigned int port;

	mutex_lock(&common->qos_lock);
	for_each_set_bit(port, common->bwl_valid, SMI_COMMON_PORT_NR)
		writel(common->bwl[port], common->base + SMI_L1ARB(port));
	mutex_unlock(&common->qos_lock);
}

int mtk_smi_larb_set_ostd(unsigned int larb_id, unsigned int port,
			  unsigned int ostd)
{
	struct mtk_smi_larb *larb;
	struct device *dev;
	int ret;

	if (port >= MTK_LARB_NR_MAX)
		return -EINVAL;

	dev = mtk_smi_qos_lock_device(mtk_smi_qos_larbs,
				      ARRAY_SIZE(mtk_smi_qos_larbs), larb_id);
	if (!dev)
		return -ENODEV;

	larb = dev_get_drvdata(dev);
	ostd = clamp(ostd, 1U, SMI_LARB_OSTD_MAX);
	mutex_lock(&larb->qos_lock);
	larb->ostd[port] = ostd;
	set_bit(port, larb->ostd_valid);
	mutex_unlock(&larb->qos_lock);

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto out_unlock_device;

	mutex_lock(&larb->qos_lock);
	writel(larb->ostd[port],
	       larb->base + SMI_LARB_OSTDL_PORT(port));
	mutex_unlock(&larb->qos_lock);
	pm_runtime_put_sync(dev);
	ret = 0;

out_unlock_device:
	mtk_smi_qos_unlock_device();
	return ret;
}

int mtk_smi_common_set_bwl(unsigned int common_id, unsigned int port,
			   unsigned int bwl, bool soft)
{
	struct mtk_smi *common;
	struct device *dev;
	u32 value;
	int ret;

	if (port >= SMI_COMMON_PORT_NR)
		return -EINVAL;

	dev = mtk_smi_qos_lock_device(mtk_smi_qos_commons,
				      ARRAY_SIZE(mtk_smi_qos_commons), common_id);
	if (!dev)
		return -ENODEV;

	common = dev_get_drvdata(dev);
	value = (soft ? 0x1000 : 0x3000) | min(bwl, SMI_COMMON_BWL_MAX);
	mutex_lock(&common->qos_lock);
	common->bwl[port] = value;
	set_bit(port, common->bwl_valid);
	mutex_unlock(&common->qos_lock);

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto out_unlock_device;

	mutex_lock(&common->qos_lock);
	writel(common->bwl[port], common->base + SMI_L1ARB(port));
	mutex_unlock(&common->qos_lock);
	pm_runtime_put_sync(dev);
	ret = 0;

out_unlock_device:
	mtk_smi_qos_unlock_device();
	return ret;
}

int mtk_smi_larb_get(struct device *larbdev)
{
	int ret = pm_runtime_resume_and_get(larbdev);

	return (ret < 0) ? ret : 0;
}
EXPORT_SYMBOL_GPL(mtk_smi_larb_get);

void mtk_smi_larb_put(struct device *larbdev)
{
	pm_runtime_put_sync(larbdev);
}
EXPORT_SYMBOL_GPL(mtk_smi_larb_put);

static int mtk_smi_larb_config_mmu(struct mtk_smi_larb *larb)
{
	unsigned long mmu_mask;
	u32 reg;
	int i;

	if (larb->larbid < 0 || !larb->mmu || !larb->bank)
		return -ENODEV;

	if (BIT(larb->larbid) & larb->larb_gen->larb_direct_to_common_mask)
		return 0;

	mmu_mask = READ_ONCE(*larb->mmu) |
		   larb->larb_gen->mmu_force_mask[larb->larbid];
	for_each_set_bit(i, &mmu_mask, 32) {
		reg = readl_relaxed(larb->base + SMI_LARB_NONSEC_CON(i));
		reg |= F_MMU_EN;
		reg |= BANK_SEL(larb->bank[i]);
		writel(reg, larb->base + SMI_LARB_NONSEC_CON(i));
	}

	wmb();
	return 0;
}

int mtk_smi_larb_reapply_mmu(struct device *larbdev)
{
	struct mtk_smi_larb *larb;
	int ret;

	if (!larbdev)
		return -EINVAL;

	larb = dev_get_drvdata(larbdev);
	if (!larb)
		return -ENODEV;

	ret = mtk_smi_larb_config_mmu(larb);
	if (!ret && larb->larb_gen->mmu_force_mask[larb->larbid])
		dev_info_once(larbdev,
			      "larb%d MMU ports: mask=%#x p0=%#x p1=%#x\n",
			      larb->larbid, READ_ONCE(*larb->mmu),
			      readl(larb->base + SMI_LARB_NONSEC_CON(0)),
			      readl(larb->base + SMI_LARB_NONSEC_CON(1)));

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_smi_larb_reapply_mmu);

static int
mtk_smi_larb_bind(struct device *dev, struct device *master, void *data)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(dev);
	struct mtk_smi_larb_iommu *larb_mmu = data;
	unsigned int         i;
	int ret;

	for (i = 0; i < MTK_LARB_NR_MAX; i++) {
		if (dev == larb_mmu[i].dev) {
			if (larb->larbid >= 0 && larb->larbid != i) {
				dev_err(dev, "DT larb id %d does not match IOMMU id %u\n",
					larb->larbid, i);
				return -EINVAL;
			}
			if (larb->larbid < 0)
				larb->larbid = i;
			larb->mmu = &larb_mmu[i].mmu;
			larb->bank = larb_mmu[i].bank;
			if (larb->larb_gen->has_qos) {
				ret = mtk_smi_qos_register_device(mtk_smi_qos_larbs,
								  ARRAY_SIZE(mtk_smi_qos_larbs),
								  larb->larbid, dev);
				if (ret) {
					larb->mmu = NULL;
					larb->bank = NULL;
					return ret;
				}
			}
			return 0;
		}
	}
	return -ENODEV;
}

static void mtk_smi_larb_config_port_gen2_general(struct device *dev)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(dev);

	mtk_smi_larb_config_mmu(larb);
}

struct mtk_smi_larb_mt6877_config {
	u8 num_ports;
	u8 ostd[29];
	u8 cmd_group[2];
	u8 bw_throttle[2];
};

/* MT6877 ESL settings, restored after power loss before cached MMQoS votes. */
static const struct mtk_smi_larb_mt6877_config mtk_smi_larb_mt6877_config[] = {
	[0] = {
		.num_ports = 5,
		.ostd = { 0x2, 0x2, 0x30, 0x6, 0x30 },
		.cmd_group = { 2, 3 },
		.bw_throttle = { 4, 5 },
	},
	[1] = {
		.num_ports = 7,
		.ostd = { 0x2, 0x18, 0x8, 0xc, 0x28, 0x2, 0x18 },
		.cmd_group = { 1, 2 },
		.bw_throttle = { 4, 5 },
	},
	[2] = {
		.num_ports = 5,
		.ostd = { 0x2, 0x2, 0x2, 0x2, 0x1 },
		.bw_throttle = { 0, 5 },
	},
	[4] = {
		.num_ports = 12,
		.ostd = { 0x35, 0x14, 0x9, 0x1, 0x1, 0x1,
			  0x1, 0x1, 0x1, 0x3, 0x11, 0x1 },
		.bw_throttle = { 0, 12 },
	},
	[7] = {
		.num_ports = 15,
		.ostd = { 0x1, 0x3, 0x1, 0x1, 0x1, 0x1, 0x4, 0x4,
			  0x1, 0x3, 0x2, 0x4, 0x2, 0xf, 0x10 },
		.bw_throttle = { 0, 15 },
	},
	[9] = {
		.num_ports = 29,
		.ostd = { 0x6, 0x3, 0xc, 0x6, 0x1, 0x4, 0x4, 0x2,
			  0x2, 0x5, 0x5, 0x2, 0x6, 0x3, 0x3, 0xb,
			  0x1, 0x6, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1,
			  0x1, 0x1, 0x1, 0x1, 0x1 },
		.bw_throttle = { 0, 29 },
	},
	[11] = {
		.num_ports = 29,
		.ostd = { 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1,
			  0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0xb,
			  0x1, 0x6, 0x6, 0x5, 0x6, 0x1, 0x5, 0x2,
			  0xc, 0x6, 0x1, 0x1, 0x1 },
		.bw_throttle = { 0, 29 },
	},
	[13] = {
		.num_ports = 15,
		.ostd = { 0x2, 0xa, 0xa, 0xc, 0x6, 0x6, 0x6, 0x6,
			  0x6, 0xe, 0x4, 0x1, 0x6, 0x6, 0x2 },
		.bw_throttle = { 11, 12 },
	},
	[14] = {
		.num_ports = 10,
		.ostd = { 0x1, 0x1, 0x1, 0x2a, 0xe, 0x4, 0xc, 0xc,
			  0x6, 0x6 },
	},
	[16] = {
		.num_ports = 17,
		.ostd = { 0x28, 0x10, 0x2, 0x8, 0x14, 0x2, 0x1e, 0x10,
			  0x4, 0x2, 0x4, 0x2, 0x4, 0x2, 0x6, 0x2, 0x4 },
	},
	[17] = {
		.num_ports = 17,
		.ostd = { 0x28, 0x10, 0x2, 0x8, 0x14, 0x2, 0x1e, 0x10,
			  0x4, 0x2, 0x4, 0x2, 0x4, 0x2, 0x6, 0x2, 0x4 },
	},
	[19] = {
		.num_ports = 4,
		.ostd = { 0x2, 0x2, 0x3, 0x2 },
		.bw_throttle = { 0, 4 },
	},
	[20] = {
		.num_ports = 6,
		.ostd = { 0x7, 0x7, 0x4, 0x4, 0x1, 0x1 },
		.bw_throttle = { 0, 6 },
	},
};

static void mtk_smi_larb_config_port_mt6877(struct device *dev)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(dev);
	const struct mtk_smi_larb_mt6877_config *config;
	unsigned int i;
	u32 reg;

	mtk_smi_larb_config_port_gen2_general(dev);

	if (larb->larbid < 0 ||
	    larb->larbid >= ARRAY_SIZE(mtk_smi_larb_mt6877_config))
		return;

	config = &mtk_smi_larb_mt6877_config[larb->larbid];
	if (!config->num_ports)
		return;

	for (i = 0; i < config->num_ports; i++)
		writel_relaxed(config->ostd[i],
			       larb->base + SMI_LARB_OSTDL_PORT(i));

	writel_relaxed(0x370256, larb->base + SMI_LARB_CMD_THRT_CON);
	writel_relaxed(1, larb->base + SMI_LARB_SW_FLAG);

	/* RAW A/B require the reference AAHO ultra-priority setting as well. */
	if (larb->larbid == 16 || larb->larbid == 17)
		writel_relaxed(0x8000, larb->base + SMI_LARB_FORCE_ULTRA);

	for (i = config->cmd_group[0]; i < config->cmd_group[1]; i++) {
		reg = readl_relaxed(larb->base + SMI_LARB_NONSEC_CON(i));
		writel_relaxed(reg | BIT(1),
			       larb->base + SMI_LARB_NONSEC_CON(i));
	}

	for (i = config->bw_throttle[0]; i < config->bw_throttle[1]; i++) {
		reg = readl_relaxed(larb->base + SMI_LARB_NONSEC_CON(i));
		writel_relaxed(reg | BIT(3), larb->base + SMI_LARB_NONSEC_CON(i));
	}
	wmb();
}

static void mtk_smi_larb_config_port_mt8173(struct device *dev)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(dev);

	writel(*larb->mmu, larb->base + SMI_LARB_MMU_EN);
}

static void mtk_smi_larb_config_port_mt8167(struct device *dev)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(dev);

	writel(*larb->mmu, larb->base + MT8167_SMI_LARB_MMU_EN);
}

static void mtk_smi_larb_config_port_gen1(struct device *dev)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(dev);
	const struct mtk_smi_larb_gen *larb_gen = larb->larb_gen;
	struct mtk_smi *common = dev_get_drvdata(larb->smi_common_dev);
	int i, m4u_port_id, larb_port_num;
	u32 sec_con_val, reg_val;

	m4u_port_id = larb_gen->port_in_larb[larb->larbid];
	larb_port_num = larb_gen->port_in_larb[larb->larbid + 1]
			- larb_gen->port_in_larb[larb->larbid];

	for (i = 0; i < larb_port_num; i++, m4u_port_id++) {
		if (*larb->mmu & BIT(i)) {
			/* bit[port + 3] controls the virtual or physical */
			sec_con_val = SMI_SECUR_CON_VAL_VIRT(m4u_port_id);
		} else {
			/* do not need to enable m4u for this port */
			continue;
		}
		reg_val = readl(common->smi_ao_base
			+ REG_SMI_SECUR_CON_ADDR(m4u_port_id));
		reg_val &= SMI_SECUR_CON_VAL_MSK(m4u_port_id);
		reg_val |= sec_con_val;
		reg_val |= SMI_SECUR_CON_VAL_DOMAIN(m4u_port_id);
		writel(reg_val,
			common->smi_ao_base
			+ REG_SMI_SECUR_CON_ADDR(m4u_port_id));
	}
}

static void
mtk_smi_larb_unbind(struct device *dev, struct device *master, void *data)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(dev);

	if (larb->larb_gen->has_qos)
		mtk_smi_qos_unregister_device(mtk_smi_qos_larbs,
					      ARRAY_SIZE(mtk_smi_qos_larbs),
					      larb->larbid, dev);
	pm_runtime_barrier(dev);
	larb->mmu = NULL;
	larb->bank = NULL;
}

static const struct component_ops mtk_smi_larb_component_ops = {
	.bind = mtk_smi_larb_bind,
	.unbind = mtk_smi_larb_unbind,
};

static const struct mtk_smi_larb_gen mtk_smi_larb_mt8173 = {
	/* mt8173 do not need the port in larb */
	.config_port = mtk_smi_larb_config_port_mt8173,
};

static const struct mtk_smi_larb_gen mtk_smi_larb_mt8167 = {
	/* mt8167 do not need the port in larb */
	.config_port = mtk_smi_larb_config_port_mt8167,
};

static const struct mtk_smi_larb_gen mtk_smi_larb_mt2701 = {
	.port_in_larb = {
		LARB0_PORT_OFFSET, LARB1_PORT_OFFSET,
		LARB2_PORT_OFFSET, LARB3_PORT_OFFSET
	},
	.config_port = mtk_smi_larb_config_port_gen1,
};

static const struct mtk_smi_larb_gen mtk_smi_larb_mt2712 = {
	.config_port                = mtk_smi_larb_config_port_gen2_general,
	.larb_direct_to_common_mask = BIT(8) | BIT(9),      /* bdpsys */
};

static const struct mtk_smi_larb_gen mtk_smi_larb_mt6779 = {
	.config_port  = mtk_smi_larb_config_port_gen2_general,
	.larb_direct_to_common_mask =
		BIT(4) | BIT(6) | BIT(11) | BIT(12) | BIT(13),
		/* DUMMY | IPU0 | IPU1 | CCU | MDLA */
};

static const struct mtk_smi_larb_gen mtk_smi_larb_mt6877 = {
	.config_port = mtk_smi_larb_config_port_mt6877,
	.mmu_force_mask = { [1] = BIT(0) | BIT(1) },
	.has_qos = true,
};

static const struct mtk_smi_larb_gen mtk_smi_larb_mt8183 = {
	.has_gals                   = true,
	.config_port                = mtk_smi_larb_config_port_gen2_general,
	.larb_direct_to_common_mask = BIT(2) | BIT(3) | BIT(7),
				      /* IPU0 | IPU1 | CCU */
};

static const struct mtk_smi_larb_gen mtk_smi_larb_mt8192 = {
	.config_port                = mtk_smi_larb_config_port_gen2_general,
};

static const struct of_device_id mtk_smi_larb_of_ids[] = {
	{
		.compatible = "mediatek,mt8167-smi-larb",
		.data = &mtk_smi_larb_mt8167
	},
	{
		.compatible = "mediatek,mt8173-smi-larb",
		.data = &mtk_smi_larb_mt8173
	},
	{
		.compatible = "mediatek,mt2701-smi-larb",
		.data = &mtk_smi_larb_mt2701
	},
	{
		.compatible = "mediatek,mt2712-smi-larb",
		.data = &mtk_smi_larb_mt2712
	},
	{
		.compatible = "mediatek,mt6779-smi-larb",
		.data = &mtk_smi_larb_mt6779
	},
	{
		.compatible = "mediatek,mt6877-smi-larb",
		.data = &mtk_smi_larb_mt6877
	},
	{
		.compatible = "mediatek,mt8183-smi-larb",
		.data = &mtk_smi_larb_mt8183
	},
	{
		.compatible = "mediatek,mt8192-smi-larb",
		.data = &mtk_smi_larb_mt8192
	},
	{}
};

static int mtk_smi_larb_probe(struct platform_device *pdev)
{
	struct mtk_smi_larb *larb;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct device_node *smi_node;
	struct platform_device *smi_pdev;
	struct device_link *link;
	u32 larbid;
	int ret;

	larb = devm_kzalloc(dev, sizeof(*larb), GFP_KERNEL);
	if (!larb)
		return -ENOMEM;

	larb->larb_gen = of_device_get_match_data(dev);
	larb->larbid = -1;
	if (!of_property_read_u32(dev->of_node, "mediatek,larb-id", &larbid)) {
		if (larbid >= MTK_LARB_NR_MAX)
			return dev_err_probe(dev, -EINVAL,
					     "invalid larb id %u\n", larbid);
		larb->larbid = larbid;
	}
	if (larb->larb_gen->has_qos && larb->larbid < 0)
		return dev_err_probe(dev, -EINVAL, "missing mediatek,larb-id\n");
	mutex_init(&larb->qos_lock);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	larb->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(larb->base))
		return PTR_ERR(larb->base);

	larb->smi.clk_power = devm_clk_get_optional(dev, "power");
	if (IS_ERR(larb->smi.clk_power))
		return PTR_ERR(larb->smi.clk_power);

	larb->smi.clk_apb = devm_clk_get(dev, "apb");
	if (IS_ERR(larb->smi.clk_apb))
		return PTR_ERR(larb->smi.clk_apb);

	larb->smi.clk_smi = devm_clk_get(dev, "smi");
	if (IS_ERR(larb->smi.clk_smi))
		return PTR_ERR(larb->smi.clk_smi);

	if (larb->larb_gen->has_gals) {
		/* The larbs may still haven't gals even if the SoC support.*/
		larb->smi.clk_gals0 = devm_clk_get(dev, "gals");
		if (PTR_ERR(larb->smi.clk_gals0) == -ENOENT)
			larb->smi.clk_gals0 = NULL;
		else if (IS_ERR(larb->smi.clk_gals0))
			return PTR_ERR(larb->smi.clk_gals0);
	}
	larb->smi.dev = dev;

	smi_node = of_parse_phandle(dev->of_node, "mediatek,smi", 0);
	if (!smi_node)
		return -EINVAL;

	smi_pdev = of_find_device_by_node(smi_node);
	of_node_put(smi_node);
	if (smi_pdev) {
		if (!platform_get_drvdata(smi_pdev))
			return -EPROBE_DEFER;
		larb->smi_common_dev = &smi_pdev->dev;
		link = device_link_add(dev, larb->smi_common_dev,
				       DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS);
		if (!link) {
			dev_err(dev, "Unable to link smi-common dev\n");
			return -ENODEV;
		}
	} else {
		dev_err(dev, "Failed to get the smi_common device\n");
		return -EINVAL;
	}

	pm_runtime_enable(dev);
	platform_set_drvdata(pdev, larb);
	ret = component_add(dev, &mtk_smi_larb_component_ops);
	if (ret) {
		pm_runtime_disable(dev);
		device_link_remove(dev, larb->smi_common_dev);
	}

	return ret;
}

static int mtk_smi_larb_remove(struct platform_device *pdev)
{
	struct mtk_smi_larb *larb = platform_get_drvdata(pdev);

	component_del(&pdev->dev, &mtk_smi_larb_component_ops);
	device_link_remove(&pdev->dev, larb->smi_common_dev);
	pm_runtime_disable(&pdev->dev);
	return 0;
}

static int __maybe_unused mtk_smi_larb_resume(struct device *dev)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(dev);
	const struct mtk_smi_larb_gen *larb_gen = larb->larb_gen;
	int ret;

	ret = mtk_smi_clk_enable(&larb->smi);
	if (ret < 0) {
		dev_err(dev, "Failed to enable clock(%d).\n", ret);
		return ret;
	}

	/* Configure the basic setting for this larb */
	larb_gen->config_port(dev);
	mtk_smi_larb_apply_qos(larb);

	return 0;
}

static int __maybe_unused mtk_smi_larb_suspend(struct device *dev)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(dev);

	mtk_smi_clk_disable(&larb->smi);
	return 0;
}

static const struct dev_pm_ops smi_larb_pm_ops = {
	SET_RUNTIME_PM_OPS(mtk_smi_larb_suspend, mtk_smi_larb_resume, NULL)
	SET_LATE_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				     pm_runtime_force_resume)
};

static struct platform_driver mtk_smi_larb_driver = {
	.probe	= mtk_smi_larb_probe,
	.remove	= mtk_smi_larb_remove,
	.driver	= {
		.name = "mtk-smi-larb",
		.of_match_table = mtk_smi_larb_of_ids,
		.pm             = &smi_larb_pm_ops,
	}
};

static const struct mtk_smi_common_plat mtk_smi_common_gen1 = {
	.gen = MTK_SMI_GEN1,
};

static const struct mtk_smi_common_plat mtk_smi_common_gen2 = {
	.gen = MTK_SMI_GEN2,
};

static const struct mtk_smi_common_plat mtk_smi_common_mt6779 = {
	.gen		= MTK_SMI_GEN2,
	.has_gals	= true,
	.bus_sel	= F_MMU1_LARB(1) | F_MMU1_LARB(2) | F_MMU1_LARB(4) |
			  F_MMU1_LARB(5) | F_MMU1_LARB(6) | F_MMU1_LARB(7),
};

static const struct mtk_smi_reg_pair mtk_smi_common_mt6877_init[] = {
	{ SMI_L1LEN, 0xb },
	{ SMI_L1ARB(0), 0 },
	{ SMI_L1ARB(1), 0 },
	{ SMI_L1ARB(2), 0 },
	{ SMI_L1ARB(3), 0 },
	{ SMI_L1ARB(4), 0 },
	{ SMI_L1ARB(5), 0 },
	{ SMI_L1ARB(6), 0 },
	{ SMI_L1ARB(7), 0 },
	{ SMI_M4U_TH, 0xe100e10 },
	{ SMI_FIFO_TH1, 0x90a090a },
	{ SMI_FIFO_TH2, 0x506090a },
	{ SMI_DCM, 0x4f1 },
	{ SMI_DUMMY, 0x1 },
};

static const char * const mtk_smi_common_mt6877_sub_common_compat[] = {
	"mediatek,disp_smi_2x1_sub_common_u0",
	"mediatek,disp_smi_2x1_sub_common_u1",
};

static const struct mtk_smi_reg_pair mtk_smi_common_mt6877_sub_common_init[] = {
	{ SMI_L1LEN, 0xa },
	{ SMI_L1ARB(0), 0 },
	{ SMI_L1ARB(1), 0 },
	{ SMI_L1ARB(2), 0 },
	{ SMI_L1ARB(3), 0 },
	{ SMI_L1ARB(4), 0 },
	{ SMI_L1ARB(5), 0 },
	{ SMI_L1ARB(6), 0 },
	{ SMI_L1ARB(7), 0 },
	{ SMI_PREULTRA_MASK1, 0x2105 },
	{ SMI_DCM, 0x4f1 },
	{ SMI_DUMMY, 0x1 },
};

static const struct mtk_smi_common_plat mtk_smi_common_mt6877 = {
	.gen                 = MTK_SMI_GEN2,
	.has_gals            = true,
	.bus_sel             = F_MMU1_LARB(1) | F_MMU1_LARB(2) |
			       F_MMU1_LARB(5) | F_MMU1_LARB(6),
	.init                = mtk_smi_common_mt6877_init,
	.init_num            = ARRAY_SIZE(mtk_smi_common_mt6877_init),
	.sub_common_compat   = mtk_smi_common_mt6877_sub_common_compat,
	.sub_common_num      = ARRAY_SIZE(mtk_smi_common_mt6877_sub_common_compat),
	.sub_common_init     = mtk_smi_common_mt6877_sub_common_init,
	.sub_common_init_num = ARRAY_SIZE(mtk_smi_common_mt6877_sub_common_init),
	.has_qos             = true,
	.qos_id              = 0,
};

static const struct mtk_smi_common_plat mtk_smi_common_mt8183 = {
	.gen      = MTK_SMI_GEN2,
	.has_gals = true,
	.bus_sel  = F_MMU1_LARB(1) | F_MMU1_LARB(2) | F_MMU1_LARB(5) |
		    F_MMU1_LARB(7),
};

static const struct mtk_smi_common_plat mtk_smi_common_mt8192 = {
	.gen      = MTK_SMI_GEN2,
	.has_gals = true,
	.bus_sel  = F_MMU1_LARB(1) | F_MMU1_LARB(2) | F_MMU1_LARB(5) |
		    F_MMU1_LARB(6),
};

static const struct of_device_id mtk_smi_common_of_ids[] = {
	{
		.compatible = "mediatek,mt8173-smi-common",
		.data = &mtk_smi_common_gen2,
	},
	{
		.compatible = "mediatek,mt8167-smi-common",
		.data = &mtk_smi_common_gen2,
	},
	{
		.compatible = "mediatek,mt2701-smi-common",
		.data = &mtk_smi_common_gen1,
	},
	{
		.compatible = "mediatek,mt2712-smi-common",
		.data = &mtk_smi_common_gen2,
	},
	{
		.compatible = "mediatek,mt6779-smi-common",
		.data = &mtk_smi_common_mt6779,
	},
	{
		.compatible = "mediatek,mt6877-smi-common",
		.data = &mtk_smi_common_mt6877,
	},
	{
		.compatible = "mediatek,mt8183-smi-common",
		.data = &mtk_smi_common_mt8183,
	},
	{
		.compatible = "mediatek,mt8192-smi-common",
		.data = &mtk_smi_common_mt8192,
	},
	{}
};

static int mtk_smi_common_map_sub_commons(struct mtk_smi *common)
{
	const struct mtk_smi_common_plat *plat = common->plat;
	struct device *dev = common->dev;
	struct device_node *parent, *node;
	unsigned int i;
	int ret = 0;

	if (!plat->sub_common_num)
		return 0;

	common->sub_common_base = devm_kcalloc(dev, plat->sub_common_num,
					       sizeof(*common->sub_common_base),
					       GFP_KERNEL);
	if (!common->sub_common_base)
		return -ENOMEM;

	parent = of_get_parent(dev->of_node);
	if (!parent)
		return dev_err_probe(dev, -ENODEV,
				     "failed to find SMI common parent\n");

	for (i = 0; i < plat->sub_common_num; i++) {
		node = of_get_compatible_child(parent,
					       plat->sub_common_compat[i]);
		if (!node) {
			ret = dev_err_probe(dev, -ENODEV,
					    "failed to find %s\n",
					    plat->sub_common_compat[i]);
			break;
		}

		common->sub_common_base[i] = devm_of_iomap(dev, node, 0, NULL);
		of_node_put(node);
		if (IS_ERR(common->sub_common_base[i])) {
			ret = dev_err_probe(dev,
					    PTR_ERR(common->sub_common_base[i]),
					    "failed to map %s\n",
					    plat->sub_common_compat[i]);
			break;
		}
	}

	of_node_put(parent);
	return ret;
}

static int mtk_smi_common_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_smi *common;
	struct resource *res;
	int ret;

	common = devm_kzalloc(dev, sizeof(*common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;
	common->dev = dev;
	common->plat = of_device_get_match_data(dev);
	mutex_init(&common->qos_lock);

	common->clk_power = devm_clk_get_optional(dev, "power");
	if (IS_ERR(common->clk_power))
		return PTR_ERR(common->clk_power);

	common->clk_apb = devm_clk_get(dev, "apb");
	if (IS_ERR(common->clk_apb))
		return PTR_ERR(common->clk_apb);

	common->clk_smi = devm_clk_get(dev, "smi");
	if (IS_ERR(common->clk_smi))
		return PTR_ERR(common->clk_smi);

	if (common->plat->has_gals) {
		common->clk_gals0 = devm_clk_get(dev, "gals0");
		if (IS_ERR(common->clk_gals0))
			return PTR_ERR(common->clk_gals0);

		common->clk_gals1 = devm_clk_get(dev, "gals1");
		if (IS_ERR(common->clk_gals1))
			return PTR_ERR(common->clk_gals1);
	}

	/*
	 * for mtk smi gen 1, we need to get the ao(always on) base to config
	 * m4u port, and we need to enable the aync clock for transform the smi
	 * clock into emi clock domain, but for mtk smi gen2, there's no smi ao
	 * base.
	 */
	if (common->plat->gen == MTK_SMI_GEN1) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		common->smi_ao_base = devm_ioremap_resource(dev, res);
		if (IS_ERR(common->smi_ao_base))
			return PTR_ERR(common->smi_ao_base);

		common->clk_async = devm_clk_get(dev, "async");
		if (IS_ERR(common->clk_async))
			return PTR_ERR(common->clk_async);

		ret = clk_prepare_enable(common->clk_async);
		if (ret)
			return ret;
	} else {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		common->base = devm_ioremap_resource(dev, res);
		if (IS_ERR(common->base))
			return PTR_ERR(common->base);
	}

	ret = mtk_smi_common_map_sub_commons(common);
	if (ret)
		return ret;

	pm_runtime_enable(dev);
	platform_set_drvdata(pdev, common);
	if (common->plat->has_qos) {
		ret = mtk_smi_qos_register_device(mtk_smi_qos_commons,
						  ARRAY_SIZE(mtk_smi_qos_commons),
						  common->plat->qos_id, dev);
		if (ret) {
			pm_runtime_disable(dev);
			return dev_err_probe(dev, ret,
					     "failed to register QoS common\n");
		}
	}
	return 0;
}

static int mtk_smi_common_remove(struct platform_device *pdev)
{
	struct mtk_smi *common = platform_get_drvdata(pdev);

	if (common->plat->has_qos)
		mtk_smi_qos_unregister_device(mtk_smi_qos_commons,
					      ARRAY_SIZE(mtk_smi_qos_commons),
					      common->plat->qos_id,
					      &pdev->dev);
	pm_runtime_barrier(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return 0;
}

static int __maybe_unused mtk_smi_common_resume(struct device *dev)
{
	struct mtk_smi *common = dev_get_drvdata(dev);
	u32 bus_sel = common->plat->bus_sel;
	unsigned int i, j;
	int ret;

	ret = mtk_smi_clk_enable(common);
	if (ret) {
		dev_err(common->dev, "Failed to enable clock(%d).\n", ret);
		return ret;
	}

	for (i = 0; i < common->plat->sub_common_num; i++)
		for (j = 0; j < common->plat->sub_common_init_num; j++)
			writel_relaxed(common->plat->sub_common_init[j].value,
				       common->sub_common_base[i] +
				       common->plat->sub_common_init[j].offset);

	if (common->plat->gen == MTK_SMI_GEN2 && bus_sel)
		writel(bus_sel, common->base + SMI_BUS_SEL);

	for (i = 0; i < common->plat->init_num; i++)
		writel_relaxed(common->plat->init[i].value,
			       common->base + common->plat->init[i].offset);
	if (common->plat->init_num || common->plat->sub_common_init_num)
		wmb();
	mtk_smi_common_apply_qos(common);
	return 0;
}

static int __maybe_unused mtk_smi_common_suspend(struct device *dev)
{
	struct mtk_smi *common = dev_get_drvdata(dev);

	mtk_smi_clk_disable(common);
	return 0;
}

static const struct dev_pm_ops smi_common_pm_ops = {
	SET_RUNTIME_PM_OPS(mtk_smi_common_suspend, mtk_smi_common_resume, NULL)
	SET_LATE_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				     pm_runtime_force_resume)
};

static struct platform_driver mtk_smi_common_driver = {
	.probe	= mtk_smi_common_probe,
	.remove = mtk_smi_common_remove,
	.driver	= {
		.name = "mtk-smi-common",
		.of_match_table = mtk_smi_common_of_ids,
		.pm             = &smi_common_pm_ops,
	}
};

static struct platform_driver * const smidrivers[] = {
	&mtk_smi_common_driver,
	&mtk_smi_larb_driver,
};

static int __init mtk_smi_init(void)
{
	return platform_register_drivers(smidrivers, ARRAY_SIZE(smidrivers));
}
module_init(mtk_smi_init);

static void __exit mtk_smi_exit(void)
{
	platform_unregister_drivers(smidrivers, ARRAY_SIZE(smidrivers));
}
module_exit(mtk_smi_exit);

MODULE_DESCRIPTION("MediaTek SMI driver");
MODULE_LICENSE("GPL v2");
