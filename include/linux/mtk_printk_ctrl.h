/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015 MediaTek Inc.
 */

#ifndef _LINUX_MTK_PRINTK_CTRL_H
#define _LINUX_MTK_PRINTK_CTRL_H

#include <linux/types.h>

#ifdef CONFIG_PRINTK_MTK_UART_CONSOLE
bool mt_get_uartlog_status(void);
void mt_disable_uart(void);
void mt_enable_uart(void);
#else
static inline bool mt_get_uartlog_status(void)
{
	return false;
}

static inline void mt_disable_uart(void)
{
}

static inline void mt_enable_uart(void)
{
}
#endif

#endif /* _LINUX_MTK_PRINTK_CTRL_H */
