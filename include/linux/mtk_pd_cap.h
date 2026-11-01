/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MTK_PD_CAP_H
#define _LINUX_MTK_PD_CAP_H

#include <linux/types.h>

#define ADAPTER_CAP_MAX_NR 10

/* Shared by the adapter interface and its charge-pump consumers. */
struct pd_cap {
	u8 selected_cap_idx;
	u8 nr;
	u8 pdp;
	u8 pwr_limit[ADAPTER_CAP_MAX_NR];
	int max_mv[ADAPTER_CAP_MAX_NR];
	int min_mv[ADAPTER_CAP_MAX_NR];
	int ma[ADAPTER_CAP_MAX_NR];
	int maxwatt[ADAPTER_CAP_MAX_NR];
	int minwatt[ADAPTER_CAP_MAX_NR];
	u8 type[ADAPTER_CAP_MAX_NR];
	int info[ADAPTER_CAP_MAX_NR];
};

#endif /* _LINUX_MTK_PD_CAP_H */
