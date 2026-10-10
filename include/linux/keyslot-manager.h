/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2019 Google LLC
 */

#ifndef __LINUX_KEYSLOT_MANAGER_H
#define __LINUX_KEYSLOT_MANAGER_H

#include <linux/bio.h>
#include <linux/blk-crypto.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

struct request_queue;
struct blk_keyslot_manager;
struct blk_ksm_keyslot;

/**
 * struct blk_ksm_ll_ops - functions to manage keyslots in hardware
 */
struct blk_ksm_ll_ops {
	int (*keyslot_program)(struct blk_keyslot_manager *ksm,
			       const struct blk_crypto_key *key,
			       unsigned int slot);
	int (*keyslot_evict)(struct blk_keyslot_manager *ksm,
			     const struct blk_crypto_key *key,
			     unsigned int slot);
};

struct blk_keyslot_manager {
	struct blk_ksm_ll_ops ksm_ll_ops;
	unsigned int max_dun_bytes_supported;
	unsigned int crypto_modes_supported[BLK_ENCRYPTION_MODE_MAX];
	struct device *dev;

	/* private: keyslot manager internals */
	unsigned int num_slots;
	struct rw_semaphore lock;

	wait_queue_head_t idle_slots_wait_queue;
	struct list_head idle_slots;
	spinlock_t idle_slots_lock;

	struct hlist_head *slot_hashtable;
	unsigned int log_slot_ht_size;

	struct blk_ksm_keyslot *slots;
};

int blk_ksm_init(struct blk_keyslot_manager *ksm, unsigned int num_slots);
int devm_blk_ksm_init(struct device *dev, struct blk_keyslot_manager *ksm,
		      unsigned int num_slots);

blk_status_t blk_ksm_get_slot_for_key(struct blk_keyslot_manager *ksm,
				      const struct blk_crypto_key *key,
				      struct blk_ksm_keyslot **slot_ptr);

unsigned int blk_ksm_get_slot_idx(struct blk_ksm_keyslot *slot);
void blk_ksm_put_slot(struct blk_ksm_keyslot *slot);

bool blk_ksm_crypto_cfg_supported(struct blk_keyslot_manager *ksm,
				  const struct blk_crypto_config *cfg);
int blk_ksm_evict_key(struct blk_keyslot_manager *ksm,
		      const struct blk_crypto_key *key);

void blk_ksm_reprogram_all_keys(struct blk_keyslot_manager *ksm);
void blk_ksm_destroy(struct blk_keyslot_manager *ksm);
bool blk_ksm_register(struct blk_keyslot_manager *ksm, struct request_queue *q);

void blk_ksm_intersect_modes(struct blk_keyslot_manager *parent,
			     const struct blk_keyslot_manager *child);
void blk_ksm_init_passthrough(struct blk_keyslot_manager *ksm);
bool blk_ksm_is_superset(struct blk_keyslot_manager *ksm_superset,
			 struct blk_keyslot_manager *ksm_subset);
void blk_ksm_update_capabilities(struct blk_keyslot_manager *target_ksm,
				 struct blk_keyslot_manager *reference_ksm);

#endif /* __LINUX_KEYSLOT_MANAGER_H */
