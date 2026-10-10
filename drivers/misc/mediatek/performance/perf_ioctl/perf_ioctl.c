// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */
#include "perf_ioctl.h"

#define TAG "PERF_IOCTL"

int (*fpsgo_notify_qudeq_fp)(int qudeq,
		unsigned int startend,
		int pid, unsigned long long identifier,
		unsigned long long sf_buf_id);
EXPORT_SYMBOL_GPL(fpsgo_notify_qudeq_fp);
void (*fpsgo_notify_connect_fp)(int pid,
		int connectedAPI, unsigned long long identifier);
EXPORT_SYMBOL_GPL(fpsgo_notify_connect_fp);
void (*fpsgo_notify_bqid_fp)(int pid, unsigned long long bufID,
		int queue_SF, unsigned long long identifier, int create);
EXPORT_SYMBOL_GPL(fpsgo_notify_bqid_fp);
void (*fpsgo_notify_vsync_fp)(void);
EXPORT_SYMBOL_GPL(fpsgo_notify_vsync_fp);
void (*fpsgo_notify_vsync_period_fp)(unsigned long long period);
EXPORT_SYMBOL_GPL(fpsgo_notify_vsync_period_fp);
void (*fpsgo_get_fps_fp)(int *pid, int *fps);
EXPORT_SYMBOL_GPL(fpsgo_get_fps_fp);
void (*fpsgo_get_cmd_fp)(int *cmd, int *value1, int *value2);
EXPORT_SYMBOL_GPL(fpsgo_get_cmd_fp);
void (*gbe_get_cmd_fp)(int *cmd, int *value1, int *value2);
EXPORT_SYMBOL_GPL(gbe_get_cmd_fp);
void (*rsu_getusage_fp)(__s32 *devusage, __u32 *bwusage, __u32 pid);
void (*rsu_getstate_fp)(int *throttled);
void (*perf_rsi_getindex_fp)(__s32 *data, __s32 input_size);
void (*perf_rsi_switch_collect_fp)(__s32 cmd);
int (*fpsgo_get_fstb_active_fp)(long long time_diff);
EXPORT_SYMBOL_GPL(fpsgo_get_fstb_active_fp);
int (*fpsgo_wait_fstb_active_fp)(void);
EXPORT_SYMBOL_GPL(fpsgo_wait_fstb_active_fp);
void (*fpsgo_notify_swap_buffer_fp)(int pid);
EXPORT_SYMBOL_GPL(fpsgo_notify_swap_buffer_fp);
void (*fpsgo_notify_acquire_fp)(int c_pid, int p_pid,
	int connectedAPI, unsigned long long buffer_id);
EXPORT_SYMBOL_GPL(fpsgo_notify_acquire_fp);
void (*fpsgo_get_pid_fp)(int cmd, int *pid, int value1, int value2);
EXPORT_SYMBOL_GPL(fpsgo_get_pid_fp);
void (*fpsgo_notify_nn_job_begin_fp)(unsigned int tid,
		unsigned long long mid);
EXPORT_SYMBOL_GPL(fpsgo_notify_nn_job_begin_fp);
void (*fpsgo_notify_nn_job_end_fp)(int pid, int tid,
		unsigned long long mid, int num_step, __s32 *boost,
		__s32 *device, __u64 *exec_time);
EXPORT_SYMBOL_GPL(fpsgo_notify_nn_job_end_fp);
int (*fpsgo_get_nn_priority_fp)(unsigned int pid,
		unsigned long long mid);
EXPORT_SYMBOL_GPL(fpsgo_get_nn_priority_fp);
void (*fpsgo_get_nn_ttime_fp)(unsigned int pid, unsigned long long mid,
		int num_step, __u64 *ttime);
EXPORT_SYMBOL_GPL(fpsgo_get_nn_ttime_fp);

void (*fpsgo_notify_sbe_rescue_fp)(int pid, int start, int enhance,
		int rescue_type, unsigned long long rescue_target, unsigned long long frameID);
EXPORT_SYMBOL_GPL(fpsgo_notify_sbe_rescue_fp);
int (*fpsgo_notify_sbe_policy_fp)(int pid,  char *name,
	unsigned long mask, int start, char *specific_name, int num);
EXPORT_SYMBOL_GPL(fpsgo_notify_sbe_policy_fp);
int (*fpsgo_notify_frame_hint_fp)(int qudeq,
		int pid, int frameID,
		unsigned long long id,
		int dep_mode, char *dep_name, int dep_num, long long frame_flags);
EXPORT_SYMBOL_GPL(fpsgo_notify_frame_hint_fp);
int (*fpsgo_notify_ux_buffer_count_fp)(int pid,int count, int max_buffer);
EXPORT_SYMBOL_GPL(fpsgo_notify_ux_buffer_count_fp);

int (*fpsgo_notify_smart_launch_algorithm_fp)(int feedback_time,
		int target_time, int pre_opp, int capabilty_ration);
EXPORT_SYMBOL_GPL(fpsgo_notify_smart_launch_algorithm_fp);

void (*fpsgo_notify_buffer_quota_fp)(int pid, int quota, unsigned long long identifier);
EXPORT_SYMBOL_GPL(fpsgo_notify_buffer_quota_fp);

#if IS_ENABLED(CONFIG_ARM64)
struct proc_dir_entry *perfmgr_root;
EXPORT_SYMBOL(perfmgr_root);
#else
static struct proc_dir_entry *perfmgr_root;
#endif

static unsigned long perfctl_copy_from_user(void *pvTo,
		const void __user *pvFrom, unsigned long ulBytes)
{
	if (access_ok(pvFrom, ulBytes))
		return __copy_from_user(pvTo, pvFrom, ulBytes);

	return ulBytes;
}

static unsigned long perfctl_copy_to_user(void __user *pvTo,
		const void *pvFrom, unsigned long ulBytes)
{
	if (access_ok(pvTo, ulBytes))
		return __copy_to_user(pvTo, pvFrom, ulBytes);

	return ulBytes;
}

/*--------------------XGFFRAME------------------------*/
int (*xgff_frame_startend_fp)(unsigned int startend,
		unsigned int tid,
		unsigned long long queueid,
		unsigned long long frameid,
		unsigned long long *cputime,
		unsigned int *area,
		unsigned int *pdeplistsize,
		unsigned int *pdeplist);
EXPORT_SYMBOL(xgff_frame_startend_fp);
void (*xgff_frame_getdeplist_maxsize_fp)(
		unsigned int *pdeplistsize);
EXPORT_SYMBOL(xgff_frame_getdeplist_maxsize_fp);
void (*xgff_frame_min_cap_fp)(unsigned int min_cap);
EXPORT_SYMBOL(xgff_frame_min_cap_fp);

static int xgff_show(struct seq_file *m, void *v)
{
	return 0;
}

static int xgff_open(struct inode *inode, struct file *file)
{
	return single_open(file, xgff_show, inode->i_private);
}

static long xgff_ioctl_impl(struct file *filp,
		unsigned int cmd, unsigned long arg, void *pKM)
{
	ssize_t ret = 0;
	struct _XGFFRAME_PACKAGE *msgKM = NULL,
		*msgUM = (struct _XGFFRAME_PACKAGE *)arg;
	struct _XGFFRAME_PACKAGE smsgKM;

	__u32 *vpdeplist = NULL;
	unsigned int maxsize_deplist = 0;

	msgKM = (struct _XGFFRAME_PACKAGE *)pKM;
	if (!msgKM) {
		msgKM = &smsgKM;
		if (perfctl_copy_from_user(msgKM, msgUM,
				sizeof(struct _XGFFRAME_PACKAGE))) {
			ret = -EFAULT;
			goto ret_ioctl;
		}
	}

	switch (cmd) {
	case XGFFRAME_START:
		if (!xgff_frame_startend_fp || !xgff_frame_getdeplist_maxsize_fp) {
			ret = -EAGAIN;
			goto ret_ioctl;
		}

		xgff_frame_getdeplist_maxsize_fp(&maxsize_deplist);
		vpdeplist = kcalloc(msgKM->deplist_size, sizeof(__u32), GFP_KERNEL);
		if (!vpdeplist) {
			ret = -ENOMEM;
			goto ret_ioctl;
		}

		if (perfctl_copy_from_user(vpdeplist,
			msgKM->deplist, msgKM->deplist_size * sizeof(__s32))) {
			kfree(vpdeplist);
			ret = -EFAULT;
			goto ret_ioctl;
		}

		if (msgKM->deplist_size > maxsize_deplist)
			msgKM->deplist_size = maxsize_deplist;
		ret = xgff_frame_startend_fp(1, msgKM->tid, msgKM->queueid,
			msgKM->frameid, NULL, NULL, &msgKM->deplist_size, vpdeplist);

		perfctl_copy_to_user(msgUM, msgKM, sizeof(struct _XGFFRAME_PACKAGE));

		kfree(vpdeplist);
		break;
	case XGFFRAME_END:
		if (!xgff_frame_startend_fp || !xgff_frame_getdeplist_maxsize_fp) {
			ret = -EAGAIN;
			goto ret_ioctl;
		}

		xgff_frame_getdeplist_maxsize_fp(&maxsize_deplist);
		vpdeplist = kcalloc(msgKM->deplist_size, sizeof(__u32), GFP_KERNEL);

		if (!vpdeplist) {
			ret = -ENOMEM;
			goto ret_ioctl;
		}

		if (perfctl_copy_from_user(vpdeplist,
			msgKM->deplist, msgKM->deplist_size * sizeof(__s32))) {
			kfree(vpdeplist);
			ret = -EFAULT;
			goto ret_ioctl;
		}

		if (msgKM->deplist_size > maxsize_deplist)
			msgKM->deplist_size = maxsize_deplist;

		ret = xgff_frame_startend_fp(0, msgKM->tid, msgKM->queueid,
			msgKM->frameid, &msgKM->cputime, &msgKM->area,
			&msgKM->deplist_size, vpdeplist);

		perfctl_copy_to_user(msgUM, msgKM, sizeof(struct _XGFFRAME_PACKAGE));


		kfree(vpdeplist);

		break;

	case XGFFRAME_MIN_CAP:
		if (!xgff_frame_min_cap_fp) {
			ret = -EAGAIN;
			goto ret_ioctl;
		}
		xgff_frame_min_cap_fp((unsigned int)msgKM->min_cap);
		break;

	default:
		pr_debug(TAG "%s %d: unknown cmd %x\n",
			__FILE__, __LINE__, cmd);
		ret = -1;
		goto ret_ioctl;
	}

ret_ioctl:
	return ret;
}

static long xgff_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	return xgff_ioctl_impl(filp, cmd, arg, NULL);
}

#if IS_ENABLED(CONFIG_COMPAT)
static long xgff_compat_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	int ret = -EFAULT;
	struct _XGFFRAME_PACKAGE_32 {
		__u32 tid;
		__u64 queueid;
		__u64 frameid;

		__u64 cputime;
		__u32 area;
		__u32 deplist_size;

		union {
			__u32 *deplist;
			__u64 p_dummy_deplist;
		};
	};

	struct _XGFFRAME_PACKAGE sEaraPackageKM64;
	struct _XGFFRAME_PACKAGE_32 sEaraPackageKM32;
	struct _XGFFRAME_PACKAGE_32 *psEaraPackageKM32 = &sEaraPackageKM32;
	struct _XGFFRAME_PACKAGE_32 *psEaraPackageUM32 =
		(struct _XGFFRAME_PACKAGE_32 *)arg;

	if (perfctl_copy_from_user(psEaraPackageKM32,
			psEaraPackageUM32, sizeof(struct _XGFFRAME_PACKAGE_32)))
		goto unlock_and_return;

	sEaraPackageKM64 = *((struct _XGFFRAME_PACKAGE *)psEaraPackageKM32);
	sEaraPackageKM64.deplist =
		(void *)((size_t) psEaraPackageKM32->deplist);

	ret = xgff_ioctl_impl(filp, cmd, arg, &sEaraPackageKM64);

unlock_and_return:
	return ret;
}
#endif

static const struct proc_ops xgff_Fops = {
	.proc_ioctl = xgff_ioctl,
#if IS_ENABLED(CONFIG_COMPAT)
	.proc_compat_ioctl = xgff_compat_ioctl,
#endif
	.proc_open = xgff_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/*--------------------------FPSGO_LR_IOCTL-----------------------*/
int (*fpsgo_get_lr_pair_fp)(unsigned long long sf_buffer_id,
	unsigned long long *cur_queue_ts,
	unsigned long long *l2q_ns, unsigned long long *logic_head_ts,
	unsigned int *is_logic_head_alive, unsigned long long *now_ts);
EXPORT_SYMBOL(fpsgo_get_lr_pair_fp);

void (*fpsgo_set_rl_expected_l2q_us_fp)(int vsync_multiple,
	unsigned long long user_expected_l2q_us);
EXPORT_SYMBOL(fpsgo_set_rl_expected_l2q_us_fp);

void (*fpsgo_set_rl_l2q_enable_fp)(int enable);
EXPORT_SYMBOL(fpsgo_set_rl_l2q_enable_fp);

static long fpsgo_lr_ioctl_impl(struct file *filp,
		unsigned int cmd, unsigned long arg, void *pKM)
{
	ssize_t ret = 0;
	struct _FPSGO_LR_PAIR_PACKAGE *msgKM = NULL,
		*msgUM = (struct _FPSGO_LR_PAIR_PACKAGE *)arg;
	struct _FPSGO_LR_PAIR_PACKAGE smsgKM;
	unsigned long long logical_head_ts = 0;
	unsigned long long l2q_ns_ts = 0;
	unsigned long long cur_queue_end_ts = 0;
	unsigned int is_logic_head_alive = 0;
	unsigned long long ktime_ns = 0;

	msgKM = (struct _FPSGO_LR_PAIR_PACKAGE *)pKM;
	if (!msgKM) {
		msgKM = &smsgKM;
		if (perfctl_copy_from_user(msgKM, msgUM,
				sizeof(struct _FPSGO_LR_PAIR_PACKAGE))) {
			ret = -EFAULT;
			goto ret_ioctl;
		}
	}

	switch (cmd) {
	case FPSGO_LR_PAIR:
		if (!fpsgo_get_lr_pair_fp) {
			ret = -EAGAIN;
			goto ret_ioctl;
		}

		ret = fpsgo_get_lr_pair_fp(msgKM->buffer_id, &cur_queue_end_ts,
			&l2q_ns_ts, &logical_head_ts, &is_logic_head_alive, &ktime_ns);

		if (logical_head_ts)
			msgKM->logic_head_ts = logical_head_ts;
		if (cur_queue_end_ts)
			msgKM->queue_ts = cur_queue_end_ts;
		if (l2q_ns_ts)
			msgKM->l2q_ns = l2q_ns_ts;
		if (is_logic_head_alive)
			msgKM->is_logic_head_valid = is_logic_head_alive;
		if (ktime_ns)
			msgKM->ktime_now_ns = ktime_ns;

		perfctl_copy_to_user(msgUM, msgKM, sizeof(struct _FPSGO_LR_PAIR_PACKAGE));

		break;
	case FPSGO_SF_TOUCH_ACTIVE:
		if (!fpsgo_set_rl_l2q_enable_fp) {
			ret = -EAGAIN;
			goto ret_ioctl;
		}

		fpsgo_set_rl_l2q_enable_fp(msgKM->fpsgo_l2q_enable);
		break;

	case FPSGO_SF_EXP_L2Q:
		if (!fpsgo_set_rl_expected_l2q_us_fp) {
			ret = -EAGAIN;
			goto ret_ioctl;
		}

		// default exp_vsync_multiple = 2, rl_exp_l2q_us = 33333
		// If the value isn't equal to zero, then FPSGO will use the user tuning value.
		// If the user set both param. at the same time, the priority of
		// rl_exp_l2q_us value is higher than exp_vsync_multiple value.
		fpsgo_set_rl_expected_l2q_us_fp(msgKM->exp_vsync_multiple,
			msgKM->rl_exp_l2q_us);
		break;

	default:
		pr_debug(TAG "%s %d: unknown cmd %x\n",
			__FILE__, __LINE__, cmd);
		ret = -1;
		goto ret_ioctl;
	}

ret_ioctl:
	return ret;
}

static long fpsgo_lr_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	return fpsgo_lr_ioctl_impl(filp, cmd, arg, NULL);
}

static int fpsgo_lr_show(struct seq_file *m, void *v)
{
	return 0;
}

static int fpsgo_lr_open(struct inode *inode, struct file *file)
{
	return single_open(file, fpsgo_lr_show, inode->i_private);
}


static const struct proc_ops fpsgo_lr_Fops = {
#if IS_ENABLED(CONFIG_COMPAT)
	.proc_compat_ioctl = fpsgo_lr_ioctl,
#endif
	.proc_ioctl = fpsgo_lr_ioctl,
	.proc_open = fpsgo_lr_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};


static int eara_array_count(const struct _EARA_NN_PACKAGE *msg,
		size_t *count)
{
	if (msg->num_step < 0)
		return -EINVAL;
	if (check_mul_overflow((size_t)msg->num_step,
			(size_t)MAX_DEVICE, count))
		return -EOVERFLOW;
	return 0;
}

static int eara_nn_begin(struct _EARA_NN_PACKAGE *msg)
{
	__u64 *target_time;
	size_t count;
	int ret = 0;

	if (!fpsgo_notify_nn_job_begin_fp || !fpsgo_get_nn_ttime_fp)
		return 0;

	ret = eara_array_count(msg, &count);
	if (ret)
		return ret;

	fpsgo_notify_nn_job_begin_fp(msg->tid, msg->mid);
	if (!count || !msg->target_time)
		return 0;

	target_time = kcalloc(count, sizeof(*target_time), GFP_KERNEL);
	if (!target_time)
		return -ENOMEM;

	fpsgo_get_nn_ttime_fp(msg->pid, msg->mid, msg->num_step,
			target_time);
	ret = perfctl_copy_to_user((void __user *)msg->target_time,
			target_time, count * sizeof(*target_time)) ? -EFAULT : 0;
	kfree(target_time);
	return ret;
}

static int eara_nn_end(struct _EARA_NN_PACKAGE *msg)
{
	__s32 *boost = NULL, *device = NULL;
	__u64 *exec_time = NULL;
	size_t count;
	int ret = 0;

	if (!fpsgo_notify_nn_job_end_fp || !fpsgo_get_nn_priority_fp)
		return 0;

	ret = eara_array_count(msg, &count);
	if (ret)
		goto notify_without_data;

	if (!count || !msg->boost || !msg->device || !msg->exec_time)
		goto notify_without_data;

	boost = memdup_user((const void __user *)msg->boost,
			count * sizeof(*boost));
	if (IS_ERR(boost)) {
		ret = PTR_ERR(boost);
		boost = NULL;
		goto notify_without_data;
	}
	device = memdup_user((const void __user *)msg->device,
			count * sizeof(*device));
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		device = NULL;
		goto notify_without_data;
	}
	exec_time = memdup_user((const void __user *)msg->exec_time,
			count * sizeof(*exec_time));
	if (IS_ERR(exec_time)) {
		ret = PTR_ERR(exec_time);
		exec_time = NULL;
		goto notify_without_data;
	}

	fpsgo_notify_nn_job_end_fp(msg->pid, msg->tid, msg->mid,
			msg->num_step, boost, device, exec_time);
	msg->priority = fpsgo_get_nn_priority_fp(msg->pid, msg->mid);
	kfree(exec_time);
	kfree(device);
	kfree(boost);
	return 0;

notify_without_data:
	fpsgo_notify_nn_job_end_fp(msg->pid, msg->tid, msg->mid,
			msg->num_step, NULL, NULL, NULL);
	kfree(exec_time);
	kfree(device);
	kfree(boost);
	return ret;
}

static int eara_ioctl_package(unsigned int cmd,
		struct _EARA_NN_PACKAGE *msg)
{
	int ret = 0;


	switch (cmd) {
	case EARA_GETUSAGE:
		msg->bw_usage = 0;
		if (rsu_getusage_fp)
			rsu_getusage_fp(&msg->dev_usage, &msg->bw_usage, msg->pid);
		else
			msg->dev_usage = 0;
		break;
	case EARA_GETSTATE:
		if (rsu_getstate_fp)
			rsu_getstate_fp(&msg->thrm_throttled);
		else
			msg->thrm_throttled = -1;
		break;
	case EARA_NN_BEGIN:
		ret = eara_nn_begin(msg);
		break;
	case EARA_NN_END:
		ret = eara_nn_end(msg);
		break;
	default:
		return -ENOTTY;
	}
	return ret;
}

static long ruby_eara_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct _EARA_NN_PACKAGE msg;
	void __user *user = (void __user *)arg;
	int ret;

	if (copy_from_user(&msg, user, sizeof(msg)))
		return -EFAULT;

	ret = eara_ioctl_package(cmd, &msg);
	if (ret)
		return ret;
	return copy_to_user(user, &msg, sizeof(msg)) ? -EFAULT : 0;
}

#if IS_ENABLED(CONFIG_COMPAT)
struct eara_nn_package_compat {
	__u32 pid;
	__u32 tid;
	__u64 mid;
	__s32 errorno;
	__s32 priority;
	__s32 num_step;
	__s32 dev_usage;
	__u32 bw_usage;
	__s32 thrm_throttled;
	union { compat_uptr_t device; __u64 p_dummy_device; };
	union { compat_uptr_t boost; __u64 p_dummy_boost; };
	union { compat_uptr_t exec_time; __u64 p_dummy_exec_time; };
	union { compat_uptr_t target_time; __u64 p_dummy_target_time; };
};

static long ruby_eara_compat_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct eara_nn_package_compat msg32;
	struct _EARA_NN_PACKAGE msg = {0};
	void __user *user = compat_ptr(arg);
	int ret;

	if (copy_from_user(&msg32, user, sizeof(msg32)))
		return -EFAULT;

	msg.pid = msg32.pid;
	msg.tid = msg32.tid;
	msg.mid = msg32.mid;
	msg.errorno = msg32.errorno;
	msg.priority = msg32.priority;
	msg.num_step = msg32.num_step;
	msg.dev_usage = msg32.dev_usage;
	msg.bw_usage = msg32.bw_usage;
	msg.thrm_throttled = msg32.thrm_throttled;
	msg.device = compat_ptr(msg32.device);
	msg.boost = compat_ptr(msg32.boost);
	msg.exec_time = compat_ptr(msg32.exec_time);
	msg.target_time = compat_ptr(msg32.target_time);

	ret = eara_ioctl_package(cmd, &msg);
	if (ret)
		return ret;

	msg32.errorno = msg.errorno;
	msg32.priority = msg.priority;
	msg32.dev_usage = msg.dev_usage;
	msg32.bw_usage = msg.bw_usage;
	msg32.thrm_throttled = msg.thrm_throttled;
	return copy_to_user(user, &msg32, sizeof(msg32)) ? -EFAULT : 0;
}
#endif

static const struct proc_ops ruby_eara_ops = {
	.proc_ioctl = ruby_eara_ioctl,
#if IS_ENABLED(CONFIG_COMPAT)
	.proc_compat_ioctl = ruby_eara_compat_ioctl,
#endif
};

static long ruby_earasys_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct _EARA_SYS_PACKAGE msg = {0};
	void __user *user = (void __user *)arg;

	switch (cmd) {
	case EARA_GETINDEX:
		if (!perf_rsi_getindex_fp)
			return -ENODEV;
		perf_rsi_getindex_fp(msg.data, sizeof(msg));
		return copy_to_user(user, &msg, sizeof(msg)) ? -EFAULT : 0;
	case EARA_COLLECT:
		if (copy_from_user(&msg, user, sizeof(msg)))
			return -EFAULT;
		if (!perf_rsi_switch_collect_fp)
			return -ENODEV;
		perf_rsi_switch_collect_fp(msg.cmd);
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct proc_ops ruby_earasys_ops = {
	.proc_ioctl = ruby_earasys_ioctl,
#if IS_ENABLED(CONFIG_COMPAT)
	.proc_compat_ioctl = ruby_earasys_ioctl,
#endif
};

/*--------------------INIT------------------------*/

static int device_show(struct seq_file *m, void *v)
{
	return 0;
}

static int device_open(struct inode *inode, struct file *file)
{
	return single_open(file, device_show, inode->i_private);
}

static long device_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	ssize_t ret = 0;
	int pwr_cmd = -1, value1 = -1, value2 = -1, pwr_pid = -1, pwr_fps = -1;
	struct _FPSGO_PACKAGE *msgKM = NULL, *msgUM = NULL;
	struct _FPSGO_PACKAGE smsgKM;
	struct _FPSGO_SBE_PACKAGE *msgKM_SBE = NULL, *msgUM_SBE = NULL;
	struct _FPSGO_SBE_PACKAGE smsgKM_SBE;
	struct _SMART_LAUNCH_PACKAGE *smart_launch_p;
	struct _SMART_LAUNCH_PACKAGE smart_launch;

	if (cmd == SMART_LAUNCH_ALGORITHM) {
		smart_launch_p = (struct _SMART_LAUNCH_PACKAGE *)arg;
		if (perfctl_copy_from_user(&smart_launch, smart_launch_p,
					sizeof(struct _SMART_LAUNCH_PACKAGE))) {
			ret = -EFAULT;
			goto ret_ioctl;
		}

		if (fpsgo_notify_smart_launch_algorithm_fp)
			ret = fpsgo_notify_smart_launch_algorithm_fp(smart_launch.feedback_time,
				smart_launch.target_time, smart_launch.pre_opp, smart_launch.capabilty_ration);

		smart_launch.next_opp = ret;
		perfctl_copy_to_user(smart_launch_p, &smart_launch, sizeof(struct _SMART_LAUNCH_PACKAGE));

		goto ret_ioctl;
	}

	if (cmd == FPSGO_SBE_SET_POLICY || cmd == FPSGO_HINT_FRAME) {
		msgUM_SBE = (struct _FPSGO_SBE_PACKAGE *)arg;
		msgKM_SBE = &smsgKM_SBE;
		if (perfctl_copy_from_user(msgKM_SBE, msgUM_SBE,
					sizeof(struct _FPSGO_SBE_PACKAGE))) {
			ret = -EFAULT;
			goto ret_ioctl;
		}
#if IS_ENABLED(CONFIG_MTK_FPSGO_V3)
		switch (cmd) {
		case FPSGO_SBE_SET_POLICY:
			if (fpsgo_notify_sbe_policy_fp)
				ret = fpsgo_notify_sbe_policy_fp(msgKM_SBE->pid, msgKM_SBE->name,
						msgKM_SBE->mask, msgKM_SBE->start,
						msgKM_SBE->specific_name, msgKM_SBE->num);
			break;
		case FPSGO_HINT_FRAME:
			if (fpsgo_notify_frame_hint_fp)
				msgKM_SBE->blc = fpsgo_notify_frame_hint_fp(msgKM_SBE->start,
					msgKM_SBE->rtid, msgKM_SBE->frame_id, msgKM_SBE->identifier,
					msgKM_SBE->mode, msgKM_SBE->specific_name, msgKM_SBE->num, msgKM_SBE->mask);
			perfctl_copy_to_user(msgUM_SBE, msgKM_SBE,
				sizeof(struct _FPSGO_SBE_PACKAGE));
			break;
		default:
			pr_debug(TAG "%s %d: unknown SBE cmd %x\n",
				__FILE__, __LINE__, cmd);
			ret = -1;
			break;
		}
#endif
		goto ret_ioctl;
	}

	msgUM = (struct _FPSGO_PACKAGE *)arg;
	msgKM = &smsgKM;

	if (perfctl_copy_from_user(msgKM, msgUM,
				sizeof(struct _FPSGO_PACKAGE))) {
		ret = -EFAULT;
		goto ret_ioctl;
	}

	switch (cmd) {
#if defined(CONFIG_MTK_FPSGO_V3)
	case FPSGO_QUEUE:
		if (fpsgo_notify_qudeq_fp)
			ret = fpsgo_notify_qudeq_fp(1,
					msgKM->start, msgKM->tid,
					msgKM->identifier, msgKM->sf_buf_id);
		break;
	case FPSGO_DEQUEUE:
		if (fpsgo_notify_qudeq_fp)
			fpsgo_notify_qudeq_fp(0,
					msgKM->start, msgKM->tid,
					msgKM->identifier, 0);
		break;
	case FPSGO_QUEUE_CONNECT:
		if (fpsgo_notify_connect_fp)
			fpsgo_notify_connect_fp(msgKM->tid,
					msgKM->connectedAPI, msgKM->identifier);
		break;
	case FPSGO_BQID:
		if (fpsgo_notify_bqid_fp)
			fpsgo_notify_bqid_fp(msgKM->tid, msgKM->bufID,
				msgKM->queue_SF, msgKM->identifier,
				msgKM->start);
		break;
	case FPSGO_TOUCH:
		break;
	case FPSGO_VSYNC:
		if (fpsgo_notify_vsync_fp)
			fpsgo_notify_vsync_fp();
		break;
	case FPSGO_VSYNC_PERIOD:
		if (fpsgo_notify_vsync_period_fp)
			fpsgo_notify_vsync_period_fp(msgKM->frame_time);
		break;
	case FPSGO_SWAP_BUFFER:
		if (fpsgo_notify_swap_buffer_fp)
			fpsgo_notify_swap_buffer_fp(msgKM->tid);
		break;
	case FPSGO_GET_FPS:
		if (fpsgo_get_fps_fp) {
			fpsgo_get_fps_fp(&pwr_pid, &pwr_fps);
			msgKM->tid = pwr_pid;
			msgKM->value1 = pwr_fps;
		} else
			ret = -1;
		perfctl_copy_to_user(msgUM, msgKM,
				sizeof(struct _FPSGO_PACKAGE));
		break;
	case FPSGO_GET_CMD:
		if (fpsgo_get_cmd_fp) {
			fpsgo_get_cmd_fp(&pwr_cmd, &value1, &value2);
			msgKM->cmd = pwr_cmd;
			msgKM->value1 = value1;
			msgKM->value2 = value2;
		} else
			ret = -1;
		perfctl_copy_to_user(msgUM, msgKM,
				sizeof(struct _FPSGO_PACKAGE));
		break;
	case FPSGO_GBE_GET_CMD:
		if (gbe_get_cmd_fp) {
			gbe_get_cmd_fp(&pwr_cmd, &value1, &value2);
			msgKM->cmd = pwr_cmd;
			msgKM->value1 = value1;
			msgKM->value2 = value2;
		} else
			ret = -ENODEV;
		perfctl_copy_to_user(msgUM, msgKM,
				sizeof(struct _FPSGO_PACKAGE));
		break;
	case FPSGO_GET_FSTB_ACTIVE:
		if (fpsgo_get_fstb_active_fp)
			msgKM->active = fpsgo_get_fstb_active_fp(msgKM->time_diff);
		else
			ret = 0;
		perfctl_copy_to_user(msgUM, msgKM,
				sizeof(struct _FPSGO_PACKAGE));
		break;
	case FPSGO_WAIT_FSTB_ACTIVE:
		if (fpsgo_wait_fstb_active_fp)
			fpsgo_wait_fstb_active_fp();
		break;
	case FPSGO_SBE_RESCUE:
		if (fpsgo_notify_sbe_rescue_fp)
			fpsgo_notify_sbe_rescue_fp(msgKM->tid, msgKM->start, msgKM->value2,
						msgKM->identifier, msgKM->frame_time, msgKM->frame_id);
		break;
	case FPSGO_SBE_BUFFER_COUNT:
		if (fpsgo_notify_ux_buffer_count_fp)
			fpsgo_notify_ux_buffer_count_fp(msgKM->tid, msgKM->value1, msgKM->value2);
		break;
	case FPSGO_ACQUIRE:
		if (fpsgo_notify_acquire_fp)
			fpsgo_notify_acquire_fp(msgKM->pid1, msgKM->pid2,
				msgKM->connectedAPI, msgKM->bufID);
		break;
	case FPSGO_BUFFER_QUOTA:
		if (fpsgo_notify_buffer_quota_fp)
			fpsgo_notify_buffer_quota_fp(msgKM->tid, msgKM->value1,
						msgKM->identifier);
		break;
	case FPSGO_GET_CAM_APK_PID:
	case FPSGO_GET_CAM_SERVER_PID:
		if (fpsgo_get_pid_fp)
			fpsgo_get_pid_fp(msgKM->value1, &msgKM->pid1,
				msgKM->pid1, msgKM->value2);
		perfctl_copy_to_user(msgUM, msgKM,
			sizeof(struct _FPSGO_PACKAGE));
		break;
#else
	case FPSGO_TOUCH:
		 [[fallthrough]];
	case FPSGO_QUEUE:
		 [[fallthrough]];
	case FPSGO_DEQUEUE:
		 [[fallthrough]];
	case FPSGO_QUEUE_CONNECT:
		 [[fallthrough]];
	case FPSGO_VSYNC:
		 [[fallthrough]];
	case FPSGO_VSYNC_PERIOD:
		 [[fallthrough]];
	case FPSGO_BQID:
		 [[fallthrough]];
	case FPSGO_SWAP_BUFFER:
		 [[fallthrough]];
	case FPSGO_GET_FPS:
		 [[fallthrough]];
	case FPSGO_GET_CMD:
		 [[fallthrough]];
	case FPSGO_GBE_GET_CMD:
		[[fallthrough]];
	case FPSGO_GET_FSTB_ACTIVE:
		[[fallthrough]];
	case FPSGO_WAIT_FSTB_ACTIVE:
		[[fallthrough]];
	case FPSGO_BUFFER_QUOTA:
		[[fallthrough]];
	case FPSGO_SBE_RESCUE:
		[[fallthrough]];
	case FPSGO_SBE_BUFFER_COUNT:
		[[fallthrough]];
	case FPSGO_ACQUIRE:
		break;
	case FPSGO_GET_CAM_APK_PID:
		break;
	case FPSGO_GET_CAM_SERVER_PID:
		break;
#endif

	default:
		pr_debug(TAG "%s %d: unknown cmd %x\n",
			__FILE__, __LINE__, cmd);
		ret = -1;
		goto ret_ioctl;
	}

ret_ioctl:
	return ret;
}

static const struct proc_ops Fops = {
#if IS_ENABLED(CONFIG_COMPAT)
	.proc_compat_ioctl = device_ioctl,
#endif
	.proc_ioctl = device_ioctl,
	.proc_open = device_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/*--------------------INIT------------------------*/
int init_perfctl(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *pe;
	int ret_val = 0;


	pr_debug(TAG"Start to init perf_ioctl driver\n");

	perfmgr_root = parent;

	pe = proc_create("perf_ioctl", 0664, parent, &Fops);
	if (!pe) {
		pr_debug(TAG"%s failed with %d\n",
				"Creating file node ",
				ret_val);
		ret_val = -ENOMEM;
		goto out_wq;
	}

	if (!proc_create("eara_ioctl", 0664, parent, &ruby_eara_ops) ||
	    !proc_create("eara_sys_ioctl", 0664, parent, &ruby_earasys_ops))
		return -ENOMEM;

	pe = proc_create("xgff_ioctl", 0664, parent, &xgff_Fops);
	if (!pe) {
		pr_debug(TAG"%s failed with %d\n",
				"Creating file node ",
				ret_val);
		ret_val = -ENOMEM;
		goto out_wq;
	}

	pe = proc_create("fpsgo_lr_ioctl", 0664, parent, &fpsgo_lr_Fops);
	if (!pe) {
		pr_debug(TAG"%s failed with %d\n",
				"Creating file node ",
				ret_val);
		ret_val = -ENOMEM;
		goto out_wq;
	}

	pr_debug(TAG"init perf_ioctl driver done\n");

	return 0;

out_wq:
	return ret_val;
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek FPSGO perf_ioctl");
MODULE_AUTHOR("MediaTek Inc.");
