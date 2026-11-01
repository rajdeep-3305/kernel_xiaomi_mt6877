/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MTK_INTF_H
#define __MTK_INTF_H

#include <linux/mtk_pd_cap.h>

#define ADAPTER_CAP_MAX_NR_BQ 10

enum adapter_ret {
	ADAPTER_OK = 0,
	ADAPTER_NOT_SUPPORT,
	ADAPTER_TIMEOUT,
	ADAPTER_REJECT,
	ADAPTER_ERROR,
	ADAPTER_ADJUST,
};

struct charger_manager;

int adapter_set_cap(int mV, int mA);
int adapter_set_cap_start(int mV, int mA);
int adapter_set_cap_end(int mV, int mA);
int adapter_set_cap_xm(int mV, int mA);
int adapter_set_cap_start_xm(int mV, int mA);
int adapter_set_cap_end_xm(int mV, int mA);
int adapter_get_cap(struct pd_cap *cap);
int adapter_get_output(int *mV, int *mA);
int set_charger_manager(struct charger_manager *info);
int enable_vbus_ovp(bool en);
int wake_up_charger(void);

#endif
