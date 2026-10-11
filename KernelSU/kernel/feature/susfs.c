/* SPDX-License-Identifier: GPL-2.0-only */
/* SUSFS 1.5.5 non-GKI interface for KernelSU Next's legacy driver. */
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/ksu_hooks.h>
#include <linux/namei.h>
#include <linux/susfs.h>
#include <linux/uaccess.h>

#include "feature/susfs.h"

#define SUSFS_PRCTL_MAGIC 0xdeadbeef

bool susfs_umount_for_zygote_system_process_enabled;

extern bool susfs_is_auto_add_sus_bind_mount_enabled;
extern bool susfs_is_auto_add_sus_ksu_default_mount_enabled;
extern bool susfs_is_auto_add_try_umount_for_bind_mount_enabled;
extern void susfs_run_try_umount_for_current_mnt_ns(void);

static bool susfs_marker_exists(const char *name)
{
	struct path path;

	if (kern_path(name, 0, &path))
		return false;
	path_put(&path);
	return true;
}

void susfs_on_post_fs_data(void)
{
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
	susfs_umount_for_zygote_system_process_enabled =
		susfs_marker_exists(DATA_ADB_UMOUNT_FOR_ZYGOTE_SYSTEM_PROCESS);
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT
	susfs_is_auto_add_sus_bind_mount_enabled =
		!susfs_marker_exists(DATA_ADB_NO_AUTO_ADD_SUS_BIND_MOUNT);
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT
	susfs_is_auto_add_sus_ksu_default_mount_enabled =
		!susfs_marker_exists(DATA_ADB_NO_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT);
#endif
#ifdef CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT
	susfs_is_auto_add_try_umount_for_bind_mount_enabled =
		!susfs_marker_exists(DATA_ADB_NO_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT);
#endif
}

/* Bit positions are part of the upstream ksu_susfs 1.5.5 userspace ABI. */
static u64 susfs_enabled_features(void)
{
	return ((u64)IS_ENABLED(CONFIG_KSU_SUSFS_SUS_PATH) << 0) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_SUS_MOUNT) << 1) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT) << 2) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT) << 3) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_SUS_KSTAT) << 4) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_SUS_OVERLAYFS) << 5) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_TRY_UMOUNT) << 6) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT) << 7) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_SPOOF_UNAME) << 8) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_ENABLE_LOG) << 9) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS) << 10) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG) << 11) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_OPEN_REDIRECT) << 12) |
		((u64)IS_ENABLED(CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT) << 14);
}

long ksu_susfs_prctl(int option, unsigned long cmd, unsigned long arg,
		    unsigned long result)
{
	void __user *data = (void __user *)arg;
	int __user *reply = (int __user *)result;
	int error = -EOPNOTSUPP;
	u64 features;

	if ((unsigned int)option != SUSFS_PRCTL_MAGIC)
		return -ENOSYS;
	if (current_uid().val != 0)
		return -EPERM;
	if (!access_ok(VERIFY_WRITE, reply, sizeof(*reply)))
		return -EFAULT;

	switch (cmd) {
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	case CMD_SUSFS_ADD_SUS_PATH:
		error = susfs_add_sus_path(data);
		break;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
	case CMD_SUSFS_ADD_SUS_MOUNT:
		error = susfs_add_sus_mount(data);
		break;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
	case CMD_SUSFS_ADD_SUS_KSTAT:
	case CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY:
		error = susfs_add_sus_kstat(data);
		break;
	case CMD_SUSFS_UPDATE_SUS_KSTAT:
		error = susfs_update_sus_kstat(data);
		break;
#endif
#ifdef CONFIG_KSU_SUSFS_TRY_UMOUNT
	case CMD_SUSFS_ADD_TRY_UMOUNT:
		error = susfs_add_try_umount(data);
		break;
	case CMD_SUSFS_RUN_UMOUNT_FOR_CURRENT_MNT_NS:
		susfs_run_try_umount_for_current_mnt_ns();
		error = 0;
		break;
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
	case CMD_SUSFS_SET_UNAME:
		error = susfs_set_uname(data);
		break;
#endif
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
	case CMD_SUSFS_ENABLE_LOG:
		if (arg > 1) {
			error = -EINVAL;
			break;
		}
		susfs_set_log(arg);
		error = 0;
		break;
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
	case CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG:
		error = susfs_set_cmdline_or_bootconfig(data);
		break;
#endif
#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
	case CMD_SUSFS_ADD_OPEN_REDIRECT:
		error = susfs_add_open_redirect(data);
		break;
#endif
	case CMD_SUSFS_SHOW_VERSION:
		error = copy_to_user(data, SUSFS_VERSION, sizeof(SUSFS_VERSION)) ? -EFAULT : 0;
		break;
	case CMD_SUSFS_SHOW_VARIANT:
		error = copy_to_user(data, SUSFS_VARIANT, sizeof(SUSFS_VARIANT)) ? -EFAULT : 0;
		break;
	case CMD_SUSFS_SHOW_ENABLED_FEATURES:
		features = susfs_enabled_features();
		error = copy_to_user(data, &features, sizeof(features)) ? -EFAULT : 0;
		break;
	default:
		break;
	}

	return put_user(error, reply) ? -EFAULT : 0;
}
