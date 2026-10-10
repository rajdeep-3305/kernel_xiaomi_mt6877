// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek Turbo Write compatibility for the 6.6 UFS core.
 *
 * Ruby's 4.19 UFS feature driver called standard WriteBooster "Turbo Write"
 * and added three pieces around it: per-LU provisioning, lifetime policing,
 * and a suspend check that requests a buffer flush during Hibern8.  The
 * protocol transactions remain the standard UFS WriteBooster transactions;
 * the 6.6 core continues to own its generic WriteBooster state machine.
 */

#include <asm/unaligned.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kstrtox.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>

#include <ufs/ufshcd.h>
#include <ufs/ufs_quirks.h>
#include <ufs/ufstw.h>
#include "ufshcd-priv.h"

#define UFSTW_SHARED_LUN			2

struct ufstw_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct ufstw_lu *tw, char *buf);
	ssize_t (*store)(struct ufstw_lu *tw, const char *buf, size_t count);
};

static int ufstw_create_sysfs(struct ufs_hba *hba, struct ufstw_lu *tw);

static inline u8 ufstw_query_index(const struct ufstw_info *info, u8 lun)
{
	return info->dev.buffer_type == WB_BUF_MODE_SHARED ? 0 : lun;
}

static inline bool ufstw_is_write(const struct scsi_cmnd *cmd)
{
	return cmd && (cmd->cmnd[0] == WRITE_10 || cmd->cmnd[0] == WRITE_16);
}

static int ufstw_pm_get(struct ufs_hba *hba)
{
	struct device *dev;
	int ret;

	/* __ufshcd_wl_suspend() already holds the controller active. */
	if (READ_ONCE(hba->pm_op_in_progress))
		return 0;

	if (!hba->ufs_device_wlun)
		return -ENODEV;

	dev = &hba->ufs_device_wlun->sdev_gendev;
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		pm_runtime_put_noidle(dev);
		return ret;
	}

	return 1;
}

static void ufstw_pm_put(struct ufs_hba *hba, int pm_ref)
{
	if (pm_ref > 0)
		pm_runtime_put_sync(&hba->ufs_device_wlun->sdev_gendev);
}

static int ufstw_query_flag(struct ufs_hba *hba, u8 index, enum query_opcode op,
				enum flag_idn idn, bool *value)
{
	int ret, pm_ref;

	pm_ref = ufstw_pm_get(hba);
	if (pm_ref < 0) {
		return pm_ref;
	}

	ret = ufshcd_query_flag_retry(hba, op, idn, index, value);
	ufstw_pm_put(hba, pm_ref);
	return ret;
}

static int ufstw_set_flag(struct ufstw_lu *tw, enum flag_idn idn, bool *value)
{
	int ret;
	u8 index = ufstw_query_index(&tw->hba->ufstw, tw->lun);

	ret = ufstw_query_flag(tw->hba, index, UPIU_QUERY_OPCODE_SET_FLAG,
				       idn, NULL);
	if (!ret) {
		if (value)
			*value = true;
		if (idn == QUERY_FLAG_IDN_WB_EN)
			tw->hba->dev_info.wb_enabled = true;
		else if (idn == QUERY_FLAG_IDN_WB_BUFF_FLUSH_EN)
			tw->hba->dev_info.wb_buf_flush_enabled = true;
	}
	return ret;
}

static int ufstw_clear_flag(struct ufstw_lu *tw, enum flag_idn idn,
				    bool *value)
{
	int ret;
	u8 index = ufstw_query_index(&tw->hba->ufstw, tw->lun);

	ret = ufstw_query_flag(tw->hba, index, UPIU_QUERY_OPCODE_CLEAR_FLAG,
				       idn, NULL);
	if (!ret) {
		if (value)
			*value = false;
		if (idn == QUERY_FLAG_IDN_WB_EN)
			tw->hba->dev_info.wb_enabled = false;
		else if (idn == QUERY_FLAG_IDN_WB_BUFF_FLUSH_EN)
			tw->hba->dev_info.wb_buf_flush_enabled = false;
	}
	return ret;
}

static int ufstw_read_flag(struct ufstw_lu *tw, enum flag_idn idn,
				   bool *value)
{
	u8 index = ufstw_query_index(&tw->hba->ufstw, tw->lun);

	return ufstw_query_flag(tw->hba, index, UPIU_QUERY_OPCODE_READ_FLAG,
				idn, value);
}

static int ufstw_read_attr(struct ufstw_lu *tw, enum attr_idn idn, u32 *value)
{
	int ret, pm_ref;
	u8 index = ufstw_query_index(&tw->hba->ufstw, tw->lun);

	pm_ref = ufstw_pm_get(tw->hba);
	if (pm_ref < 0) {
		return pm_ref;
	}

	ret = ufshcd_query_attr_retry(tw->hba, UPIU_QUERY_OPCODE_READ_ATTR,
				       idn, index, 0, value);
	ufstw_pm_put(tw->hba, pm_ref);
	return ret;
}

static int ufstw_update_ee(struct ufs_hba *hba, bool enable)
{
	int ret, pm_ref;

	pm_ref = ufstw_pm_get(hba);
	if (pm_ref < 0) {
		return pm_ref;
	}

	ret = ufshcd_update_ee_drv_mask(hba,
			enable ? MASK_EE_WRITEBOOSTER_EVENT : 0,
			enable ? 0 : MASK_EE_WRITEBOOSTER_EVENT);
	ufstw_pm_put(hba, pm_ref);
	return ret;
}

static bool ufstw_lifetime_valid(u32 value)
{
#ifdef CONFIG_SCSI_UFS_TW_IGNORE_GUARANTEE_BIT
	value &= ~UFSTW_LIFETIME_NOT_GUARANTEED;
#else
	if (value & UFSTW_LIFETIME_NOT_GUARANTEED)
		return false;
#endif

	return value < UFSTW_MAX_LIFETIME_VALUE;
}

static void ufstw_disable_locked(struct ufstw_lu *tw)
{
	if (tw->tw_enabled)
		ufstw_clear_flag(tw, QUERY_FLAG_IDN_WB_EN, &tw->tw_enabled);
	if (tw->flush_during_hibern8)
		ufstw_clear_flag(tw,
			QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
			&tw->flush_during_hibern8);
	tw->mode = UFSTW_MODE_DISABLED;
	tw->disabled = true;
}

static int ufstw_refresh_buffer_locked(struct ufstw_lu *tw)
{
	int ret;

	ret = ufstw_read_attr(tw, QUERY_ATTR_IDN_AVAIL_WB_BUFF_SIZE,
				      &tw->available_buffer);
	if (ret)
		return ret;

	ret = ufstw_read_attr(tw, QUERY_ATTR_IDN_CURR_WB_BUFF_SIZE,
				      &tw->current_buffer);
	if (ret)
		return ret;

	return ufstw_read_attr(tw, QUERY_ATTR_IDN_WB_FLUSH_STATUS,
				       &tw->flush_status);
}

static void ufstw_lifetime_work_fn(struct work_struct *work)
{
	struct ufstw_lu *tw = container_of(work, struct ufstw_lu,
					   lifetime_work);
	u32 lifetime;

	mutex_lock(&tw->lock);
	if (!tw->initialized || tw->disabled ||
	    atomic_read(&tw->hba->ufstw.state) != UFSTW_PRESENT)
		goto out;

	if (ufstw_read_attr(tw, QUERY_ATTR_IDN_WB_BUFF_LIFE_TIME_EST,
				   &lifetime))
		goto out;

	tw->lifetime_estimate = lifetime;
	if (!ufstw_lifetime_valid(lifetime))
		ufstw_disable_locked(tw);
out:
	mutex_unlock(&tw->lock);
}

static void ufstw_flush_work_fn(struct work_struct *work)
{
	struct ufstw_lu *tw = container_of(to_delayed_work(work),
					   struct ufstw_lu, flush_work);
	bool reschedule = false;

	mutex_lock(&tw->lock);
	if (!tw->initialized || tw->disabled ||
	    atomic_read(&tw->hba->ufstw.state) != UFSTW_PRESENT)
		goto out;

	if (ufstw_refresh_buffer_locked(tw)) {
		reschedule = true;
		goto out_schedule;
	}

	if (tw->flush_during_hibern8 &&
	    tw->available_buffer >= tw->flush_th_max) {
		ufstw_clear_flag(tw,
			QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
			&tw->flush_during_hibern8);
		tw->next_flush = 0;
		goto out;
	}

	if (tw->available_buffer < tw->flush_th_max) {
		if (tw->flush_during_hibern8)
			reschedule = true;
		else if (tw->available_buffer < tw->flush_th_min) {
			if (!ufstw_set_flag(tw,
				QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
				&tw->flush_during_hibern8))
				reschedule = true;
		}
	}

out_schedule:
	if (reschedule) {
		tw->next_flush = jiffies +
			msecs_to_jiffies(UFSTW_FLUSH_CHECK_PERIOD_MS);
		mod_delayed_work(system_wq, &tw->flush_work,
			msecs_to_jiffies(UFSTW_FLUSH_CHECK_PERIOD_MS));
	} else
		tw->next_flush = 0;
out:
	mutex_unlock(&tw->lock);
}

static void ufstw_flush_h8_work_fn(struct work_struct *work)
{
	struct ufstw_lu *tw = container_of(to_delayed_work(work),
					   struct ufstw_lu, flush_h8_work);
	struct ufs_hba *hba = tw->hba;
	bool reschedule = false;
	int pm_ref;

	/* Let the device complete the Hibern8-triggered buffer flush first. */
	pm_ref = ufstw_pm_get(hba);
	if (pm_ref < 0)
		return;
	msleep(1000);

	mutex_lock(&tw->lock);
	if (!tw->initialized || tw->disabled ||
	    atomic_read(&tw->hba->ufstw.state) != UFSTW_PRESENT)
		goto out;

	if (ufstw_refresh_buffer_locked(tw)) {
		reschedule = true;
		goto out_schedule;
	}

	if (tw->available_buffer >= tw->flush_th_max) {
		if (tw->flush_during_hibern8)
			ufstw_clear_flag(tw,
				QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
				&tw->flush_during_hibern8);
		tw->next_flush = 0;
		goto out;
	}

	reschedule = true;
out_schedule:
	if (reschedule) {
		tw->next_flush = jiffies +
			msecs_to_jiffies(UFSTW_FLUSH_CHECK_PERIOD_MS);
		mod_delayed_work(system_wq, &tw->flush_h8_work,
			msecs_to_jiffies(UFSTW_FLUSH_CHECK_PERIOD_MS));
	}
out:
	mutex_unlock(&tw->lock);
	ufstw_pm_put(hba, pm_ref);
}

void ufstw_get_dev_info(struct ufs_hba *hba, const u8 *desc_buf)
{
	struct ufstw_dev_info *dev = &hba->ufstw.dev;
	bool geo_valid = dev->geo_valid;
	u8 number_lu = dev->number_lu;
	u32 extended_features;

	memset(dev, 0, sizeof(*dev));
	dev->geo_valid = geo_valid;
	dev->number_lu = number_lu;
	extended_features = get_unaligned_be32(desc_buf +
						       DEVICE_DESC_PARAM_EXT_UFS_FEATURE_SUP);
	if (!(extended_features & UFS_DEV_WRITE_BOOSTER_SUP) ||
	    (dev->geo_valid && !dev->number_lu))
		return;

	dev->supported = true;
	dev->no_user_space_reduction =
		desc_buf[DEVICE_DESC_PARAM_WB_PRESRV_USRSPC_EN];
	dev->buffer_type = desc_buf[DEVICE_DESC_PARAM_WB_TYPE];
	if (dev->buffer_type != WB_BUF_MODE_LU_DEDICATED &&
	    dev->buffer_type != WB_BUF_MODE_SHARED)
		dev->supported = false;
}

void ufstw_get_geo_info(struct ufs_hba *hba, const u8 *geo_buf)
{
	struct ufstw_dev_info *dev = &hba->ufstw.dev;

	dev->geo_valid = true;
	dev->number_lu = geo_buf[GEOMETRY_DESC_PARAM_WB_MAX_WB_LUNS];
	if (!dev->number_lu)
		dev->supported = false;
}

static int ufstw_read_unit_alloc(struct ufs_hba *hba, u8 lun, u32 *alloc)
{
	u8 buf[sizeof(*alloc)] = {};
	int ret, pm_ref;

	pm_ref = ufstw_pm_get(hba);
	if (pm_ref < 0) {
		return pm_ref;
	}

	ret = ufshcd_read_desc_param(hba, QUERY_DESC_IDN_UNIT, lun,
				     UNIT_DESC_PARAM_WB_BUF_ALLOC_UNITS,
				     buf, sizeof(buf));
	ufstw_pm_put(hba, pm_ref);
	if (ret)
		return ret;

	*alloc = get_unaligned_be32(buf);
	return 0;
}

void ufstw_init(struct ufs_hba *hba)
{
	struct ufstw_info *info = &hba->ufstw;

	if (!info->dev.supported || !ufshcd_is_wb_allowed(hba)) {
		atomic_set(&info->state, UFSTW_NOT_SUPPORTED);
		return;
	}

	/* Ruby left the WriteBooster exception event under explicit control. */
	if (!ufstw_update_ee(hba, false)) {
		unsigned int lun;

		for (lun = 0; lun < UFS_UPIU_MAX_UNIT_NUM_ID; lun++)
			if (info->lu[lun])
				info->lu[lun]->ee_mode = UFSTW_EE_DISABLED;
	}
	atomic_set(&info->state, UFSTW_PRESENT);
}

void ufstw_init_lu(struct ufs_hba *hba, struct scsi_device *sdev)
{
	struct ufstw_info *info = &hba->ufstw;
	struct ufstw_lu *tw;
	u32 alloc = 0;
	bool enabled = false;
	int ret;
	u8 lun = sdev->lun;

	/* Ruby builds UFS_MTK_TW_AWAYS_ON: only the shared LU2 is active. */
	if (!info->dev.supported || lun >= UFS_UPIU_MAX_UNIT_NUM_ID ||
	    lun != UFSTW_SHARED_LUN ||
	    atomic_read(&info->state) != UFSTW_PRESENT || info->lu[lun])
		return;

	if (info->dev.buffer_type == WB_BUF_MODE_SHARED) {
		if (lun != UFSTW_SHARED_LUN)
			return;
	} else {
		ret = ufstw_read_unit_alloc(hba, lun, &alloc);
		if (ret || !alloc)
			return;
	}

	tw = kzalloc(sizeof(*tw), GFP_KERNEL);
	if (!tw)
		return;

	tw->hba = hba;
	tw->lun = lun;
	tw->buffer_alloc_units = alloc;
	tw->mode = UFSTW_MODE_MANUAL;
	tw->ee_mode = UFSTW_EE_DISABLED;
	tw->flush_th_min = UFSTW_FLUSH_WORKER_TH_MIN;
	tw->flush_th_max = UFSTW_FLUSH_WORKER_TH_MAX;
	mutex_init(&tw->lock);
	mutex_init(&tw->sysfs_lock);
	spin_lock_init(&tw->lifetime_lock);
	atomic_set(&tw->active_cnt, 0);
	INIT_WORK(&tw->lifetime_work, ufstw_lifetime_work_fn);
	INIT_DELAYED_WORK(&tw->flush_work, ufstw_flush_work_fn);
	INIT_DELAYED_WORK(&tw->flush_h8_work, ufstw_flush_h8_work_fn);
	info->lu[lun] = tw;
	tw->initialized = true;
	/* Sysfs is observability; it must not disable the storage feature. */
	if (ufstw_create_sysfs(hba, tw))
		dev_warn(hba->dev, "failed to create Turbo Write sysfs for LU%u\n",
			 lun);

	ret = ufstw_read_flag(tw, QUERY_FLAG_IDN_WB_EN, &enabled);
	if (!ret)
		tw->tw_enabled = enabled;
	ret = ufstw_read_flag(tw, QUERY_FLAG_IDN_WB_BUFF_FLUSH_EN,
				      &tw->flush_enabled);
	if (ret)
		tw->flush_enabled = false;
	ret = ufstw_read_flag(tw, QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
				      &tw->flush_during_hibern8);
	if (ret)
		tw->flush_during_hibern8 = false;

	if (ufstw_read_attr(tw, QUERY_ATTR_IDN_WB_BUFF_LIFE_TIME_EST,
				   &tw->lifetime_estimate) ||
	    !ufstw_lifetime_valid(tw->lifetime_estimate)) {
		mutex_lock(&tw->lock);
		ufstw_disable_locked(tw);
		mutex_unlock(&tw->lock);
		return;
	}
	ufstw_refresh_buffer_locked(tw);

	/* Ruby's MTK path keeps Turbo Write and Hibern8 flushing on for LU2. */
	if (tw->lun != 0) {
		if (!tw->tw_enabled)
			ufstw_set_flag(tw, QUERY_FLAG_IDN_WB_EN, &tw->tw_enabled);
		if (!tw->flush_during_hibern8)
			ufstw_set_flag(tw,
				QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
				&tw->flush_during_hibern8);
	}

	if (tw->available_buffer < tw->flush_th_max)
		mod_delayed_work(system_wq, &tw->flush_work, 0);
}

void ufstw_destroy_lu(struct ufs_hba *hba, struct scsi_device *sdev)
{
	struct ufstw_lu *tw;
	u8 lun = sdev->lun;

	if (lun >= UFS_UPIU_MAX_UNIT_NUM_ID)
		return;

	tw = hba->ufstw.lu[lun];
	if (!tw)
		return;

	hba->ufstw.lu[lun] = NULL;
	tw->initialized = false;
	cancel_work_sync(&tw->lifetime_work);
	cancel_delayed_work_sync(&tw->flush_work);
	cancel_delayed_work_sync(&tw->flush_h8_work);
	if (tw->kobj_added) {
		kobject_del(&tw->kobj);
		kobject_put(&tw->kobj);
	} else {
		kfree(tw);
	}
}

void ufstw_prep(struct ufs_hba *hba, struct ufshcd_lrb *lrbp)
{
	struct scsi_cmnd *cmd = lrbp->cmd;
	struct ufstw_lu *tw;
	unsigned int sectors;
	bool check_lifetime = false;

	if (!cmd || !ufstw_is_write(cmd) || lrbp->lun >= UFS_UPIU_MAX_UNIT_NUM_ID)
		return;

	tw = hba->ufstw.lu[lrbp->lun];
	if (!tw)
		return;

	sectors = blk_rq_sectors(scsi_cmd_to_rq(cmd));
	spin_lock_bh(&tw->lifetime_lock);
	if (READ_ONCE(tw->initialized) && !READ_ONCE(tw->disabled) &&
	    READ_ONCE(tw->tw_enabled)) {
		tw->written_sectors += sectors;
		if (tw->written_sectors >= UFSTW_LIFETIME_SECT) {
			tw->written_sectors = 0;
			check_lifetime = true;
		}
	}
	spin_unlock_bh(&tw->lifetime_lock);

	if (check_lifetime)
		schedule_work(&tw->lifetime_work);
}

bool ufstw_need_flush(struct ufs_hba *hba)
{
	struct ufstw_info *info = &hba->ufstw;
	struct ufstw_lu *tw;
	unsigned int lun;
	bool need_flush = false;

	if (atomic_read(&info->state) != UFSTW_PRESENT)
		return false;

	for (lun = 0; lun < UFS_UPIU_MAX_UNIT_NUM_ID; lun++) {
		tw = info->lu[lun];
		if (!tw)
			continue;

		mutex_lock(&tw->lock);
		if (!tw->initialized || tw->disabled || !tw->tw_enabled)
			goto next;
		if (work_busy(&tw->flush_h8_work.work)) {
			need_flush = true;
			goto next;
		}
		if (ufstw_refresh_buffer_locked(tw))
			goto next;
		if (tw->available_buffer >= tw->flush_th_max)
			goto next;

		if (hba->dev_quirks & UFS_DEVICE_QUIRK_WRITE_BOOSETER_FLUSH) {
			if (ufstw_clear_flag(tw, QUERY_FLAG_IDN_WB_EN,
						&tw->tw_enabled) ||
			    ufstw_set_flag(tw, QUERY_FLAG_IDN_WB_EN,
						&tw->tw_enabled))
				goto next;
		} else {
			if (!tw->flush_during_hibern8)
				ufstw_set_flag(tw,
					QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
					&tw->flush_during_hibern8);
			mod_delayed_work(system_wq, &tw->flush_h8_work, 0);
			need_flush = true;
		}
next:
		mutex_unlock(&tw->lock);
	}

	return need_flush;
}

void ufstw_exception_event(struct ufs_hba *hba)
{
	struct ufstw_lu *tw;
	unsigned int lun;

	for (lun = 0; lun < UFS_UPIU_MAX_UNIT_NUM_ID; lun++) {
		tw = hba->ufstw.lu[lun];
		if (tw) {
			tw->next_flush = jiffies;
			mod_delayed_work(system_wq, &tw->flush_work, 0);
		}
	}
}

void ufstw_suspend(struct ufs_hba *hba)
{
	struct ufstw_lu *tw;
	unsigned int lun;

	for (lun = 0; lun < UFS_UPIU_MAX_UNIT_NUM_ID; lun++) {
		tw = hba->ufstw.lu[lun];
		if (tw) {
			cancel_delayed_work_sync(&tw->flush_work);
			cancel_delayed_work_sync(&tw->flush_h8_work);
		}
	}
}

void ufstw_resume(struct ufs_hba *hba)
{
	struct ufstw_lu *tw;
	unsigned int lun;
	unsigned long delay;

	for (lun = 0; lun < UFS_UPIU_MAX_UNIT_NUM_ID; lun++) {
		tw = hba->ufstw.lu[lun];
		if (!tw || !tw->initialized || !tw->tw_enabled ||
		    !tw->next_flush)
			continue;

		delay = time_after_eq(jiffies, tw->next_flush) ? 0 :
			tw->next_flush - jiffies;
		mod_delayed_work(system_wq, &tw->flush_work, delay);
	}
}

void ufstw_reset(struct ufs_hba *hba)
{
	if (atomic_read(&hba->ufstw.state) == UFSTW_PRESENT) {
		unsigned int lun;

		for (lun = 0; lun < UFS_UPIU_MAX_UNIT_NUM_ID; lun++) {
			struct ufstw_lu *tw = hba->ufstw.lu[lun];

			if (!tw)
				continue;
			mutex_lock(&tw->lock);
			if (tw->disabled) {
				if (tw->tw_enabled)
					ufstw_clear_flag(tw, QUERY_FLAG_IDN_WB_EN,
							&tw->tw_enabled);
				if (tw->flush_during_hibern8)
					ufstw_clear_flag(tw,
						QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
						&tw->flush_during_hibern8);
			} else {
				if (tw->mode == UFSTW_MODE_MANUAL && tw->tw_enabled &&
				    ufstw_set_flag(tw, QUERY_FLAG_IDN_WB_EN,
						   &tw->tw_enabled))
					tw->tw_enabled = false;
				if (tw->flush_enabled &&
				    ufstw_set_flag(tw,
						QUERY_FLAG_IDN_WB_BUFF_FLUSH_EN,
						&tw->flush_enabled))
					tw->flush_enabled = false;
				if (tw->flush_during_hibern8 &&
				    ufstw_set_flag(tw,
						QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
						&tw->flush_during_hibern8))
					tw->flush_during_hibern8 = false;
			}
			mutex_unlock(&tw->lock);
		}
		ufstw_resume(hba);
	}
}

void ufstw_remove(struct ufs_hba *hba)
{
	struct ufstw_lu *tw;
	unsigned int lun;

	for (lun = 0; lun < UFS_UPIU_MAX_UNIT_NUM_ID; lun++) {
		tw = hba->ufstw.lu[lun];
		if (!tw)
			continue;
		hba->ufstw.lu[lun] = NULL;
		tw->initialized = false;
		cancel_work_sync(&tw->lifetime_work);
		cancel_delayed_work_sync(&tw->flush_work);
		cancel_delayed_work_sync(&tw->flush_h8_work);
		if (tw->kobj_added) {
			kobject_del(&tw->kobj);
			kobject_put(&tw->kobj);
		} else {
			kfree(tw);
		}
	}
	atomic_set(&hba->ufstw.state, UFSTW_NOT_SUPPORTED);
}

static bool ufstw_present(struct ufstw_lu *tw)
{
	return tw->initialized &&
		atomic_read(&tw->hba->ufstw.state) == UFSTW_PRESENT;
}

static ssize_t ufstw_show_mode(struct ufstw_lu *tw, char *buf)
{
	return sysfs_emit(buf, "%u\n", tw->mode);
}

static ssize_t ufstw_store_mode(struct ufstw_lu *tw, const char *buf,
					size_t count)
{
	unsigned int mode;
	int ret = 0;

	if (kstrtouint(buf, 0, &mode) || mode >= UFSTW_MODE_NUM ||
	    mode == UFSTW_MODE_DISABLED || !ufstw_present(tw))
		return -EINVAL;

	mutex_lock(&tw->lock);
	if (tw->disabled) {
		mutex_unlock(&tw->lock);
		return -EINVAL;
	}
	if (mode == UFSTW_MODE_FS) {
		if (tw->tw_enabled)
			ret = ufstw_clear_flag(tw, QUERY_FLAG_IDN_WB_EN,
					       &tw->tw_enabled);
	} else if (!tw->tw_enabled) {
		ret = ufstw_set_flag(tw, QUERY_FLAG_IDN_WB_EN,
				     &tw->tw_enabled);
	}
	if (!ret)
		tw->mode = mode;
	mutex_unlock(&tw->lock);
	return ret ?: count;
}

static ssize_t ufstw_show_tw_enable(struct ufstw_lu *tw, char *buf)
{
	bool enabled;

	if (ufstw_read_flag(tw, QUERY_FLAG_IDN_WB_EN, &enabled))
		return -EIO;
	tw->tw_enabled = enabled;
	return sysfs_emit(buf, "%u\n", enabled);
}

static ssize_t ufstw_store_tw_enable(struct ufstw_lu *tw, const char *buf,
					     size_t count)
{
	unsigned int enable;
	int ret;

	if (kstrtouint(buf, 0, &enable) || enable > 1 ||
	    !ufstw_present(tw))
		return -EINVAL;

	mutex_lock(&tw->lock);
	if (tw->disabled || tw->mode != UFSTW_MODE_MANUAL) {
		mutex_unlock(&tw->lock);
		return -EINVAL;
	}
	if (enable)
		ret = ufstw_set_flag(tw, QUERY_FLAG_IDN_WB_EN,
				     &tw->tw_enabled);
	else
		ret = ufstw_clear_flag(tw, QUERY_FLAG_IDN_WB_EN,
				       &tw->tw_enabled);
	mutex_unlock(&tw->lock);
	return ret ?: count;
}

static ssize_t ufstw_show_flush_enable(struct ufstw_lu *tw, char *buf)
{
	bool enabled;

	if (ufstw_read_flag(tw, QUERY_FLAG_IDN_WB_BUFF_FLUSH_EN, &enabled))
		return -EIO;
	tw->flush_enabled = enabled;
	return sysfs_emit(buf, "%u\n", enabled);
}

static ssize_t ufstw_store_flush_enable(struct ufstw_lu *tw,
						const char *buf, size_t count)
{
	unsigned int enable;
	int ret;

	if (kstrtouint(buf, 0, &enable) || enable > 1 ||
	    !ufstw_present(tw) || tw->ee_mode == UFSTW_EE_AUTO)
		return -EINVAL;

	mutex_lock(&tw->lock);
	if (enable)
		ret = ufstw_set_flag(tw, QUERY_FLAG_IDN_WB_BUFF_FLUSH_EN,
				     &tw->flush_enabled);
	else
		ret = ufstw_clear_flag(tw, QUERY_FLAG_IDN_WB_BUFF_FLUSH_EN,
				       &tw->flush_enabled);
	mutex_unlock(&tw->lock);
	return ret ?: count;
}

static ssize_t ufstw_show_h8_flush(struct ufstw_lu *tw, char *buf)
{
	bool enabled;

	if (ufstw_read_flag(tw,
			QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8, &enabled))
		return -EIO;
	tw->flush_during_hibern8 = enabled;
	return sysfs_emit(buf, "%u\n", enabled);
}

static ssize_t ufstw_store_h8_flush(struct ufstw_lu *tw, const char *buf,
					    size_t count)
{
	unsigned int enable;
	int ret;

	if (kstrtouint(buf, 0, &enable) || enable > 1 ||
	    !ufstw_present(tw) || tw->ee_mode == UFSTW_EE_AUTO)
		return -EINVAL;

	mutex_lock(&tw->lock);
	if (enable)
		ret = ufstw_set_flag(tw,
			QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
			&tw->flush_during_hibern8);
	else
		ret = ufstw_clear_flag(tw,
			QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
			&tw->flush_during_hibern8);
	mutex_unlock(&tw->lock);
	return ret ?: count;
}

static ssize_t ufstw_show_ee_mode(struct ufstw_lu *tw, char *buf)
{
	return sysfs_emit(buf, "%u\n", tw->ee_mode);
}

static ssize_t ufstw_store_ee_mode(struct ufstw_lu *tw, const char *buf,
					   size_t count)
{
	unsigned int mode, lun;
	int ret;

	if (kstrtouint(buf, 0, &mode) || mode >= UFSTW_EE_MODE_NUM ||
	    !ufstw_present(tw))
		return -EINVAL;

	ret = ufstw_update_ee(tw->hba, mode == UFSTW_EE_AUTO);
	if (ret)
		return ret;

	for (lun = 0; lun < UFS_UPIU_MAX_UNIT_NUM_ID; lun++)
		if (tw->hba->ufstw.lu[lun])
			tw->hba->ufstw.lu[lun]->ee_mode = mode;
	return count;
}

static ssize_t ufstw_show_attr(struct ufstw_lu *tw, char *buf, u32 *value,
				       enum attr_idn idn)
{
	if (ufstw_read_attr(tw, idn, value))
		return -EIO;
	return sysfs_emit(buf, "%u\n", *value);
}

static ssize_t ufstw_show_flush_status(struct ufstw_lu *tw, char *buf)
{
	return ufstw_show_attr(tw, buf, &tw->flush_status,
				       QUERY_ATTR_IDN_WB_FLUSH_STATUS);
}

static ssize_t ufstw_show_available_buffer(struct ufstw_lu *tw, char *buf)
{
	return ufstw_show_attr(tw, buf, &tw->available_buffer,
				       QUERY_ATTR_IDN_AVAIL_WB_BUFF_SIZE);
}

static ssize_t ufstw_show_current_buffer(struct ufstw_lu *tw, char *buf)
{
	return ufstw_show_attr(tw, buf, &tw->current_buffer,
				       QUERY_ATTR_IDN_CURR_WB_BUFF_SIZE);
}

static ssize_t ufstw_show_lifetime(struct ufstw_lu *tw, char *buf)
{
	return ufstw_show_attr(tw, buf, &tw->lifetime_estimate,
				       QUERY_ATTR_IDN_WB_BUFF_LIFE_TIME_EST);
}

static ssize_t ufstw_show_debug(struct ufstw_lu *tw, char *buf)
{
	return sysfs_emit(buf, "%u\n", tw->debug);
}

static ssize_t ufstw_store_debug(struct ufstw_lu *tw, const char *buf,
					 size_t count)
{
	unsigned int enabled;

	if (kstrtouint(buf, 0, &enabled) || enabled > 1)
		return -EINVAL;
	tw->debug = enabled;
	return count;
}

static ssize_t ufstw_show_active_cnt(struct ufstw_lu *tw, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&tw->active_cnt));
}

static ssize_t ufstw_show_flush_th_min(struct ufstw_lu *tw, char *buf)
{
	return sysfs_emit(buf, "%u\n", tw->flush_th_min);
}

static ssize_t ufstw_store_flush_th_min(struct ufstw_lu *tw, const char *buf,
						size_t count)
{
	unsigned int value;

	if (kstrtouint(buf, 0, &value) || value > 10 ||
	    value >= tw->flush_th_max)
		return -EINVAL;
	tw->flush_th_min = value;
	return count;
}

static ssize_t ufstw_show_flush_th_max(struct ufstw_lu *tw, char *buf)
{
	return sysfs_emit(buf, "%u\n", tw->flush_th_max);
}

static ssize_t ufstw_store_flush_th_max(struct ufstw_lu *tw, const char *buf,
						size_t count)
{
	unsigned int value;

	if (kstrtouint(buf, 0, &value) || value > 10 ||
	    value <= tw->flush_th_min)
		return -EINVAL;
	tw->flush_th_max = value;
	return count;
}

static ssize_t ufstw_show_version(struct ufstw_lu *tw, char *buf)
{
	return sysfs_emit(buf, "TW version %04X DD version %04X\n",
			  UFSTW_VER, UFSTW_DD_VER);
}

static struct ufstw_sysfs_entry ufstw_sysfs_entries[];

static ssize_t ufstw_attr_show(struct kobject *kobj, struct attribute *attr,
				       char *buf)
{
	struct ufstw_sysfs_entry *entry =
		container_of(attr, struct ufstw_sysfs_entry, attr);
	struct ufstw_lu *tw = container_of(kobj, struct ufstw_lu, kobj);
	ssize_t ret;

	if (!entry->show)
		return -EIO;
	mutex_lock(&tw->sysfs_lock);
	ret = entry->show(tw, buf);
	mutex_unlock(&tw->sysfs_lock);
	return ret;
}

static ssize_t ufstw_attr_store(struct kobject *kobj, struct attribute *attr,
					const char *buf, size_t count)
{
	struct ufstw_sysfs_entry *entry =
		container_of(attr, struct ufstw_sysfs_entry, attr);
	struct ufstw_lu *tw = container_of(kobj, struct ufstw_lu, kobj);
	ssize_t ret;

	if (!entry->store)
		return -EIO;
	mutex_lock(&tw->sysfs_lock);
	ret = entry->store(tw, buf, count);
	mutex_unlock(&tw->sysfs_lock);
	return ret;
}

static const struct sysfs_ops ufstw_sysfs_ops = {
	.show = ufstw_attr_show,
	.store = ufstw_attr_store,
};

static void ufstw_kobj_release(struct kobject *kobj)
{
	struct ufstw_lu *tw = container_of(kobj, struct ufstw_lu, kobj);

	/* kobject_add() failures leave the LU alive without sysfs. */
	if (tw->kobj_added)
		kfree(tw);
}

static struct kobj_type ufstw_ktype = {
	.sysfs_ops = &ufstw_sysfs_ops,
	.release = ufstw_kobj_release,
};

static struct ufstw_sysfs_entry ufstw_sysfs_entries[] = {
	__ATTR(tw_mode, 0644, ufstw_show_mode, ufstw_store_mode),
	__ATTR(tw_enable, 0644, ufstw_show_tw_enable, ufstw_store_tw_enable),
	__ATTR(flush_enable, 0644, ufstw_show_flush_enable,
		ufstw_store_flush_enable),
	__ATTR(flush_during_hibern_enter, 0644, ufstw_show_h8_flush,
		ufstw_store_h8_flush),
	__ATTR(ee_mode, 0644, ufstw_show_ee_mode, ufstw_store_ee_mode),
	__ATTR(flush_status, 0444, ufstw_show_flush_status, NULL),
	__ATTR(available_buffer_size, 0444, ufstw_show_available_buffer, NULL),
	__ATTR(current_tw_buffer_size, 0444, ufstw_show_current_buffer, NULL),
	__ATTR(lifetime_est, 0444, ufstw_show_lifetime, NULL),
	__ATTR(debug, 0644, ufstw_show_debug, ufstw_store_debug),
	__ATTR(debug_active_cnt, 0444, ufstw_show_active_cnt, NULL),
	__ATTR(flush_th_min, 0644, ufstw_show_flush_th_min,
		ufstw_store_flush_th_min),
	__ATTR(flush_th_max, 0644, ufstw_show_flush_th_max,
		ufstw_store_flush_th_max),
	__ATTR(version, 0444, ufstw_show_version, NULL),
	{ }
};

static int ufstw_create_sysfs(struct ufs_hba *hba, struct ufstw_lu *tw)
{
	struct ufstw_sysfs_entry *entry;
	int ret;

	tw->sysfs_entries = ufstw_sysfs_entries;
	kobject_init(&tw->kobj, &ufstw_ktype);
	ret = kobject_add(&tw->kobj, &hba->dev->kobj, "ufstw_lu%d", tw->lun);
	if (ret) {
		kobject_put(&tw->kobj);
		return ret;
	}

	for (entry = tw->sysfs_entries; entry->attr.name; entry++) {
		ret = sysfs_create_file(&tw->kobj, &entry->attr);
		if (ret) {
			kobject_del(&tw->kobj);
			kobject_put(&tw->kobj);
			return ret;
		}
	}
	kobject_uevent(&tw->kobj, KOBJ_ADD);
	tw->kobj_added = true;
	return 0;
}
