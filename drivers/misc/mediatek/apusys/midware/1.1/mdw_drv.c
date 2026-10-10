// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/module.h>  /* Needed by all modules */
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <linux/slab.h>

#include "apusys_power.h"

#include "apusys_drv.h"
#include "mdw_dbg.h"
#include "mdw_cmn.h"
#include "mdw_cmd.h"
#include "mdw_mem.h"
#include "mdw_usr.h"
#include "mdw_rsc.h"
#include "mdw_sched.h"
#include "mdw_tag.h"
#include "mdw_sysfs.h"

/* define */
#define APUSYS_DEV_NAME "apusys"

/* global variable */
struct device *mdw_device;
struct miscdevice mdw_miscdev;

/* function declaration */
static int mdw_open(struct inode *, struct file *);
static int mdw_release(struct inode *, struct file *);
static long mdw_ioctl(struct file *, unsigned int, unsigned long);
static long mdw_compat_ioctl(struct file *, unsigned int, unsigned long);

static const struct file_operations mdw_fops = {
	.owner = THIS_MODULE,
	.open = mdw_open,
	.unlocked_ioctl = mdw_ioctl,
	.compat_ioctl = mdw_compat_ioctl,
	.release = mdw_release,
};

static int mdw_open(struct inode *inode, struct file *filp)
{
	struct mdw_usr *u;

	u = mdw_usr_create();
	if (!u)
		return -ENOMEM;

	filp->private_data = u;

	return 0;
}

static int mdw_release(struct inode *inode, struct file *filp)
{
	struct mdw_usr *u;

	u = filp->private_data;
	mdw_usr_put(u);

	return 0;
}

static int mdw_probe(struct platform_device *pdev)
{
	int ret;

	if (!apusys_power_check())
		return -ENODEV;

	/* DT attaches this device to Ruby's bank-3 APU data domain. */
	ret = mdw_mem_init(&pdev->dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot initialize APUSYS DMA-BUF mapping\n");
	mdw_device = &pdev->dev;

	/*
	 * Publish /dev/apusys only after its mapping backend is ready.
	 * Keep the misc device parentless: Ruby's camera HAL SELinux policy
	 * labels /sys/devices/virtual/misc/apusys/queue.
	 */
	memset(&mdw_miscdev, 0, sizeof(mdw_miscdev));
	mdw_miscdev.minor = MISC_DYNAMIC_MINOR;
	mdw_miscdev.name = APUSYS_DEV_NAME;
	mdw_miscdev.fops = &mdw_fops;
	ret = misc_register(&mdw_miscdev);
	if (ret) {
		mdw_mem_exit();
		mdw_device = NULL;
		return ret;
	}

	/* These optional diagnostics do not affect the memory ABI. */
	ret = mdw_sysfs_init(mdw_miscdev.this_device);
	if (ret)
		dev_warn(&pdev->dev, "APUSYS sysfs initialization failed: %d\n", ret);
	dev_dbg(&pdev->dev, "Ruby APUSYS DMA-BUF backend ready (bank 3)\n");
	return 0;
}

static int mdw_remove(struct platform_device *pdev)
{
	mdw_sysfs_exit();
	misc_deregister(&mdw_miscdev);
	mdw_mem_exit();
	mdw_device = NULL;
	return 0;
}

static int mdw_suspend(struct platform_device *pdev, pm_message_t mesg)
{
	return mdw_sched_pause();
}

static int mdw_resume(struct platform_device *pdev)
{
	mdw_sched_restart();
	return 0;
}

static int mdw_handshake(struct apusys_ioctl_hs *hs)
{
	int ret = 0;

	hs->magic_num = mdw_cmd_get_magic();
	hs->cmd_version = mdw_cmd_get_ver();
	switch (hs->type) {
	case APUSYS_HANDSHAKE_BEGIN:
		hs->begin.mem_support = mdw_mem_get_support();
		hs->begin.dev_support = mdw_rsc_get_dev_bmp();
		hs->begin.dev_type_max = 64;
		if (hs->begin.mem_support & (1UL << APUSYS_MEM_VLM)) {
			mdw_mem_get_vlm(&hs->begin.vlm_start,
				&hs->begin.vlm_size);
		} else {
			hs->begin.vlm_start = 0;
			hs->begin.vlm_size = 0;
		}

		mdw_drv_debug("support dev(0x%llx)mem(0x%x/0x%x/%u)\n",
			hs->begin.dev_support,
			hs->begin.mem_support,
			hs->begin.vlm_start,
			hs->begin.vlm_size);
		break;

	case APUSYS_HANDSHAKE_QUERY_DEV:
		hs->dev.num = mdw_rsc_get_dev_num(hs->dev.type);
		if (hs->dev.num <= 0)
			ret = -ENODEV;
		mdw_drv_debug("dev(%u) num(%u)\n", hs->dev.type, hs->dev.num);
		break;

	default:
		mdw_drv_debug("wrong handshake type(%d)\n",
			hs->type);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static long mdw_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct apusys_mem um;
	struct apusys_ioctl_cmd ucmd;
	struct apusys_ioctl_hs hs;
	struct mdw_usr *u;
	struct apusys_ioctl_power upwr;
	struct apusys_ioctl_ucmd uc;
	struct apusys_ioctl_sec us;

	u = (struct mdw_usr *)filp->private_data;

	switch (cmd) {
	case APUSYS_IOCTL_HANDSHAKE:
		/* handshaking, pass kernel apusys information */
		if (copy_from_user(&hs, (void *)arg,
			sizeof(struct apusys_ioctl_hs))) {
			mdw_drv_err("copy handshake struct fail\n");
			ret = -EINVAL;
			goto out;
		}

		ret = mdw_handshake(&hs);
		if (ret)
			goto out;

		if (copy_to_user((void *)arg, &hs,
			sizeof(struct apusys_ioctl_hs))) {
			mdw_drv_err("handshake with user fail\n");
			ret = -EINVAL;
		}
		break;

	case APUSYS_IOCTL_MEM_ALLOC:
		mdw_drv_warn("not support mem alloc\n");
		ret = -EINVAL;
		break;

	case APUSYS_IOCTL_MEM_IMPORT:
		if (copy_from_user(&um, (void *)arg,
			sizeof(struct apusys_mem))) {
			mdw_drv_err("copy mem struct fail\n");
			ret = -EINVAL;
			goto out;
		}

		ret = mdw_usr_mem_import(&um, u);
		if (ret)
			goto out;

		if (copy_to_user((void *)arg, &um,
			sizeof(struct apusys_mem))) {
			mdw_usr_mem_free(&um, u);
			mdw_drv_err("copy mem struct to u fail\n");
			ret = -EFAULT;
		}
		break;

	case APUSYS_IOCTL_MEM_MAP:
		if (copy_from_user(&um, (void *)arg,
			sizeof(struct apusys_mem))) {
			mdw_drv_err("copy mem struct fail\n");
			ret = -EINVAL;
			goto out;
		}

		ret = mdw_usr_mem_map(&um, u);
		if (ret)
			goto out;

		if (copy_to_user((void *)arg, &um,
			sizeof(struct apusys_mem))) {
			mdw_usr_mem_free(&um, u);
			mdw_drv_err("copy mem struct to u fail\n");
			ret = -EFAULT;
		}
		break;

	case APUSYS_IOCTL_MEM_CTL:
		if (copy_from_user(&um, (void __user *)arg, sizeof(um))) {
			ret = -EFAULT;
			break;
		}
		ret = mdw_usr_mem_ctl(&um, u);
		break;

	case APUSYS_IOCTL_MEM_FREE:
	case APUSYS_IOCTL_MEM_UNIMPORT:
	case APUSYS_IOCTL_MEM_UNMAP:
		if (copy_from_user(&um, (void *)arg,
			sizeof(struct apusys_mem))) {
			mdw_drv_err("copy mem struct fail\n");
			ret = -EINVAL;
			goto out;
		}

		ret = mdw_usr_mem_free(&um, u);
		break;

	case APUSYS_IOCTL_RUN_CMD_SYNC:
		if (copy_from_user(&ucmd, (void *)arg,
			sizeof(struct apusys_ioctl_cmd))) {
			mdw_drv_err("copy cmd struct fail\n");
			ret = -EINVAL;
			goto out;
		}

		ret = mdw_usr_run_cmd_sync(u, &ucmd);
		break;

	case APUSYS_IOCTL_RUN_CMD_ASYNC:
		if (copy_from_user(&ucmd, (void *)arg,
			sizeof(struct apusys_ioctl_cmd))) {
			mdw_drv_err("copy cmd struct fail\n");
			ret = -EINVAL;
			goto out;
		}

		ret = mdw_usr_run_cmd_async(u, &ucmd);

		if (copy_to_user((void *)arg, &ucmd,
			sizeof(struct apusys_ioctl_cmd))) {
			mdw_drv_err("copy cmd struct to u fail\n");
			ret = -EINVAL;
		}
		break;

	case APUSYS_IOCTL_WAIT_CMD:
		if (copy_from_user(&ucmd, (void *)arg,
			sizeof(struct apusys_ioctl_cmd))) {
			mdw_drv_err("copy cmd struct fail\n");
			ret = -EINVAL;
			goto out;
		}

		ret = mdw_usr_wait_cmd(u, &ucmd);
		break;

	case APUSYS_IOCTL_SET_POWER:
		ret = copy_from_user(&upwr, (void *)arg,
			sizeof(struct apusys_ioctl_power));
		if (ret) {
			mdw_drv_err("copy power struct fail\n");
			goto out;
		}

		ret = mdw_usr_set_pwr(&upwr);
		break;

	case APUSYS_IOCTL_DEVICE_ALLOC:
		ret = -EINVAL;
		break;

	case APUSYS_IOCTL_DEVICE_FREE:
		ret = -EINVAL;
		break;

	case APUSYS_IOCTL_FW_LOAD:
		ret = -EINVAL;
		mdw_drv_warn("not support fw load\n");
		break;

	case APUSYS_IOCTL_FW_UNLOAD:
		ret = -EINVAL;
		mdw_drv_warn("not suppot fw unload\n");
		break;

	case APUSYS_IOCTL_USER_CMD:
		ret = copy_from_user(&uc, (void *)arg,
			sizeof(struct apusys_ioctl_ucmd));
		if (ret) {
			mdw_drv_err("copy ucmd struct fail\n");
			goto out;
		}

		ret = mdw_usr_ucmd(&uc);
		break;

	case APUSYS_IOCTL_SEC_DEVICE_LOCK:
		ret = copy_from_user(&us, (void *)arg,
			sizeof(struct apusys_ioctl_sec));
		if (ret) {
			mdw_drv_err("copy sec struct fail\n");
			goto out;
		}

		ret = mdw_usr_dev_sec_alloc(us.dev_type, u);
		break;

	case APUSYS_IOCTL_SEC_DEVICE_UNLOCK:
		ret = copy_from_user(&us, (void *)arg,
			sizeof(struct apusys_ioctl_sec));
		if (ret) {
			mdw_drv_err("copy sec struct fail\n");
			goto out;
		}

		if (us.dev_type >= APUSYS_DEVICE_MAX || us.dev_type < 0) {
			mdw_drv_err("invalid sec type(%d)\n", us.dev_type);
			ret = -EINVAL;
			goto out;
		}

		ret = mdw_usr_dev_sec_free(us.dev_type, u);
		break;

	default:
		ret = -EINVAL;
		break;
	}

out:
	mdw_flw_debug("cmd(%u)ret(%d)\n", _IOC_NR(cmd), ret);
	return ret;
}

static long mdw_compat_ioctl(struct file *flip, unsigned int cmd,
	unsigned long arg)
{
	switch (cmd) {
	case APUSYS_IOCTL_HANDSHAKE:
	case APUSYS_IOCTL_MEM_ALLOC:
	case APUSYS_IOCTL_MEM_FREE:
	case APUSYS_IOCTL_MEM_IMPORT:
	case APUSYS_IOCTL_MEM_UNIMPORT:
	case APUSYS_IOCTL_MEM_CTL:
	case APUSYS_IOCTL_RUN_CMD_SYNC:
	case APUSYS_IOCTL_RUN_CMD_ASYNC:
	case APUSYS_IOCTL_WAIT_CMD:
	case APUSYS_IOCTL_SET_POWER:
	case APUSYS_IOCTL_DEVICE_ALLOC:
	case APUSYS_IOCTL_DEVICE_FREE:
	case APUSYS_IOCTL_FW_LOAD:
	case APUSYS_IOCTL_FW_UNLOAD:
	case APUSYS_IOCTL_USER_CMD:
	case APUSYS_IOCTL_SEC_DEVICE_LOCK:
	case APUSYS_IOCTL_SEC_DEVICE_UNLOCK:
	case APUSYS_IOCTL_MEM_MAP:
	case APUSYS_IOCTL_MEM_UNMAP:
	{
		return flip->f_op->unlocked_ioctl(flip, cmd,
					(unsigned long)compat_ptr(arg));
	}
	default:
		return -ENOIOCTLCMD;
		/*return vpu_ioctl(flip, cmd, arg);*/
	}

	return 0;
}

#ifdef CONFIG_PM
static int mdw_pm_suspend(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);

	WARN_ON(pdev == NULL);
	return mdw_suspend(pdev, PMSG_SUSPEND);
}

static int mdw_pm_resume(struct device *device)
{
	struct platform_device *pdev = to_platform_device(device);

	WARN_ON(pdev == NULL);
	return mdw_resume(pdev);
}

static int mdw_pm_restore_noirq(struct device *device)
{
	return 0;
}

static const struct dev_pm_ops mdw_pm_ops = {
	.suspend = mdw_pm_suspend,
	.resume = mdw_pm_resume,
	.freeze = mdw_pm_suspend,
	.thaw = mdw_pm_resume,
	.poweroff = mdw_pm_suspend,
	.restore = mdw_pm_resume,
	.restore_noirq = mdw_pm_restore_noirq,
};
#endif

static const struct of_device_id mdw_of_match[] = {
	{ .compatible = "mediatek,mt6877-apusys-midware" },
	{ }
};
MODULE_DEVICE_TABLE(of, mdw_of_match);

static struct platform_driver mdw_drv = {
	.probe = mdw_probe,
	.remove = mdw_remove,
	.suspend = mdw_suspend,
	.resume  = mdw_resume,
	.driver = {
		.name = APUSYS_DEV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = mdw_of_match,
		.suppress_bind_attrs = true,
#ifdef CONFIG_PM
		.pm = &mdw_pm_ops,
#endif
	},
};

static int __init mdw_init(void)
{
	int ret;

	if (!apusys_power_check())
		return -ENODEV;

	/* Hardware drivers can register while the DT mapper defers on power/IOMMU. */
	ret = mdw_rsc_init();
	if (ret)
		return ret;
	ret = mdw_usr_init();
	if (ret)
		goto exit_rsc;
	ret = mdw_dbg_init();
	if (ret)
		goto exit_usr;
	ret = mdw_tag_init();
	if (ret)
		goto exit_dbg;
	ret = platform_driver_register(&mdw_drv);
	if (!ret)
		return 0;

	mdw_tag_exit();
exit_dbg:
	mdw_dbg_exit();
exit_usr:
	mdw_usr_exit();
exit_rsc:
	mdw_rsc_exit();
	return ret;
}

static void __exit mdw_exit(void)
{
	platform_driver_unregister(&mdw_drv);
	mdw_tag_exit();
	mdw_dbg_exit();
	mdw_usr_exit();
	mdw_rsc_exit();
}

module_init(mdw_init);
module_exit(mdw_exit);
MODULE_DESCRIPTION("MTK APUSys Middleware Driver");
MODULE_AUTHOR("SPT1");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(DMA_BUF);
