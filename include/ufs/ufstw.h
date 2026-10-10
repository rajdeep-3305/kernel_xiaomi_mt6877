/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek Turbo Write compatibility layer for the 6.6 UFS core.
 *
 * Ruby's 4.19 driver called the UFS WriteBooster feature "Turbo Write" and
 * kept per-LU state around the standard UFS query flags and attributes.  The
 * 6.6 core already owns the common WriteBooster implementation; this header
 * describes only the vendor state that is still needed for Ruby's per-LU
 * lifetime and suspend policy.
 */

#ifndef _UFS_TURBO_WRITE_H_
#define _UFS_TURBO_WRITE_H_

#include <linux/atomic.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <ufs/ufs.h>

struct scsi_device;
struct ufshcd_lrb;
struct ufs_hba;

#define UFSTW_LIFETIME_SECT		2097152U /* 1 GiB in 512-byte sectors */
#define UFSTW_VER			0x0101
#define UFSTW_DD_VER			0x0103
#define UFSTW_MAX_LIFETIME_VALUE	0x0B
#define UFSTW_LIFETIME_NOT_GUARANTEED	0x80000080U
#define UFSTW_FLUSH_CHECK_PERIOD_MS	1000
#define UFSTW_FLUSH_WORKER_TH_MIN	3
#define UFSTW_FLUSH_WORKER_TH_MAX	8

enum ufstw_state {
	UFSTW_NOT_SUPPORTED = -1,
	UFSTW_NEED_INIT = 0,
	UFSTW_PRESENT = 1,
	UFSTW_FAILED = -2,
	UFSTW_RESET = -3,
};

enum ufstw_mode {
	UFSTW_MODE_DISABLED,
	UFSTW_MODE_MANUAL,
	UFSTW_MODE_FS,
	UFSTW_MODE_NUM,
};

enum ufstw_ee_mode {
	UFSTW_EE_DISABLED,
	UFSTW_EE_AUTO,
	UFSTW_EE_MODE_NUM,
};

struct ufstw_sysfs_entry;

struct ufstw_dev_info {
	bool supported;
	bool geo_valid;
	u8 buffer_type;
	u8 number_lu;
	u8 no_user_space_reduction;
};

struct ufstw_lu {
	struct ufs_hba *hba;
	u8 lun;
	u32 buffer_alloc_units;

	struct mutex lock;
	spinlock_t lifetime_lock;
	bool initialized;
	bool tw_enabled;
	bool flush_during_hibern8;
	bool flush_enabled;
	bool disabled;
	enum ufstw_mode mode;
	enum ufstw_ee_mode ee_mode;

	u32 lifetime_estimate;
	u32 available_buffer;
	u32 current_buffer;
	u32 flush_status;
	u32 flush_th_min;
	u32 flush_th_max;
	u64 written_sectors;
	unsigned long next_flush;
	atomic_t active_cnt;
	bool debug;

	struct work_struct lifetime_work;
	struct delayed_work flush_work;
	struct delayed_work flush_h8_work;

	struct kobject kobj;
	struct mutex sysfs_lock;
	struct ufstw_sysfs_entry *sysfs_entries;
	bool kobj_added;
};

struct ufstw_info {
	struct ufstw_dev_info dev;
	struct ufstw_lu *lu[UFS_UPIU_MAX_UNIT_NUM_ID];
	atomic_t state;
};

#ifdef CONFIG_SCSI_UFS_TW
void ufstw_get_dev_info(struct ufs_hba *hba, const u8 *desc_buf);
void ufstw_get_geo_info(struct ufs_hba *hba, const u8 *geo_buf);
void ufstw_init(struct ufs_hba *hba);
void ufstw_init_lu(struct ufs_hba *hba, struct scsi_device *sdev);
void ufstw_destroy_lu(struct ufs_hba *hba, struct scsi_device *sdev);
void ufstw_prep(struct ufs_hba *hba, struct ufshcd_lrb *lrbp);
void ufstw_reset(struct ufs_hba *hba);
void ufstw_suspend(struct ufs_hba *hba);
void ufstw_resume(struct ufs_hba *hba);
bool ufstw_need_flush(struct ufs_hba *hba);
void ufstw_exception_event(struct ufs_hba *hba);
void ufstw_remove(struct ufs_hba *hba);
#else
static inline void ufstw_get_dev_info(struct ufs_hba *hba, const u8 *desc_buf) {}
static inline void ufstw_get_geo_info(struct ufs_hba *hba, const u8 *geo_buf) {}
static inline void ufstw_init(struct ufs_hba *hba) {}
static inline void ufstw_init_lu(struct ufs_hba *hba, struct scsi_device *sdev) {}
static inline void ufstw_destroy_lu(struct ufs_hba *hba, struct scsi_device *sdev) {}
static inline void ufstw_prep(struct ufs_hba *hba, struct ufshcd_lrb *lrbp) {}
static inline void ufstw_reset(struct ufs_hba *hba) {}
static inline void ufstw_suspend(struct ufs_hba *hba) {}
static inline void ufstw_resume(struct ufs_hba *hba) {}
static inline bool ufstw_need_flush(struct ufs_hba *hba) { return false; }
static inline void ufstw_exception_event(struct ufs_hba *hba) {}
static inline void ufstw_remove(struct ufs_hba *hba) {}
#endif

#endif /* _UFS_TURBO_WRITE_H_ */
