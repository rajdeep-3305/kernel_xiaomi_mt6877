// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/proc_fs.h>
#include <linux/errno.h>

#include "boost_ctrl.h"

int init_boostctrl(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *bstctrl_root = NULL;
	struct proc_dir_entry *easctrl_root = NULL;
	int cpu_ctrl_ready = 0;
	int dram_ctrl_ready = 0;
	int ret;

	pr_debug("__init %s\n", __func__);


	bstctrl_root = proc_mkdir("boost_ctrl", parent);
	if (!bstctrl_root)
		return -ENOMEM;

	/* Initialize topology before its consumers. */
	ret = topo_ctrl_init(bstctrl_root);
	if (ret)
		goto err_remove_root;

	ret = cpu_ctrl_init(bstctrl_root);
	if (ret)
		goto err_remove_root;
	cpu_ctrl_ready = 1;

	ret = dram_ctrl_init(bstctrl_root);
	if (ret)
		goto err_remove_root;
	dram_ctrl_ready = 1;

	/* EAS */
	easctrl_root = proc_mkdir("eas_ctrl", bstctrl_root);
	if (!easctrl_root)
		goto err_no_mem;
	ret = uclamp_ctrl_init(easctrl_root);
	if (ret)
		goto err_remove_root;

	ret = eas_ctrl_init(easctrl_root);
	if (ret)
		goto err_remove_root;

	return 0;

err_no_mem:
	ret = -ENOMEM;
err_remove_root:
	/* Tear down the platform provider before removing its proc parent. */
	if (dram_ctrl_ready)
		dram_ctrl_exit();

	/* Stop remaining proc readers before releasing their backing state. */
	proc_remove(bstctrl_root);
	if (cpu_ctrl_ready)
		cpu_ctrl_exit();
	topo_ctrl_exit();
	return ret;
}
