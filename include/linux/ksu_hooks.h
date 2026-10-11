/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_KSU_HOOKS_H
#define _LINUX_KSU_HOOKS_H

#include <linux/types.h>

struct file;
struct filename;
struct stat;
struct stat64;

#ifdef CONFIG_KSU_MANUAL_HOOK
extern bool ksu_input_hook;
extern bool ksu_init_rc_hook;
int ksu_handle_execveat(int *fd, struct filename **filename, void *argv,
		      void *envp, int *flags);
int ksu_handle_faccessat(int *dfd, const char __user **filename, int *mode,
		       int *flags);
int ksu_handle_stat(int *dfd, const char __user **filename, int *flags);
int ksu_handle_vfs_read(struct file **file, char __user **buf, size_t *count,
		      loff_t **pos);
int ksu_handle_input_handle_event(unsigned int *type, unsigned int *code,
				 int *value);
int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd,
			  void __user **arg);
void ksu_handle_newfstat_ret(unsigned int *fd, struct stat __user **statbuf);
void ksu_handle_fstat64_ret(unsigned long *fd, struct stat64 __user **statbuf);
#endif

#ifdef CONFIG_KSU_SUSFS
long ksu_susfs_prctl(int option, unsigned long cmd, unsigned long arg,
		    unsigned long result);
#endif

#endif
