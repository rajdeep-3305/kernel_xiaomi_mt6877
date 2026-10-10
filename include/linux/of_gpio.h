/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * OF helpers for the GPIO API
 *
 * Copyright (c) 2007-2008  MontaVista Software, Inc.
 *
 * Author: Anton Vorontsov <avorontsov@ru.mvista.com>
 */

#ifndef __LINUX_OF_GPIO_H
#define __LINUX_OF_GPIO_H

#include <linux/compiler.h>
#include <linux/gpio/driver.h>
#include <linux/gpio.h>		/* FIXME: Shouldn't be here */
#include <linux/of.h>

struct device_node;

#ifdef CONFIG_OF_GPIO

extern int of_get_named_gpio(const struct device_node *np,
			     const char *list_name, int index);

static inline int of_gpio_named_count(const struct device_node *np,
				      const char *propname)
{
	return of_count_phandle_with_args(np, propname, "#gpio-cells");
}

static inline int of_gpio_count(const struct device_node *np)
{
	return of_gpio_named_count(np, "gpios");
}

#else /* CONFIG_OF_GPIO */

#include <linux/errno.h>

/* Drivers may not strictly depend on the GPIO support, so let them link. */
static inline int of_get_named_gpio(const struct device_node *np,
                                   const char *propname, int index)
{
	return -ENOSYS;
}

static inline int of_gpio_named_count(const struct device_node *np,
				      const char *propname)
{
	return -ENOSYS;
}

static inline int of_gpio_count(const struct device_node *np)
{
	return -ENOSYS;
}

#endif /* CONFIG_OF_GPIO */

#endif /* __LINUX_OF_GPIO_H */
