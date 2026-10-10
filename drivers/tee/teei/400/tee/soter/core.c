// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015-2019, MICROTRUST Incorporated
 * Copyright (c) 2015, Linaro Limited
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/err.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <tee_drv.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#define IMSG_TAG "[tz_driver]"
#include "imsg_log.h"


#include "soter_private.h"
#include "soter_smc.h"
#include <nt_smc_call.h>


#define SOTER_SHM_NUM_PRIV_PAGES	1

static phys_addr_t shm_pool_paddr;
static size_t shm_pool_size;
static atomic_t is_shm_pool_available = ATOMIC_INIT(0);
static DECLARE_COMPLETION(shm_pool_registered);

int teei_new_capi_init(void)
{
	if (shm_pool_paddr && shm_pool_size) {
		int ret = soter_register_shm_pool(
				shm_pool_paddr, shm_pool_size);

		if (!ret) {
			atomic_set(&is_shm_pool_available, 1);
			complete_all(&shm_pool_registered);
		}
		return ret;
	}

	IMSG_ERROR("capi reserve memory is NULL!\n");

	return -ENOMEM;
}

static void soter_get_version(struct tee_device *teedev,
			      struct tee_ioctl_version_data *vers)
{
	struct tee_ioctl_version_data v = {
		.impl_id = 0x0,
		.impl_caps = 0x0,
		.gen_caps = TEE_GEN_CAP_GP,
	};
	*vers = v;
}

static int soter_open(struct tee_context *ctx)
{
	struct soter_context_data *ctxdata;
	int ret;

	ctxdata = kzalloc(sizeof(*ctxdata), GFP_KERNEL);
	if (!ctxdata)
		return -ENOMEM;

	mutex_init(&ctxdata->mutex);
	INIT_LIST_HEAD(&ctxdata->sess_list);

	ctx->data = ctxdata;

	if (!atomic_read(&is_shm_pool_available)) {
		ret = wait_for_completion_interruptible(&shm_pool_registered);
		if (ret == -ERESTARTSYS)
			return -EINTR;
	}

	return 0;
}

static void soter_release(struct tee_context *ctx)
{
	struct soter_context_data *ctxdata = ctx->data;
	struct tee_shm *shm;
	struct optee_msg_arg *arg = NULL;
	phys_addr_t parg = 0;
	struct soter_session *sess;
	struct soter_session *sess_tmp;

	if (!ctxdata)
		return;

	shm = isee_shm_alloc_noid(ctx, sizeof(struct optee_msg_arg), TEE_SHM_MAPPED);
	if (!IS_ERR(shm)) {
		arg = isee_shm_get_va(shm, 0);
		/*
		 * If va2pa fails for some reason, we can't call
		 * soter_close_session(), only free the memory. Secure OS
		 * will leak sessions and finally refuse more sessions, but
		 * we will at least let normal world reclaim its memory.
		 */
		if (!IS_ERR(arg))
			isee_shm_va2pa(shm, arg, &parg);
	}

	list_for_each_entry_safe(sess, sess_tmp, &ctxdata->sess_list,
				list_node) {
		list_del(&sess->list_node);
		if (!IS_ERR_OR_NULL(arg)) {
			memset(arg, 0, sizeof(*arg));
			arg->cmd = OPTEE_MSG_CMD_CLOSE_SESSION;
			arg->session = sess->session_id;
			soter_do_call_with_arg(ctx, parg);
		}
		kfree(sess);
	}
	kfree(ctxdata);

	if (!IS_ERR(shm))
		isee_shm_free(shm);

	ctx->data = NULL;
}

static struct tee_driver_ops soter_ops = {
	.get_version = soter_get_version,
	.open = soter_open,
	.release = soter_release,
	.open_session = soter_open_session,
	.close_session = soter_close_session,
	.invoke_func = soter_invoke_func,
	.cancel_req = soter_cancel_func,
};

static struct tee_desc soter_desc = {
	.name = "soter-clnt",
	.ops = &soter_ops,
	.owner = THIS_MODULE,
};

static struct soter_priv *soter_priv;

struct tee_device *isee_get_teedev(void)
{
	if (soter_priv != NULL)
		return soter_priv->teedev;

	IMSG_ERROR("[%s][%d] soter_priv is NULL!\n", __func__, __LINE__);
	return NULL;
}

static bool teei_shm_pool_valid(u64 paddr, u64 size)
{
	u64 end;

	if (!paddr || size > INT_MAX ||
	    size < 2 * SOTER_SHM_NUM_PRIV_PAGES * PAGE_SIZE ||
	    !IS_ALIGNED(paddr, PAGE_SIZE) || !IS_ALIGNED(size, PAGE_SIZE))
		return false;

	return !check_add_overflow(paddr, size, &end) && end <= (1ULL << 32);
}

static int teei_get_dt_shm_pool(phys_addr_t *paddr, size_t *size)
{
	struct device_node *node;
	u64 pool[2];
	int ret;

	node = of_find_compatible_node(NULL, NULL, "microtrust,utos");
	if (!node) {
		IMSG_ERROR("failed to find UTOS device\n");
		return -ENODEV;
	}

	ret = of_property_read_u64_array(node, "microtrust,shared-memory",
					 pool, ARRAY_SIZE(pool));
	of_node_put(node);
	if (ret) {
		IMSG_ERROR("failed to read UTOS shared memory: %d\n", ret);
		return ret;
	}

	if (!teei_shm_pool_valid(pool[0], pool[1]))
		return -ERANGE;

	*paddr = pool[0];
	*size = pool[1];
	IMSG_INFO("use described shared memory @ %pa size 0x%zx\n",
		  paddr, *size);

	return 0;
}

static int teei_get_shm_pool(phys_addr_t *paddr, size_t *size)
{
	long fw_paddr;
	long fw_size;

	fw_size = teei_secure_call(N_GET_RESERVED_MEM_SIZE, 0, 0, 0);
	fw_paddr = 0;
	if (fw_size > 0)
		fw_paddr = teei_secure_call(N_GET_RESERVED_MEM_PADDR, 0, 0, 0);
	if (fw_paddr > 0 && fw_size > 0 &&
	    teei_shm_pool_valid(fw_paddr, fw_size)) {
		*paddr = fw_paddr;
		*size = fw_size;
		IMSG_INFO("use firmware shared memory @ %pa size 0x%zx\n",
			  paddr, *size);
		return 0;
	}

	IMSG_ERROR("invalid firmware shared memory 0x%lx/0x%lx\n",
		   (unsigned long)fw_paddr, (unsigned long)fw_size);
	return teei_get_dt_shm_pool(paddr, size);
}

static struct tee_shm_pool *
soter_config_shm_memremap(void **memremaped_shm)
{
	struct tee_shm_pool *pool;
	unsigned long vaddr;
	phys_addr_t paddr;
	size_t size;
	void *va;
	struct tee_shm_pool_mem_info priv_info;
	struct tee_shm_pool_mem_info dmabuf_info;
	int ret;

	ret = teei_get_shm_pool(&paddr, &size);
	if (ret)
		return ERR_PTR(ret);

	IMSG_INFO("reserved memory @ 0x%llx, size %zx\n",
		(unsigned long long)paddr, size);

	if (!paddr || size < 2 * SOTER_SHM_NUM_PRIV_PAGES * PAGE_SIZE) {
		IMSG_ERROR("too small shared memory area\n");
		return ERR_PTR(-EINVAL);
	}

	va = ioremap_cache(paddr, size);

	if (!va) {
		IMSG_ERROR("shared memory ioremap failed\n");
		return ERR_PTR(-ENOMEM);
	}
	vaddr = (unsigned long)va;

	priv_info.vaddr = vaddr;
	priv_info.paddr = paddr;
	priv_info.size = SOTER_SHM_NUM_PRIV_PAGES * PAGE_SIZE;
	dmabuf_info.vaddr = vaddr + SOTER_SHM_NUM_PRIV_PAGES * PAGE_SIZE;
	dmabuf_info.paddr = paddr + SOTER_SHM_NUM_PRIV_PAGES * PAGE_SIZE;
	dmabuf_info.size = size - SOTER_SHM_NUM_PRIV_PAGES * PAGE_SIZE;

	pool = isee_shm_pool_alloc_res_mem(&priv_info, &dmabuf_info);
	if (IS_ERR(pool)) {
		iounmap(va);
		goto out;
	}

	shm_pool_paddr = paddr;
	shm_pool_size = size;
	*memremaped_shm = va;
out:
	return pool;
}

static void soter_remove(struct soter_priv *soter)
{
	/*
	 * The device has to be unregistered before we can free the
	 * other resources.
	 */
	isee_device_unregister(soter->teedev);

	isee_shm_pool_free(soter->pool);
	if (soter->memremaped_shm)
		iounmap(soter->memremaped_shm);
	shm_pool_paddr = 0;
	shm_pool_size = 0;
	mutex_destroy(&soter->call_queue.mutex);

	kfree(soter);
}

int soter_driver_init(void)
{
	struct tee_shm_pool *pool = NULL;
	struct tee_device *teedev = NULL;
	void *memremaped_shm = NULL;
	int rc;

	soter_priv = kzalloc(sizeof(*soter_priv), GFP_KERNEL);
	if (!soter_priv) {
		rc = -ENOMEM;
		goto err;
	}

	pool = soter_config_shm_memremap(&memremaped_shm);
	if (IS_ERR(pool)) {
		rc = PTR_ERR(pool);
		pool = NULL;
		goto err;
	}
	soter_priv->pool = pool;
	soter_priv->memremaped_shm = memremaped_shm;

	teedev = isee_device_alloc(&soter_desc, NULL, pool, soter_priv);
	if (IS_ERR(teedev)) {
		rc = PTR_ERR(teedev);
		goto err;
	}
	soter_priv->teedev = teedev;

	rc = isee_device_register(teedev);
	if (rc)
		goto err;

	return 0;

err:
	if (soter_priv) {
		/*
		 * isee_device_unregister() is safe to call even if the
		 * devices hasn't been registered with
		 * tee_device_register() yet.
		 */
		isee_device_unregister(soter_priv->teedev);
		kfree(soter_priv);
		soter_priv = NULL;
	}
	if (pool)
		isee_shm_pool_free(pool);
	if (memremaped_shm)
		iounmap(memremaped_shm);
	shm_pool_paddr = 0;
	shm_pool_size = 0;
	return rc;
}
EXPORT_SYMBOL_GPL(soter_driver_init);

void soter_driver_exit(void)
{
	if (soter_priv)
		soter_remove(soter_priv);
	soter_priv = NULL;
}
EXPORT_SYMBOL_GPL(soter_driver_exit);
