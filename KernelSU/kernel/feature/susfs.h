/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _KSU_FEATURE_SUSFS_H
#define _KSU_FEATURE_SUSFS_H

void susfs_on_post_fs_data(void);
extern bool susfs_umount_for_zygote_system_process_enabled;

#endif
