// SPDX-License-Identifier: GPL-2.0
/*
 * Stub for MTEE peer ops when CONFIG_MTK_GZ_KREE is disabled.
 * Provides a non-functional get_mtee_peer_ops() so that
 * mtee_devices.c / mtee_invoke.c can link without the GenieZone
 * KREE backend. Any caller that reaches the returned ops will fail
 * gracefully because all callbacks are NULL — peer_mgr / invoke
 * paths check for that.
 */

#include <linux/module.h>
#include <linux/types.h>
#include "private/tmem_error.h"
#include "private/tmem_device.h"
#include "mtee_impl/mtee_ops.h"

static struct trusted_driver_operations mtee_peer_ops_stub;

void get_mtee_peer_ops(struct trusted_driver_operations **ops)
{
	pr_info("MTEE_PEER_OPS (stub: GZ_KREE disabled)\n");
	*ops = &mtee_peer_ops_stub;
}
