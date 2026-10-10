/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2019 MediaTek Inc.
 */

#ifndef _TASK_TURBO_FUTEX_H_
#define _TASK_TURBO_FUTEX_H_

#include "turbo_common.h"

inline void futex_plist_add(struct futex_q *q, struct futex_hash_bucket *hb)
{
	struct futex_q *this;
	struct plist_node *current_node = &q->list;
	struct plist_node *this_node;

	if (!sub_feat_enable(SUB_FEAT_LOCK) ||
	    !is_turbo_task(current) || q->list.prio != MAX_RT_PRIO) {
		plist_add(&q->list, &hb->chain);
		return;
	}

	/* Insert through plist first so both of its index lists stay valid. */
	plist_add(&q->list, &hb->chain);
	plist_for_each_entry(this, &hb->chain, list) {
		if (this == q)
			break;
		if (this->list.prio == q->list.prio &&
		    (!this->pi_state || !this->rt_waiter) &&
		    !is_turbo_task(this->task)) {
			this_node = &this->list;
			if (!list_empty(&this_node->prio_list))
				list_replace_init(&this_node->prio_list,
						  &current_node->prio_list);
			list_move_tail(&current_node->node_list,
				       &this_node->node_list);
			break;
		}
	}
}
#endif
