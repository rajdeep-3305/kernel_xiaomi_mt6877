// SPDX-License-Identifier: GPL-2.0
/* MT6877 top-domain frequency hopping through the reference MCUPM ABI. */
#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/soc/mediatek/mt6877-fhctl.h>
#include <linux/soc/mediatek/mtk_tinysys_ipi.h>

#include "../../misc/mediatek/mcupm/mt6853/mcupm_ipi_id.h"
#include "clk-mt6877.h"

#define MT6877_FH_DFS_CMD		0x1006
#define MT6877_FH_KEEP_POSTDIV	9999
#define MT6877_FH_DDS_MASK	GENMASK(21, 0)
#define MT6877_FH_TIMEOUT_MS	10
#define MT6877_FH_DRV_HOP		BIT(0)
#define MT6877_FH_TOP_PLLS	13

struct mt6877_fhctl {
	struct device *dev;
	void __iomem *apmixed;
	u32 enabled;
	u8 transaction;
	bool faulted;
};

struct mt6877_fh_message {
	u32 command;
	u32 args[8];
};

static DEFINE_MUTEX(mt6877_fh_lock);
static struct mt6877_fhctl *mt6877_fh;
/* The mailbox layer can complete a late reply; keep its buffer permanent. */
static u32 mt6877_fh_ack;

static const u16 mt6877_fh_pcw[MT6877_FH_TOP_PLLS] = {
	0x20c, 0x21c, 0x22c, 0x23c, 0, 0x3b4, 0x394,
	0x3a4, 0x354, 0x364, 0x384, 0x374, 0x24c,
};

int mt6877_fhctl_ready(void)
{
	int ret;

	mutex_lock(&mt6877_fh_lock);
	ret = !mt6877_fh || !mcupm_ipi_ready() ? -EPROBE_DEFER :
		mt6877_fh->faulted ? -EIO : 0;
	mutex_unlock(&mt6877_fh_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6877_fhctl_ready);

int mt6877_fhctl_hop(unsigned int pll, u32 dds)
{
	struct mt6877_fh_message message = { .command = MT6877_FH_DFS_CMD };
	struct mt6877_fhctl *fh;
	u32 before, after;
	int ret;

	if (pll >= MT6877_FH_TOP_PLLS || !mt6877_fh_pcw[pll] ||
	    !dds || (dds & ~MT6877_FH_DDS_MASK))
		return -EINVAL;
	mutex_lock(&mt6877_fh_lock);
	fh = mt6877_fh;
	if (!fh || !mcupm_ipi_ready()) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	if (fh->faulted) {
		ret = -EIO;
		goto out;
	}
	if (!(fh->enabled & BIT(pll))) {
		ret = -EACCES;
		goto out;
	}
	/* Require CCF publication before changing a multimedia clock source. */
	ret = mt6877_pll_sync_rate(pll);
	if (ret)
		goto out;
	before = readl(fh->apmixed + mt6877_fh_pcw[pll]);
	if (before == U32_MAX) {
		ret = -EIO;
		goto fault;
	}
	if ((before & MT6877_FH_DDS_MASK) == dds) {
		ret = 0;
		goto out;
	}
	message.args[0] = pll;
	message.args[1] = dds;
	message.args[2] = MT6877_FH_KEEP_POSTDIV;
	message.args[7] = ++fh->transaction;
	WRITE_ONCE(mt6877_fh_ack, U32_MAX);
	ret = mtk_ipi_send_compl(&mcupm_ipidev, CH_S_FHCTL,
				 IPI_SEND_POLLING, &message,
				 sizeof(message) / sizeof(u32), MT6877_FH_TIMEOUT_MS);
	/* The transport uses private negative codes, not Linux errno. */
	if (ret) {
		ret = ret == IPI_COMPL_TIMEOUT ? -ETIMEDOUT : -EIO;
		goto fault;
	}
	after = readl(fh->apmixed + mt6877_fh_pcw[pll]);
	if (READ_ONCE(mt6877_fh_ack) != message.args[7] ||
	    (after & MT6877_FH_DDS_MASK) != dds ||
	    ((before ^ after) & GENMASK(26, 24))) {
		ret = -EIO;
		goto fault;
	}
	ret = mt6877_pll_sync_rate(pll);
	if (ret)
		goto fault;
	goto out;
fault:
	/* A timed-out transaction may still finish; do not send another hop. */
	fh->faulted = true;
	dev_err(fh->dev, "PLL %u hop failed (%d), further transitions blocked\n", pll, ret);
out:
	mutex_unlock(&mt6877_fh_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6877_fhctl_hop);

static int mt6877_fhctl_parse(struct device_node *root, u32 *enabled)
{
	struct device_node *map, *pll;
	const char *domain, *method;
	u32 fh_id, pll_id, perms, seen = 0;
	bool found = false;
	int ret = -EINVAL;

	for_each_available_child_of_node(root, map) {
		if (of_property_read_string(map, "domain", &domain))
			goto put_map;
		/* GPU/APU hopping is owned by their power-aware drivers. */
		if (strcmp(domain, "top"))
			continue;
		if (found || of_property_read_string(map, "method", &method) ||
		    strcmp(method, "fhctl-mcupm"))
			goto put_map;
		found = true;
		for_each_available_child_of_node(map, pll) {
			if (of_property_read_u32(pll, "fh-id", &fh_id) ||
			    of_property_read_u32(pll, "pll-id", &pll_id) ||
			    fh_id >= MT6877_FH_TOP_PLLS || fh_id != pll_id ||
			    !mt6877_fh_pcw[fh_id] || (seen & BIT(fh_id)))
				goto put_pll;
			seen |= BIT(fh_id);
			perms = U32_MAX;
			if (of_find_property(pll, "perms", NULL) &&
			    of_property_read_u32(pll, "perms", &perms))
				goto put_pll;
			if (perms & MT6877_FH_DRV_HOP)
				*enabled |= BIT(fh_id);
		}
	}
	/* Ruby multimedia tables require MMPLL and have a TVDPLL source. */
	return found && (*enabled & BIT(7)) && (*enabled & BIT(12)) ? 0 : -EINVAL;
put_pll:
	of_node_put(pll);
put_map:
	of_node_put(map);
	return ret;
}

static int mt6877_fhctl_probe(struct platform_device *pdev)
{
	struct mtk_ipi_chan_table *channel;
	struct mt6877_fhctl *fh;
	struct resource *res;
	struct device *dev = &pdev->dev;
	int ret;

	mutex_lock(&mt6877_fh_lock);
	if (mt6877_fh) {
		ret = -EBUSY;
		goto out;
	}
	if (!mcupm_ipi_ready() || !mcupm_ipidev.table) {
		ret = -EPROBE_DEFER;
		goto out;
	}
	ret = mt6877_pll_sync_rate(7);
	if (!ret)
		ret = mt6877_pll_sync_rate(12);
	if (ret)
		goto out;
	channel = &mcupm_ipidev.table[CH_S_FHCTL];
	if (!channel->pin_send || !channel->pin_recv ||
	    channel->pin_send->msg_size != sizeof(struct mt6877_fh_message) / sizeof(u32) ||
	    channel->pin_recv->msg_size != 1) {
		ret = -EINVAL;
		goto out;
	}
	fh = devm_kzalloc(dev, sizeof(*fh), GFP_KERNEL);
	if (!fh) {
		ret = -ENOMEM;
		goto out;
	}
	fh->dev = dev;
	ret = mt6877_fhctl_parse(dev->of_node, &fh->enabled);
	if (ret)
		goto out;
	/* Resource 1 is the always-on top APMIXED bank, shared with CCF. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res || res->start != 0x1000c000 || resource_size(res) < SZ_1K) {
		ret = -EINVAL;
		goto out;
	}
	fh->apmixed = devm_ioremap(dev, res->start, resource_size(res));
	if (!fh->apmixed) {
		ret = -ENOMEM;
		goto out;
	}
	ret = mtk_ipi_register(&mcupm_ipidev, CH_S_FHCTL, NULL, NULL,
			       &mt6877_fh_ack);
	if (ret) {
		ret = ret == IPI_DUPLEX ? -EBUSY : -EIO;
		goto out;
	}
	platform_set_drvdata(pdev, fh);
	mt6877_fh = fh;
	dev_info(dev, "MCUPM top-PLL hopping ready (mask %#x), no probe-time hops\n",
		 fh->enabled);
out:
	mutex_unlock(&mt6877_fh_lock);
	return ret ? dev_err_probe(dev, ret, "MCUPM frequency-hopping setup failed\n") : 0;
}

static void mt6877_fhctl_remove(struct platform_device *pdev)
{
	mutex_lock(&mt6877_fh_lock);
	mt6877_fh = NULL;
	mtk_ipi_unregister(&mcupm_ipidev, CH_S_FHCTL);
	mutex_unlock(&mt6877_fh_lock);
}

static const struct of_device_id mt6877_fhctl_match[] = {
	{ .compatible = "mediatek,mt6877-fhctl" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6877_fhctl_match);

static struct platform_driver mt6877_fhctl_driver = {
	.probe = mt6877_fhctl_probe,
	.remove_new = mt6877_fhctl_remove,
	.driver = {
		.name = "mt6877-mcupm-fhctl",
		.of_match_table = mt6877_fhctl_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6877_fhctl_driver);
MODULE_LICENSE("GPL");
