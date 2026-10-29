/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 - 2021 MediaTek Inc.
 */


/*******************************************************************************
* Dependency
*******************************************************************************/
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/printk.h>
#include <linux/version.h>
#include <linux/wait.h>
#include <conn_power_throttling.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "["KBUILD_MODNAME"]" fmt

/******************************************************************************
 * Definition
******************************************************************************/
/* device name and major number */
#define GPS_PWR_DEVNAME            "gps_pwr"
/*******************************************************************************
* structure & enumeration
*******************************************************************************/
/*---------------------------------------------------------------------------*/
struct gps_pwr_dev {
	struct class *cls;
	struct device *dev;
	dev_t devno;
	struct cdev chdev;
};

/******************************************************************************
 * local variables
******************************************************************************/
/* static int flag; */
bool has_new_battery_level;
struct gps_pwr_dev *gps_pwr_devobj;
static wait_queue_head_t GPS_PWR_wq;
static DEFINE_SPINLOCK(gps_pwr_lock);
static unsigned int battery_sequence;
#define DRV_TYPE_GPS    2
enum conn_pwr_low_battery_level battery_level;
bool gps_pwr_open_state;

int gps_pwr_msg_cb(enum conn_pwr_event_type event_type, void *level)
{
	int ret = 0;
	unsigned long flags;

	if (event_type == CONN_PWR_EVENT_LEVEL) {
		pr_info("gps_pwr_msg_cb been called, level=%d, gps_pwr_open_state=%d\n",
			*(enum conn_pwr_low_battery_level *)level, gps_pwr_open_state);
		spin_lock_irqsave(&gps_pwr_lock, flags);
		battery_level = *(enum conn_pwr_low_battery_level *)level;
		battery_sequence++;
		has_new_battery_level = true;
		spin_unlock_irqrestore(&gps_pwr_lock, flags);
		wake_up_interruptible(&GPS_PWR_wq);
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
long gps_pwr_unlocked_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	long retval = 0;

	return retval;
}

long gps_pwr_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return gps_pwr_unlocked_ioctl(filp, cmd, arg);
}

/******************************************************************************/
/*****************************************************************************/
static int gps_pwr_open(struct inode *inode, struct file *file)
{
	int ret;
	enum conn_pwr_low_battery_level level = CONN_PWR_THR_LV_0;

	/* conn_pwr_init() clears callbacks, so register after the loader ran. */
	ret = conn_pwr_register_event_cb(DRV_TYPE_GPS, gps_pwr_msg_cb);
	if (ret)
		return ret;
	ret = conn_pwr_drv_pre_on(DRV_TYPE_GPS, &level);
	if (ret) {
		conn_pwr_register_event_cb(DRV_TYPE_GPS, NULL);
		conn_pwr_drv_post_off(DRV_TYPE_GPS);
		return ret;
	}
	gps_pwr_open_state = true;
	gps_pwr_msg_cb(CONN_PWR_EVENT_LEVEL, &level);
	pr_info("conn_pwr_drv_pre_on ret = %d, battery_level = %d\n", ret, battery_level);
	return 0;
}

/*****************************************************************************/


/*****************************************************************************/
static int gps_pwr_close(struct inode *inode, struct file *file)
{
	int ret;

	gps_pwr_open_state = false;
	conn_pwr_register_event_cb(DRV_TYPE_GPS, NULL);
	ret = conn_pwr_drv_post_off(DRV_TYPE_GPS);
	pr_info("conn_pwr_drv_post_off ret = %d\n", ret);
	return 0;
}

static ssize_t gps_pwr_read(struct file *file, char __user *buf, size_t count,
			    loff_t *ppos)
{
	enum conn_pwr_low_battery_level level;
	unsigned int sequence;
	unsigned long flags;

	if (!count)
		return 0;
	if (count < sizeof(battery_level))
		return -EINVAL;
	spin_lock_irqsave(&gps_pwr_lock, flags);
	level = battery_level;
	sequence = battery_sequence;
	spin_unlock_irqrestore(&gps_pwr_lock, flags);
	if (copy_to_user(buf, &level, sizeof(level)))
		return -EFAULT;

	spin_lock_irqsave(&gps_pwr_lock, flags);
	if (sequence == battery_sequence)
		has_new_battery_level = false;
	spin_unlock_irqrestore(&gps_pwr_lock, flags);
	return sizeof(level);
}
/******************************************************************************/
static ssize_t gps_pwr_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	ssize_t retval = 0;

	return retval;
}
/*****************************************************************************/
static unsigned int gps_pwr_poll(struct file *file, poll_table *wait)
{
	unsigned int mask;

	mask = 0;

	poll_wait(file, &GPS_PWR_wq, wait);
	if (READ_ONCE(has_new_battery_level))
		mask = (POLLIN | POLLRDNORM);
	pr_info("gps_pwr_poll been return , mask = %d\n", mask);

	return mask;
}
/* Kernel interface */
static const struct file_operations gps_pwr_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gps_pwr_unlocked_ioctl,
	.compat_ioctl = gps_pwr_compat_ioctl,
	.open = gps_pwr_open,
	.read = gps_pwr_read,
	.write = gps_pwr_write,
	.release = gps_pwr_close,
	.poll = gps_pwr_poll,
};

static int gps_pwr_init(void)
{
	int ret;

	gps_pwr_devobj = kzalloc(sizeof(*gps_pwr_devobj), GFP_KERNEL);
	if (!gps_pwr_devobj)
		return -ENOMEM;

	init_waitqueue_head(&GPS_PWR_wq);
	has_new_battery_level = false;
	gps_pwr_open_state = false;

	ret = alloc_chrdev_region(&gps_pwr_devobj->devno, 0, 1,
				  GPS_PWR_DEVNAME);
	if (ret)
		goto free_device;

	cdev_init(&gps_pwr_devobj->chdev, &gps_pwr_fops);
	gps_pwr_devobj->chdev.owner = THIS_MODULE;
	ret = cdev_add(&gps_pwr_devobj->chdev, gps_pwr_devobj->devno, 1);
	if (ret)
		goto unregister_region;

	gps_pwr_devobj->cls = class_create(GPS_PWR_DEVNAME);
	if (IS_ERR(gps_pwr_devobj->cls)) {
		ret = PTR_ERR(gps_pwr_devobj->cls);
		goto delete_cdev;
	}

	gps_pwr_devobj->dev = device_create(gps_pwr_devobj->cls, NULL,
			gps_pwr_devobj->devno, gps_pwr_devobj, GPS_PWR_DEVNAME);
	if (IS_ERR(gps_pwr_devobj->dev)) {
		ret = PTR_ERR(gps_pwr_devobj->dev);
		goto destroy_class;
	}

	pr_info("GPS_PWR device init done\n");
	return 0;

destroy_class:
	class_destroy(gps_pwr_devobj->cls);
delete_cdev:
	cdev_del(&gps_pwr_devobj->chdev);
unregister_region:
	unregister_chrdev_region(gps_pwr_devobj->devno, 1);
free_device:
	kfree(gps_pwr_devobj);
	gps_pwr_devobj = NULL;
	return ret;
}

/*****************************************************************************/
static void gps_pwr_exit(void)
{
	if (!gps_pwr_devobj) {
		pr_info("null pointer: %p\n", gps_pwr_devobj);
		return;
	}

	pr_info("Unregistering gps_pwr chardev\n");
	conn_pwr_register_event_cb(DRV_TYPE_GPS, NULL);
	cdev_del(&gps_pwr_devobj->chdev);
	unregister_chrdev_region(gps_pwr_devobj->devno, 1);
	device_destroy(gps_pwr_devobj->cls, gps_pwr_devobj->devno);
	class_destroy(gps_pwr_devobj->cls);
	kfree(gps_pwr_devobj);
	gps_pwr_devobj = NULL;
	pr_info("Done\n");
}

int mtk_gps_pwr_init(void)
{
	pr_info("gps_pwr init begin");
	return gps_pwr_init();
}

void mtk_gps_pwr_exit(void)
{
	pr_info("gps_pwr exit begin");
	return gps_pwr_exit();
}
/*****************************************************************************/
module_init(mtk_gps_pwr_init);
module_exit(mtk_gps_pwr_exit);
/*****************************************************************************/
MODULE_AUTHOR("Tianfang li <Tianfang.Li@mediatek.com>");
MODULE_DESCRIPTION("GPS_PWR dev");
MODULE_LICENSE("GPL");


