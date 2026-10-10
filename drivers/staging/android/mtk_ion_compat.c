// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek legacy ION userspace compatibility for DMA-BUF heaps.
 *
 * Allocations are serviced by DMA-BUF heaps while legacy handles and the
 * MediaTek custom ioctl ABI remain available to old vendor userspace.
 */

#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/dma-mapping.h>
#include <linux/fdtable.h>
#include <linux/idr.h>
#include <linux/iommu.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/log2.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mtk_dma_heap.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>

#include "uapi/mtk_ion/ion.h"

#define MTK_ION_COMPAT_NAME		"ion"

#define MTK_ION_CMD_SYSTEM		0
#define MTK_ION_CMD_MULTIMEDIA		1
#define MTK_ION_SYS_CACHE_SYNC		0
#define MTK_ION_SYS_SET_CLIENT_NAME	4
#define MTK_ION_MM_CONFIG_BUFFER		0
#define MTK_ION_MM_SET_DEBUG_INFO	1
#define MTK_ION_MM_CONFIG_BUFFER_EXT	5
#define MTK_ION_MM_GET_IOVA		8

#define MTK_ION_HEAP_MULTIMEDIA		10
#define MTK_ION_HEAP_CAMERA		12
#define MTK_ION_HEAP_SYSTEM_MASK	BIT(ION_HEAP_TYPE_SYSTEM)
#define MTK_ION_HEAP_MULTIMEDIA_MASK	BIT(MTK_ION_HEAP_MULTIMEDIA)
#define MTK_ION_HEAP_CAMERA_MASK	BIT(MTK_ION_HEAP_CAMERA)
#define MTK_ION_HEAP_SUPPORTED_MASK	(MTK_ION_HEAP_SYSTEM_MASK | \
					 MTK_ION_HEAP_MULTIMEDIA_MASK | \
					 MTK_ION_HEAP_CAMERA_MASK)
#define MTK_ION_FLAG_MM_HEAP_INIT_ZERO	BIT(16)
#define MTK_ION_ALLOC_SUPPORTED_FLAGS	(ION_FLAG_CACHED | \
					 ION_FLAG_CACHED_NEEDS_SYNC | \
					 ION_FLAG_FREE_WITHOUT_DEFER | \
					 MTK_ION_FLAG_MM_HEAP_INIT_ZERO)

#define MTK_ION_CACHED_HEAP_NAME		"mtk_mm"
#define MTK_ION_UNCACHED_HEAP_NAME	"mtk_mm-uncached"

#define MTK_M4U_LARB_SHIFT		5
#define MTK_M4U_LARB_MASK		0x1f
#define MTK_M4U_PORT_MASK		0x1f

#define MTK_VIDEO_IOVA_BASE		BIT_ULL(32)
#define MTK_VIDEO_IOVA_END		(BIT_ULL(33) - 1)
#define MTK_MDP_IOVA_BASE		(2ULL << 32)
#define MTK_MDP_IOVA_END		((3ULL << 32) - 1)
#define MTK_CCU0_IOVA_BASE		0x240000000ULL
#define MTK_CCU0_IOVA_END		(MTK_CCU0_IOVA_BASE + SZ_64M - 1)
#define MTK_CCU1_IOVA_BASE		0x244000000ULL
#define MTK_CCU1_IOVA_END		(MTK_CCU1_IOVA_BASE + SZ_64M - 1)

enum mtk_ion_iommu_target {
	MTK_ION_IOMMU_MDP,
	MTK_ION_IOMMU_VIDEO,
	MTK_ION_IOMMU_CCU0,
	MTK_ION_IOMMU_CCU1,
	MTK_ION_IOMMU_TARGET_COUNT,
};

enum mtk_ion_cache_sync_type {
	MTK_ION_CACHE_CLEAN_BY_RANGE,
	MTK_ION_CACHE_INVALID_BY_RANGE,
	MTK_ION_CACHE_FLUSH_BY_RANGE,
	MTK_ION_CACHE_CLEAN_BY_RANGE_USE_PA,
	MTK_ION_CACHE_INVALID_BY_RANGE_USE_PA,
	MTK_ION_CACHE_FLUSH_BY_RANGE_USE_PA,
	MTK_ION_CACHE_CLEAN_ALL,
	MTK_ION_CACHE_INVALID_ALL,
	MTK_ION_CACHE_FLUSH_ALL,
};

struct mtk_ion_cache_sync_data {
	union {
		__s32 handle;
		void *kernel_handle;
	};
	void *va;
	__u32 size;
	__u32 sync_type;
	__u64 iova;
};

struct mtk_ion_sys_data {
	__u32 sys_cmd;
	__u32 __pad;
	struct mtk_ion_cache_sync_data cache_sync;
};

/* Native legacy ABI from MediaTek's ion_drv.h. */
struct mtk_ion_get_iova_data {
	union {
		__s32 handle;
		void *kernel_handle;
	};
	__s32 module_id;
	__u32 security;
	__u32 coherent;
	__u32 reserve_iova_start;
	__u32 reserve_iova_end;
	__u64 phy_addr;
	unsigned long len;
};

struct mtk_ion_mm_data {
	__u32 mm_cmd;
	__u32 __pad;
	struct mtk_ion_get_iova_data get_iova;
};

#ifdef CONFIG_COMPAT
struct mtk_ion_allocation_data32 {
	compat_size_t len;
	compat_size_t align;
	compat_uint_t heap_id_mask;
	compat_uint_t flags;
	compat_int_t handle;
};

struct mtk_ion_custom_data32 {
	compat_uint_t cmd;
	compat_ulong_t arg;
};

struct mtk_ion_get_iova_data32 {
	compat_int_t handle;
	compat_int_t module_id;
	compat_uint_t security;
	compat_uint_t coherent;
	compat_uint_t reserve_iova_start;
	compat_uint_t reserve_iova_end;
	compat_u64 phy_addr;
	compat_size_t len;
};

struct mtk_ion_mm_data32 {
	compat_uint_t mm_cmd;
	compat_uint_t __pad;
	struct mtk_ion_get_iova_data32 get_iova;
};

struct mtk_ion_cache_sync_data32 {
	compat_int_t handle;
	compat_uptr_t va;
	compat_size_t size;
	compat_uint_t sync_type;
	compat_u64 iova;
};

struct mtk_ion_sys_data32 {
	compat_uint_t sys_cmd;
	compat_uint_t __pad;
	struct mtk_ion_cache_sync_data32 cache_sync;
};

#define MTK_ION_IOC_CUSTOM32 \
	_IOWR(ION_IOC_MAGIC, 6, struct mtk_ion_custom_data32)
#define MTK_ION_IOC_ALLOC32 \
	_IOWR(ION_IOC_MAGIC, 0, struct mtk_ion_allocation_data32)
#endif

struct mtk_ion_compat_buffer {
	struct dma_buf *dmabuf;
	int module_id;
	unsigned int user_refs;
};

struct mtk_ion_compat {
	struct device *dev;
	struct device *mapping_devs[MTK_ION_IOMMU_TARGET_COUNT];
	struct miscdevice miscdev;
	/* Protects online and on-demand mapping device initialization. */
	struct mutex state_lock;
	struct kref ref;
	bool online;
};

struct mtk_ion_compat_client {
	struct mtk_ion_compat *compat;
	/* Serializes handle lookup, removal and persistent DMA mappings. */
	struct mutex lock;
	struct idr handles;
};

static void mtk_ion_compat_put(struct kref *ref)
{
	struct mtk_ion_compat *compat =
		container_of(ref, struct mtk_ion_compat, ref);
	unsigned int i;

	pm_runtime_disable(compat->dev);
	for (i = MTK_ION_IOMMU_VIDEO; i < MTK_ION_IOMMU_TARGET_COUNT; i++) {
		if (compat->mapping_devs[i])
			put_device(compat->mapping_devs[i]);
	}
	put_device(compat->dev);
	kfree(compat);
}

static void mtk_ion_buffer_release(struct mtk_ion_compat_buffer *buffer)
{
	dma_buf_put(buffer->dmabuf);
	kfree(buffer);
}

static int mtk_ion_add_buffer(struct mtk_ion_compat_client *client,
			      struct dma_buf *dmabuf)
{
	struct mtk_ion_compat_buffer *buffer;
	int handle;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		dma_buf_put(dmabuf);
		return -ENOMEM;
	}
	buffer->dmabuf = dmabuf;
	buffer->module_id = -1;
	buffer->user_refs = 1;

	mutex_lock(&client->lock);
	handle = idr_alloc(&client->handles, buffer, 1, 0, GFP_KERNEL);
	mutex_unlock(&client->lock);
	if (handle < 0)
		mtk_ion_buffer_release(buffer);

	return handle;
}

static int mtk_ion_import(struct mtk_ion_compat_client *client, int fd)
{
	struct mtk_ion_compat_buffer *buffer;
	struct mtk_ion_compat_buffer *entry;
	struct dma_buf *dmabuf;
	int id;
	int handle;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		dma_buf_put(dmabuf);
		return -ENOMEM;
	}
	buffer->dmabuf = dmabuf;
	buffer->module_id = -1;
	buffer->user_refs = 1;

	mutex_lock(&client->lock);
	idr_for_each_entry(&client->handles, entry, id) {
		if (entry->dmabuf != dmabuf)
			continue;
		if (entry->user_refs == UINT_MAX) {
			handle = -EOVERFLOW;
		} else {
			entry->user_refs++;
			handle = id;
		}
		mutex_unlock(&client->lock);
		mtk_ion_buffer_release(buffer);
		return handle;
	}
	handle = idr_alloc(&client->handles, buffer, 1, 0, GFP_KERNEL);
	mutex_unlock(&client->lock);
	if (handle < 0)
		mtk_ion_buffer_release(buffer);

	return handle;
}

static int mtk_ion_alloc(struct mtk_ion_compat_client *client,
			 const struct ion_allocation_data *data)
{
	const char *heap_name;
	struct dma_heap *heap;
	struct dma_buf *dmabuf;
	unsigned int heap_mask = data->heap_id_mask;
	unsigned int manual_flags = ION_FLAG_CACHED | ION_FLAG_CACHED_NEEDS_SYNC;
	int ret;

	if (!data->len || data->len > SZ_1G)
		return -EINVAL;
	if (data->align && (!is_power_of_2(data->align) ||
			    data->align > PAGE_SIZE))
		return -EINVAL;
	if (heap_mask == UINT_MAX)
		heap_mask = MTK_ION_HEAP_MULTIMEDIA_MASK;
	if (!(heap_mask & MTK_ION_HEAP_SUPPORTED_MASK))
		return -ENODEV;
	if (heap_mask & ~MTK_ION_HEAP_SUPPORTED_MASK)
		return -EOPNOTSUPP;
	if (data->flags & ~MTK_ION_ALLOC_SUPPORTED_FLAGS)
		return -EOPNOTSUPP;

	heap_name = data->flags & ION_FLAG_CACHED ?
		MTK_ION_CACHED_HEAP_NAME : MTK_ION_UNCACHED_HEAP_NAME;
	heap = dma_heap_find(heap_name);
	if (!heap)
		return -ENODEV;

	dmabuf = dma_heap_buffer_alloc(heap, data->len,
				       O_RDWR | O_CLOEXEC, 0);
	dma_heap_put(heap);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	if ((data->flags & manual_flags) == manual_flags) {
		ret = mtk_dma_heap_set_legacy_manual_sync(dmabuf);
		if (ret) {
			dma_buf_put(dmabuf);
			return ret;
		}
	}

	return mtk_ion_add_buffer(client, dmabuf);
}

static int mtk_ion_free(struct mtk_ion_compat_client *client, int handle)
{
	struct mtk_ion_compat_buffer *buffer;
	struct mtk_ion_compat_buffer *release = NULL;
	int ret = 0;

	if (handle <= 0)
		return -EINVAL;

	mutex_lock(&client->lock);
	buffer = idr_find(&client->handles, handle);
	if (!buffer || !buffer->user_refs) {
		ret = -EINVAL;
	} else if (!--buffer->user_refs) {
		release = idr_remove(&client->handles, handle);
	}
	mutex_unlock(&client->lock);
	if (release)
		mtk_ion_buffer_release(release);

	return ret;
}

static int mtk_ion_share(struct mtk_ion_compat_client *client, int handle)
{
	struct mtk_ion_compat_buffer *buffer;
	int fd;

	mutex_lock(&client->lock);
	buffer = idr_find(&client->handles, handle);
	if (!buffer) {
		mutex_unlock(&client->lock);
		return -EINVAL;
	}

	get_dma_buf(buffer->dmabuf);
	fd = dma_buf_fd(buffer->dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(buffer->dmabuf);
	mutex_unlock(&client->lock);

	return fd;
}

static int mtk_ion_validate_mapping(struct dma_buf *dmabuf,
				    struct device *mapping_dev,
				    dma_addr_t iova, size_t mapped_len)
{
	struct iommu_domain *domain;
	dma_addr_t aperture_start, aperture_end;

	domain = iommu_get_domain_for_dev(mapping_dev);
	if (!domain || !domain->geometry.force_aperture)
		return -ENODEV;
	aperture_start = domain->geometry.aperture_start;
	aperture_end = domain->geometry.aperture_end;

	if (mapped_len < dmabuf->size)
		return -ERANGE;
	if (!mapped_len || iova < aperture_start || iova > aperture_end ||
	    mapped_len - 1 > aperture_end - iova)
		return -ERANGE;
	return 0;
}

static bool mtk_ion_module_uses_mdp_domain(int module_id)
{
	unsigned int larb = (module_id >> MTK_M4U_LARB_SHIFT) &
		MTK_M4U_LARB_MASK;
	unsigned int port = module_id & MTK_M4U_PORT_MASK;

	switch (larb) {
	case 2:
	case 9:
	case 11:
	case 16:
	case 17:
	case 18:
	case 19:
	case 20:
		return true;
	case 13:
		return port != 9 && port != 10;
	case 14:
		return port != 4 && port != 5;
	default:
		return false;
	}
}

static bool mtk_ion_module_uses_video_domain(int module_id)
{
	unsigned int larb = (module_id >> MTK_M4U_LARB_SHIFT) &
		MTK_M4U_LARB_MASK;

	return larb == 4 || larb == 7;
}

static int mtk_ion_module_target(int module_id)
{
	unsigned int larb = (module_id >> MTK_M4U_LARB_SHIFT) &
		MTK_M4U_LARB_MASK;
	unsigned int port = module_id & MTK_M4U_PORT_MASK;

	if ((larb == 13 && (port == 9 || port == 10)) ||
	    (larb == 22 && port == 0))
		return MTK_ION_IOMMU_CCU0;
	if ((larb == 14 && (port == 4 || port == 5)) ||
	    (larb == 23 && port == 0))
		return MTK_ION_IOMMU_CCU1;
	if (mtk_ion_module_uses_video_domain(module_id))
		return MTK_ION_IOMMU_VIDEO;
	if (mtk_ion_module_uses_mdp_domain(module_id))
		return MTK_ION_IOMMU_MDP;

	return -EXDEV;
}

static int mtk_ion_validate_request(const struct mtk_ion_get_iova_data *data,
				    int target)
{
	u32 expected_start, expected_end;

	if (data->security)
		return -EOPNOTSUPP;
	if (target == MTK_ION_IOMMU_MDP || target == MTK_ION_IOMMU_VIDEO) {
		if (data->coherent || data->reserve_iova_start ||
		    data->reserve_iova_end)
			return -EOPNOTSUPP;
		return 0;
	}
	if (data->coherent > 1)
		return -EINVAL;
	if (!data->reserve_iova_start && !data->reserve_iova_end)
		return 0;

	if (target == MTK_ION_IOMMU_CCU0) {
		expected_start = lower_32_bits(MTK_CCU0_IOVA_BASE);
		expected_end = lower_32_bits(MTK_CCU0_IOVA_END);
	} else {
		expected_start = lower_32_bits(MTK_CCU1_IOVA_BASE);
		expected_end = lower_32_bits(MTK_CCU1_IOVA_END);
	}
	if (data->reserve_iova_start != expected_start ||
	    data->reserve_iova_end != expected_end)
		return -ERANGE;

	return 0;
}

static int mtk_ion_check_aperture(struct device *dev,
				  dma_addr_t expected_start,
				  dma_addr_t expected_end)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);

	if (!domain)
		return -EPROBE_DEFER;
	if (!domain->geometry.force_aperture ||
	    domain->geometry.aperture_start != expected_start ||
	    domain->geometry.aperture_end != expected_end)
		return -EINVAL;

	return 0;
}

static int mtk_ion_get_map_dev(struct device *dev, const char *property,
			       unsigned int index,
			       dma_addr_t expected_start,
			       dma_addr_t expected_end,
			       const char *target_name,
			       struct device **mapping_dev)
{
	struct platform_device *mapping_pdev;
	struct device_node *mapping_node;
	int ret;

	mapping_node = of_parse_phandle(dev->of_node, property, index);
	if (!mapping_node)
		return dev_err_probe(dev, -EINVAL,
			"missing %s IOMMU mapping device\n", target_name);

	mapping_pdev = of_find_device_by_node(mapping_node);
	of_node_put(mapping_node);
	if (!mapping_pdev)
		return dev_err_probe(dev, -EPROBE_DEFER,
			"waiting for %s IOMMU mapping device\n", target_name);

	/* DMA configuration precedes probe; wait for the provider's PM setup. */
	if (!device_is_bound(&mapping_pdev->dev)) {
		put_device(&mapping_pdev->dev);
		return dev_err_probe(dev, -EPROBE_DEFER,
			"waiting for %s IOMMU mapping driver\n", target_name);
	}

	ret = mtk_ion_check_aperture(&mapping_pdev->dev, expected_start,
				     expected_end);
	if (ret) {
		put_device(&mapping_pdev->dev);
		return dev_err_probe(dev, ret,
			"unsafe %s IOMMU mapping aperture\n", target_name);
	}

	*mapping_dev = &mapping_pdev->dev;
	return 0;
}

/*
 * Graphics and CCU must not wait for an optional video provider. Resolve
 * video on its first GET_IOVA, allowing provider deferral to be retried and
 * refusing video mappings with a stale DT instead of blocking /dev/ion.
 */
static struct device *mtk_ion_mapping_dev(struct mtk_ion_compat *compat,
					  int target)
{
	struct device *mapping_dev;
	int ret;

	mutex_lock(&compat->state_lock);
	if (!compat->online) {
		mapping_dev = ERR_PTR(-ENODEV);
		goto out_unlock;
	}
	mapping_dev = compat->mapping_devs[target];
	if (!mapping_dev && target == MTK_ION_IOMMU_VIDEO &&
	    of_property_present(compat->dev->of_node,
				"mediatek,video-iommu-device")) {
		ret = mtk_ion_get_map_dev(compat->dev,
					  "mediatek,video-iommu-device", 0,
					  MTK_VIDEO_IOVA_BASE, MTK_VIDEO_IOVA_END,
					  "video", &mapping_dev);
		if (ret) {
			/* -EPROBE_DEFER is not a userspace ioctl error. */
			mapping_dev = ERR_PTR(ret == -EPROBE_DEFER ? -EAGAIN : ret);
			goto out_unlock;
		}
		compat->mapping_devs[target] = mapping_dev;
	}
	if (!mapping_dev)
		mapping_dev = ERR_PTR(-ENODEV);

out_unlock:
	mutex_unlock(&compat->state_lock);
	return mapping_dev;
}

static int mtk_ion_get_iova(struct mtk_ion_compat_client *client,
			    struct mtk_ion_get_iova_data *data)
{
	struct mtk_ion_compat_buffer *buffer;
	struct device *mapping_dev;
	dma_addr_t iova;
	size_t mapped_len;
	int target;
	int ret = 0;

	if (data->handle <= 0 || data->module_id < 0)
		return -EINVAL;
	target = mtk_ion_module_target(data->module_id);
	if (target < 0)
		return target;
	mapping_dev = mtk_ion_mapping_dev(client->compat, target);
	if (IS_ERR(mapping_dev))
		return PTR_ERR(mapping_dev);
	ret = mtk_ion_validate_request(data, target);
	if (ret)
		return ret;

	mutex_lock(&client->lock);
	buffer = idr_find(&client->handles, data->handle);
	if (!buffer) {
		ret = -EINVAL;
		goto out_unlock;
	}
	ret = mtk_dma_heap_get_iova(buffer->dmabuf, mapping_dev, &iova,
				    &mapped_len);
	if (ret)
		goto out_unlock;
	ret = mtk_ion_validate_mapping(buffer->dmabuf, mapping_dev, iova,
				       mapped_len);
	if (ret)
		goto out_unlock;
	buffer->module_id = data->module_id;

	data->phy_addr = iova;
	data->len = buffer->dmabuf->size;

out_unlock:
	mutex_unlock(&client->lock);
	return ret;
}

static int mtk_ion_config_buffer(struct mtk_ion_compat_client *client,
				 const struct mtk_ion_get_iova_data *data)
{
	struct mtk_ion_compat_buffer *buffer;
	int ret = 0;

	if (data->handle <= 0 || data->module_id < -1)
		return -EINVAL;
	if (data->security || data->coherent || data->reserve_iova_start ||
	    data->reserve_iova_end)
		return -EOPNOTSUPP;

	mutex_lock(&client->lock);
	buffer = idr_find(&client->handles, data->handle);
	if (!buffer) {
		ret = -EINVAL;
	} else {
		buffer->module_id = data->module_id;
	}
	mutex_unlock(&client->lock);

	return ret;
}

static int mtk_ion_validate_handle(struct mtk_ion_compat_client *client,
				   int handle)
{
	int ret;

	if (handle <= 0)
		return -EINVAL;

	mutex_lock(&client->lock);
	ret = idr_find(&client->handles, handle) ? 0 : -EINVAL;
	mutex_unlock(&client->lock);
	return ret;
}

static bool mtk_ion_cache_range(struct dma_buf *dmabuf, unsigned long va,
				u32 size, unsigned int *offset)
{
	struct vm_area_struct *vma;
	unsigned long start, delta;
	bool valid = false;

	if (!va || !size || !current->mm ||
	    !dmabuf->ops->begin_cpu_access_partial ||
	    !dmabuf->ops->end_cpu_access_partial)
		return false;

	va = untagged_addr(va);
	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, va);
	if (!vma || va < vma->vm_start || size > vma->vm_end - va ||
	    vma->vm_file != dmabuf->file ||
	    vma->vm_pgoff > (dmabuf->size >> PAGE_SHIFT))
		goto out_unlock;
	start = vma->vm_pgoff << PAGE_SHIFT;
	delta = va - vma->vm_start;
	if (delta > dmabuf->size - start)
		goto out_unlock;
	start += delta;
	if (start > UINT_MAX || size > dmabuf->size - start)
		goto out_unlock;
	*offset = start;
	valid = true;
out_unlock:
	mmap_read_unlock(current->mm);
	return valid;
}

static int mtk_ion_cache_begin(struct dma_buf *dmabuf,
			       enum dma_data_direction direction, bool partial,
			       unsigned int offset, unsigned int size)
{
	/*
	 * The legacy ION ioctl performs cache maintenance only; callers order
	 * hardware access separately. Native heap callbacks retain the cache
	 * operations without adding allocation-wide reservation fence waits.
	 * Other exporters keep the generic CPU access contract.
	 */
	if (mtk_dma_heap_is_system_heap(dmabuf)) {
		if (partial)
			return dmabuf->ops->begin_cpu_access_partial(dmabuf,
							 direction, offset, size);
		return dmabuf->ops->begin_cpu_access(dmabuf, direction);
	}

	if (partial)
		return dma_buf_begin_cpu_access_partial(dmabuf, direction, offset, size);
	return dma_buf_begin_cpu_access(dmabuf, direction);
}

static int mtk_ion_cache_end(struct dma_buf *dmabuf,
			     enum dma_data_direction direction, bool partial,
			     unsigned int offset, unsigned int size)
{
	if (partial)
		return dma_buf_end_cpu_access_partial(dmabuf, direction, offset, size);
	return dma_buf_end_cpu_access(dmabuf, direction);
}

static int mtk_ion_cache_sync(struct mtk_ion_compat_client *client,
			      int handle, u32 size, u32 sync_type,
			      unsigned long va)
{
	struct mtk_ion_compat_buffer *buffer;
	struct dma_buf *dmabuf;
	unsigned int offset = 0;
	bool partial;
	int ret;

	if (handle <= 0)
		return -EINVAL;

	mutex_lock(&client->lock);
	buffer = idr_find(&client->handles, handle);
	dmabuf = buffer ? buffer->dmabuf : NULL;
	if (dmabuf)
		get_dma_buf(dmabuf);
	mutex_unlock(&client->lock);
	if (!dmabuf)
		return -EINVAL;

	if (size > dmabuf->size) {
		ret = -EINVAL;
		goto out_put;
	}

	/* Keep the legacy full-buffer fallback for zero/unresolved addresses,
	 * PA/ALL operations and exporters without actual partial callbacks.
	 */
	partial = (sync_type == MTK_ION_CACHE_CLEAN_BY_RANGE ||
		   sync_type == MTK_ION_CACHE_INVALID_BY_RANGE ||
		   sync_type == MTK_ION_CACHE_FLUSH_BY_RANGE) &&
		  mtk_ion_cache_range(dmabuf, va, size, &offset);
	switch (sync_type) {
	case MTK_ION_CACHE_CLEAN_BY_RANGE:
	case MTK_ION_CACHE_CLEAN_BY_RANGE_USE_PA:
	case MTK_ION_CACHE_CLEAN_ALL:
		ret = mtk_ion_cache_end(dmabuf, DMA_TO_DEVICE, partial, offset, size);
		break;
	case MTK_ION_CACHE_INVALID_BY_RANGE:
	case MTK_ION_CACHE_INVALID_BY_RANGE_USE_PA:
	case MTK_ION_CACHE_INVALID_ALL:
		ret = mtk_ion_cache_begin(dmabuf, DMA_FROM_DEVICE, partial, offset, size);
		break;
	case MTK_ION_CACHE_FLUSH_BY_RANGE:
	case MTK_ION_CACHE_FLUSH_BY_RANGE_USE_PA:
	case MTK_ION_CACHE_FLUSH_ALL:
		ret = mtk_ion_cache_end(dmabuf, DMA_BIDIRECTIONAL, partial, offset, size);
		if (!ret)
			ret = mtk_ion_cache_begin(dmabuf, DMA_BIDIRECTIONAL, partial, offset, size);
		break;
	default:
		ret = -EINVAL;
		break;
	}

out_put:
	dma_buf_put(dmabuf);
	return ret;
}

static long mtk_ion_system(struct mtk_ion_compat_client *client,
			   void __user *argp)
{
	struct mtk_ion_sys_data data;

	if (copy_from_user(&data.sys_cmd, argp, sizeof(data.sys_cmd)))
		return -EFAULT;

	if (data.sys_cmd == MTK_ION_SYS_SET_CLIENT_NAME)
		return 0;
	if (data.sys_cmd != MTK_ION_SYS_CACHE_SYNC)
		return -EOPNOTSUPP;
	if (copy_from_user(&data, argp, sizeof(data)))
		return -EFAULT;

	return mtk_ion_cache_sync(client, data.cache_sync.handle,
				  data.cache_sync.size,
				  data.cache_sync.sync_type,
				  (unsigned long)data.cache_sync.va);
}

static long mtk_ion_custom(struct mtk_ion_compat_client *client,
			   void __user *argp)
{
	struct mtk_ion_mm_data mm_data;
	struct ion_custom_data custom;
	void __user *mm_argp;
	int ret;

	if (copy_from_user(&custom, argp, sizeof(custom)))
		return -EFAULT;
	if (!custom.arg)
		return -EOPNOTSUPP;

	mm_argp = (void __user *)custom.arg;
	if (custom.cmd == MTK_ION_CMD_SYSTEM)
		return mtk_ion_system(client, mm_argp);
	if (custom.cmd != MTK_ION_CMD_MULTIMEDIA)
		return -EOPNOTSUPP;

	if (copy_from_user(&mm_data, mm_argp, sizeof(mm_data)))
		return -EFAULT;

	switch (mm_data.mm_cmd) {
	case MTK_ION_MM_CONFIG_BUFFER:
	case MTK_ION_MM_CONFIG_BUFFER_EXT:
		return mtk_ion_config_buffer(client, &mm_data.get_iova);
	case MTK_ION_MM_SET_DEBUG_INFO:
		return mtk_ion_validate_handle(client,
					       mm_data.get_iova.handle);
	case MTK_ION_MM_GET_IOVA:
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = mtk_ion_get_iova(client, &mm_data.get_iova);
	if (ret)
		return ret;

	return copy_to_user(mm_argp, &mm_data, sizeof(mm_data)) ? -EFAULT : 0;
}

static long mtk_ion_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct mtk_ion_compat_client *client = file->private_data;
	void __user *argp = (void __user *)arg;
	struct ion_allocation_data allocation_data;
	struct ion_handle_data handle_data;
	struct ion_fd_data fd_data;
	int ret;

	switch (cmd) {
	case ION_IOC_ALLOC:
		if (copy_from_user(&allocation_data, argp,
				   sizeof(allocation_data)))
			return -EFAULT;
		ret = mtk_ion_alloc(client, &allocation_data);
		if (ret < 0)
			return ret;
		allocation_data.handle = ret;
		if (copy_to_user(argp, &allocation_data,
				 sizeof(allocation_data))) {
			mtk_ion_free(client, ret);
			return -EFAULT;
		}
		return 0;

	case ION_IOC_IMPORT:
		if (copy_from_user(&fd_data, argp, sizeof(fd_data)))
			return -EFAULT;
		ret = mtk_ion_import(client, fd_data.fd);
		if (ret < 0)
			return ret;
		fd_data.handle = ret;
		if (copy_to_user(argp, &fd_data, sizeof(fd_data))) {
			mtk_ion_free(client, ret);
			return -EFAULT;
		}
		return 0;

	case ION_IOC_FREE:
		if (copy_from_user(&handle_data, argp, sizeof(handle_data)))
			return -EFAULT;
		return mtk_ion_free(client, handle_data.handle);

	case ION_IOC_SHARE:
	case ION_IOC_MAP:
		if (copy_from_user(&fd_data, argp, sizeof(fd_data)))
			return -EFAULT;
		ret = mtk_ion_share(client, fd_data.handle);
		if (ret < 0)
			return ret;
		fd_data.fd = ret;
		if (copy_to_user(argp, &fd_data, sizeof(fd_data))) {
			close_fd(ret);
			return -EFAULT;
		}
		return 0;

	case ION_IOC_CUSTOM:
		return mtk_ion_custom(client, argp);

	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
static long mtk_ion_custom32(struct mtk_ion_compat_client *client,
			     void __user *argp)
{
	struct mtk_ion_custom_data32 custom;
	struct mtk_ion_sys_data32 sys_data32;
	struct mtk_ion_mm_data32 mm_data32;
	struct mtk_ion_get_iova_data data;
	void __user *mm_argp;
	int ret;

	if (copy_from_user(&custom, argp, sizeof(custom)))
		return -EFAULT;
	if (!custom.arg)
		return -EOPNOTSUPP;

	mm_argp = compat_ptr(custom.arg);
	if (custom.cmd == MTK_ION_CMD_SYSTEM) {
		if (copy_from_user(&sys_data32.sys_cmd, mm_argp,
				   sizeof(sys_data32.sys_cmd)))
			return -EFAULT;
		if (sys_data32.sys_cmd == MTK_ION_SYS_SET_CLIENT_NAME)
			return 0;
		if (sys_data32.sys_cmd != MTK_ION_SYS_CACHE_SYNC)
			return -EOPNOTSUPP;
		if (copy_from_user(&sys_data32, mm_argp, sizeof(sys_data32)))
			return -EFAULT;
		return mtk_ion_cache_sync(client,
					  sys_data32.cache_sync.handle,
					  sys_data32.cache_sync.size,
					  sys_data32.cache_sync.sync_type,
					  (unsigned long)compat_ptr(sys_data32.cache_sync.va));
	}
	if (custom.cmd != MTK_ION_CMD_MULTIMEDIA)
		return -EOPNOTSUPP;

	if (copy_from_user(&mm_data32, mm_argp, sizeof(mm_data32)))
		return -EFAULT;

	data.handle = mm_data32.get_iova.handle;
	data.module_id = mm_data32.get_iova.module_id;
	data.security = mm_data32.get_iova.security;
	data.coherent = mm_data32.get_iova.coherent;
	data.reserve_iova_start = mm_data32.get_iova.reserve_iova_start;
	data.reserve_iova_end = mm_data32.get_iova.reserve_iova_end;
	data.phy_addr = mm_data32.get_iova.phy_addr;
	data.len = mm_data32.get_iova.len;

	switch (mm_data32.mm_cmd) {
	case MTK_ION_MM_CONFIG_BUFFER:
	case MTK_ION_MM_CONFIG_BUFFER_EXT:
		return mtk_ion_config_buffer(client, &data);
	case MTK_ION_MM_SET_DEBUG_INFO:
		return mtk_ion_validate_handle(client, data.handle);
	case MTK_ION_MM_GET_IOVA:
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = mtk_ion_get_iova(client, &data);
	if (ret)
		return ret;
	if (data.len > U32_MAX)
		return -EOVERFLOW;

	mm_data32.get_iova.phy_addr = data.phy_addr;
	mm_data32.get_iova.len = data.len;
	return copy_to_user(mm_argp, &mm_data32, sizeof(mm_data32)) ?
		-EFAULT : 0;
}

static long mtk_ion_alloc32(struct mtk_ion_compat_client *client,
			    void __user *argp)
{
	struct mtk_ion_allocation_data32 data32;
	struct ion_allocation_data data;
	int ret;

	if (copy_from_user(&data32, argp, sizeof(data32)))
		return -EFAULT;

	data.len = data32.len;
	data.align = data32.align;
	data.heap_id_mask = data32.heap_id_mask;
	data.flags = data32.flags;
	data.handle = data32.handle;
	ret = mtk_ion_alloc(client, &data);
	if (ret < 0)
		return ret;

	data32.handle = ret;
	if (copy_to_user(argp, &data32, sizeof(data32))) {
		mtk_ion_free(client, ret);
		return -EFAULT;
	}
	return 0;
}

static long mtk_ion_compat_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	switch (cmd) {
	case MTK_ION_IOC_ALLOC32:
		return mtk_ion_alloc32(file->private_data, compat_ptr(arg));
	case ION_IOC_IMPORT:
	case ION_IOC_FREE:
	case ION_IOC_SHARE:
	case ION_IOC_MAP:
		return mtk_ion_ioctl(file, cmd,
				      (unsigned long)compat_ptr(arg));
	case MTK_ION_IOC_CUSTOM32:
		return mtk_ion_custom32(file->private_data, compat_ptr(arg));
	default:
		return -ENOTTY;
	}
}
#endif

static int mtk_ion_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct mtk_ion_compat *compat =
		container_of(miscdev, struct mtk_ion_compat, miscdev);
	struct mtk_ion_compat_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	mutex_lock(&compat->state_lock);
	if (!compat->online) {
		mutex_unlock(&compat->state_lock);
		kfree(client);
		return -ENODEV;
	}
	kref_get(&compat->ref);
	mutex_unlock(&compat->state_lock);

	client->compat = compat;
	mutex_init(&client->lock);
	idr_init(&client->handles);
	file->private_data = client;
	return 0;
}

static int mtk_ion_release(struct inode *inode, struct file *file)
{
	struct mtk_ion_compat_client *client = file->private_data;
	struct mtk_ion_compat_buffer *buffer;
	int id = 0;

	mutex_lock(&client->lock);
	while ((buffer = idr_get_next(&client->handles, &id))) {
		idr_remove(&client->handles, id);
		mtk_ion_buffer_release(buffer);
	}
	idr_destroy(&client->handles);
	mutex_unlock(&client->lock);

	kref_put(&client->compat->ref, mtk_ion_compat_put);
	kfree(client);
	return 0;
}

static const struct file_operations mtk_ion_fops = {
	.owner = THIS_MODULE,
	.open = mtk_ion_open,
	.release = mtk_ion_release,
	.unlocked_ioctl = mtk_ion_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mtk_ion_compat_ioctl,
#endif
	.llseek = no_llseek,
};

static int mtk_ion_compat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_ion_compat *compat;
	struct iommu_domain *domain;
	int ret;

	/* Keep these assertions beside the ioctl implementation they protect. */
	static_assert(sizeof(struct ion_allocation_data) == 32);
	static_assert(sizeof(struct ion_fd_data) == 8);
	static_assert(sizeof(struct ion_handle_data) == 4);
	static_assert(sizeof(struct ion_custom_data) == 16);
	static_assert(sizeof(struct mtk_ion_get_iova_data) == 48);
	static_assert(offsetof(struct mtk_ion_get_iova_data, module_id) == 8);
	static_assert(offsetof(struct mtk_ion_get_iova_data, phy_addr) == 32);
	static_assert(offsetof(struct mtk_ion_get_iova_data, len) == 40);
	static_assert(sizeof(struct mtk_ion_mm_data) == 56);
	static_assert(offsetof(struct mtk_ion_mm_data, get_iova) == 8);
	static_assert(sizeof(struct mtk_ion_cache_sync_data) == 32);
	static_assert(offsetof(struct mtk_ion_cache_sync_data, va) == 8);
	static_assert(offsetof(struct mtk_ion_cache_sync_data, iova) == 24);
	static_assert(sizeof(struct mtk_ion_sys_data) == 40);
	static_assert(offsetof(struct mtk_ion_sys_data, cache_sync) == 8);
#ifdef CONFIG_COMPAT
	static_assert(sizeof(struct mtk_ion_allocation_data32) == 20);
	static_assert(sizeof(struct mtk_ion_custom_data32) == 8);
	static_assert(sizeof(struct mtk_ion_get_iova_data32) == 40);
	static_assert(offsetof(struct mtk_ion_get_iova_data32, phy_addr) == 24);
	static_assert(offsetof(struct mtk_ion_get_iova_data32, len) == 32);
	static_assert(sizeof(struct mtk_ion_mm_data32) == 48);
	static_assert(sizeof(struct mtk_ion_cache_sync_data32) == 24);
	static_assert(offsetof(struct mtk_ion_cache_sync_data32, iova) == 16);
	static_assert(sizeof(struct mtk_ion_sys_data32) == 32);
	static_assert(offsetof(struct mtk_ion_sys_data32, cache_sync) == 8);
#endif

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(34));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set 34-bit DMA mask\n");
	ret = dma_set_max_seg_size(dev, UINT_MAX);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set DMA segment size\n");

	domain = iommu_get_domain_for_dev(dev);
	if (!domain)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "waiting for IOMMU DMA domain\n");
	if (mtk_ion_check_aperture(dev, MTK_MDP_IOVA_BASE,
				   MTK_MDP_IOVA_END))
		return dev_err_probe(dev, -EINVAL,
			"unsafe IOMMU aperture %pad-%pad (expected MDP domain)\n",
			&domain->geometry.aperture_start,
			&domain->geometry.aperture_end);

	compat = kzalloc(sizeof(*compat), GFP_KERNEL);
	if (!compat)
		return -ENOMEM;

	compat->dev = get_device(dev);
	compat->mapping_devs[MTK_ION_IOMMU_MDP] = compat->dev;
	compat->miscdev.minor = MISC_DYNAMIC_MINOR;
	compat->miscdev.name = MTK_ION_COMPAT_NAME;
	compat->miscdev.fops = &mtk_ion_fops;
	compat->miscdev.parent = dev;
	mutex_init(&compat->state_lock);
	kref_init(&compat->ref);
	compat->online = true;
	pm_runtime_enable(dev);

	/* Video mappings are initialized on demand, independently of graphics. */
	ret = mtk_ion_get_map_dev(dev, "mediatek,ccu-iommu-devices", 0,
				  MTK_CCU0_IOVA_BASE,
				  MTK_CCU0_IOVA_END,
				  "CCU0",
				  &compat->mapping_devs[MTK_ION_IOMMU_CCU0]);
	if (ret)
		goto err_put;
	ret = mtk_ion_get_map_dev(dev, "mediatek,ccu-iommu-devices", 1,
				  MTK_CCU1_IOVA_BASE,
				  MTK_CCU1_IOVA_END,
				  "CCU1",
				  &compat->mapping_devs[MTK_ION_IOMMU_CCU1]);
	if (ret)
		goto err_put;

	ret = misc_register(&compat->miscdev);
	if (ret)
		goto err_put;

	platform_set_drvdata(pdev, compat);
	dev_info(dev, "legacy ION DMA-BUF import compatibility ready\n");
	return 0;

err_put:
	kref_put(&compat->ref, mtk_ion_compat_put);
	return dev_err_probe(dev, ret, "failed to register legacy ION bridge\n");
}

static void mtk_ion_compat_remove(struct platform_device *pdev)
{
	struct mtk_ion_compat *compat = platform_get_drvdata(pdev);

	mutex_lock(&compat->state_lock);
	compat->online = false;
	mutex_unlock(&compat->state_lock);
	misc_deregister(&compat->miscdev);
	kref_put(&compat->ref, mtk_ion_compat_put);
}

static const struct of_device_id mtk_ion_compat_of_match[] = {
	{ .compatible = "mediatek,mt6877-ion-compat" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_ion_compat_of_match);

static struct platform_driver mtk_ion_compat_driver = {
	.probe = mtk_ion_compat_probe,
	.remove_new = mtk_ion_compat_remove,
	.driver = {
		.name = "mt6877-ion-compat",
		.of_match_table = mtk_ion_compat_of_match,
	},
};

/*
 * The legacy pseudo-M4U driver is disabled with the generic IOMMU driver.
 * Its larb4 DT node therefore needs a driver of its own: the platform bus
 * configures DMA/IOMMU operations only when a driver probes the device.
 * Merely finding that platform device does not give it a DMA domain.
 */
static int mtk_ion_video_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(34));
	if (ret)
		return ret;
	ret = dma_set_max_seg_size(dev, UINT_MAX);
	if (ret)
		return ret;
	ret = mtk_ion_check_aperture(dev, MTK_VIDEO_IOVA_BASE,
				     MTK_VIDEO_IOVA_END);
	if (ret)
		return dev_err_probe(dev, ret, "unsafe video IOMMU aperture\n");

	/* DMA-BUF mappings hold runtime PM references to this device/larb. */
	pm_runtime_enable(dev);
	dev_info(dev, "legacy ION video DMA mapping device ready\n");
	return 0;
}

static void mtk_ion_video_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id mtk_ion_video_of_match[] = {
	{ .compatible = "mediatek,mt6877-ion-video" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_ion_video_of_match);

static struct platform_driver mtk_ion_video_driver = {
	.probe = mtk_ion_video_probe,
	.remove_new = mtk_ion_video_remove,
	.driver = {
		.name = "mt6877-ion-video",
		.of_match_table = mtk_ion_video_of_match,
		.suppress_bind_attrs = true,
	},
};

static struct platform_driver * const mtk_ion_drivers[] = {
	&mtk_ion_video_driver,
	&mtk_ion_compat_driver,
};

static int __init mtk_ion_init(void)
{
	return platform_register_drivers(mtk_ion_drivers,
					 ARRAY_SIZE(mtk_ion_drivers));
}
module_init(mtk_ion_init);

static void __exit mtk_ion_exit(void)
{
	platform_unregister_drivers(mtk_ion_drivers,
				    ARRAY_SIZE(mtk_ion_drivers));
}
module_exit(mtk_ion_exit);

MODULE_DESCRIPTION("MediaTek legacy ION external DMA-BUF compatibility");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(ANDROID_GKI_VFS_EXPORT_ONLY);
MODULE_IMPORT_NS(DMA_BUF);
