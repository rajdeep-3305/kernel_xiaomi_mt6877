/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MT6877_FHCTL_H
#define __MT6877_FHCTL_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_MTK_MT6877_FHCTL)
int mt6877_fhctl_ready(void);
int mt6877_fhctl_hop(unsigned int pll, u32 dds);
#elif IS_ENABLED(CONFIG_MTK_FREQ_HOPPING) && IS_ENABLED(CONFIG_MACH_MT6877)
/* The legacy fhctl_new provider owns this DT node on Ruby. */
int mt6877_fhctl_ready(void);
int mt6877_fhctl_hop(unsigned int pll, u32 dds);
#else
static inline int mt6877_fhctl_ready(void) { return -ENODEV; }
static inline int mt6877_fhctl_hop(unsigned int pll, u32 dds)
{
	return -EOPNOTSUPP;
}
#endif

#endif
