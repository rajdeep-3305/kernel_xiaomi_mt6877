// SPDX-License-Identifier: GPL-2.0
/*
 * Ruby APUSYS 1.1 ABI on Linux 6.6.
 * DMA-BUF attachment/vmap/cache operations follow A34x middleware 2.0;
 * keep the Ruby 56-byte memory ioctl and low-32-bit, bank-3 device addresses.
 */
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/iosys-map.h>
#include <linux/log2.h>
#include <linux/mtk_apu_iommu.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include "mdw_mem_cmn.h"

struct mdw_dma_buffer {
	struct dma_buf *dbuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct iosys_map map;
	bool cpu_access;
};

static struct device *mdw_dma_device;

static struct mdw_dma_buffer *mdw_dma_get(struct apusys_kmem *mem)
{
	struct mdw_dma_buffer *buffer;
	struct dma_buf *dbuf;

	if (!mem || mem->cache > 1 ||
	    (mem->align && (!is_power_of_2(mem->align) || mem->align > PAGE_SIZE)))
		return ERR_PTR(-EINVAL);
	if (mem->khandle)
		return (void *)(uintptr_t)mem->khandle;
	dbuf = dma_buf_get(mem->fd);
	if (IS_ERR(dbuf))
		return ERR_CAST(dbuf);
	if (!dbuf->size || dbuf->size > U32_MAX || mem->size > dbuf->size ||
	    !IS_ALIGNED(dbuf->size, PAGE_SIZE)) {
		dma_buf_put(dbuf);
		return ERR_PTR(-EINVAL);
	}
	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		dma_buf_put(dbuf);
		return ERR_PTR(-ENOMEM);
	}
	buffer->dbuf = dbuf;
	mem->khandle = (uintptr_t)buffer;
	return buffer;
}

static void mdw_dma_put_unused(struct apusys_kmem *mem)
{
	struct mdw_dma_buffer *buffer = (void *)(uintptr_t)mem->khandle;

	if (buffer && !buffer->attach && iosys_map_is_null(&buffer->map)) {
		dma_buf_put(buffer->dbuf);
		kfree(buffer);
		mem->khandle = 0;
	}
}

static int mdw_dma_map_kva(struct apusys_kmem *mem)
{
	struct mdw_dma_buffer *buffer = mdw_dma_get(mem);
	int ret;

	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	if (!iosys_map_is_null(&buffer->map))
		return -EBUSY;
	ret = dma_buf_begin_cpu_access(buffer->dbuf, DMA_BIDIRECTIONAL);
	if (ret)
		goto put_buffer;
	buffer->cpu_access = true;
	ret = dma_buf_vmap_unlocked(buffer->dbuf, &buffer->map);
	if (ret)
		goto end_access;
	if (buffer->map.is_iomem || iosys_map_is_null(&buffer->map)) {
		dma_buf_vunmap_unlocked(buffer->dbuf, &buffer->map);
		iosys_map_clear(&buffer->map);
		ret = -ENXIO;
		goto end_access;
	}
	mem->kva = (uintptr_t)buffer->map.vaddr;
	return 0;

end_access:
	dma_buf_end_cpu_access(buffer->dbuf, DMA_BIDIRECTIONAL);
	buffer->cpu_access = false;
put_buffer:
	mdw_dma_put_unused(mem);
	return ret;
}

static int mdw_dma_unmap_kva(struct apusys_kmem *mem)
{
	struct mdw_dma_buffer *buffer = (void *)(uintptr_t)mem->khandle;

	if (!buffer || iosys_map_is_null(&buffer->map))
		return -EINVAL;
	dma_buf_vunmap_unlocked(buffer->dbuf, &buffer->map);
	iosys_map_clear(&buffer->map);
	if (buffer->cpu_access) {
		dma_buf_end_cpu_access(buffer->dbuf, DMA_BIDIRECTIONAL);
		buffer->cpu_access = false;
	}
	mem->kva = 0;
	mdw_dma_put_unused(mem);
	return 0;
}

static int mdw_dma_map_iova(struct apusys_kmem *mem)
{
	struct mdw_dma_buffer *buffer = mdw_dma_get(mem);
	struct scatterlist *sg;
	dma_addr_t iova, end;
	size_t length = 0;
	unsigned int i;
	int ret;

	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	if (buffer->attach)
		return -EBUSY;
	buffer->attach = dma_buf_attach(buffer->dbuf, mdw_dma_device);
	if (IS_ERR(buffer->attach)) {
		ret = PTR_ERR(buffer->attach);
		buffer->attach = NULL;
		goto put_buffer;
	}
	buffer->sgt = dma_buf_map_attachment_unlocked(buffer->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(buffer->sgt)) {
		ret = PTR_ERR(buffer->sgt);
		buffer->sgt = NULL;
		goto detach;
	}
	iova = sg_dma_address(buffer->sgt->sgl);
	end = iova;
	for_each_sgtable_dma_sg(buffer->sgt, sg, i) {
		if (sg_dma_address(sg) != end || !sg_dma_len(sg)) {
			ret = -ERANGE;
			goto unmap;
		}
		length += sg_dma_len(sg);
		end += sg_dma_len(sg);
	}
	/* Hardware consumes low 32 bits with reviser boundary 3. */
	if (upper_32_bits(iova) != 3 || !length || length > U32_MAX ||
	    length < buffer->dbuf->size ||
	    end > MTK_APU_IOVA_BANK + BIT_ULL(32)) {
		ret = -ERANGE;
		goto unmap;
	}
	mem->iova = lower_32_bits(iova);
	mem->iova_size = length;
	return 0;

unmap:
	dma_buf_unmap_attachment_unlocked(buffer->attach, buffer->sgt, DMA_BIDIRECTIONAL);
	buffer->sgt = NULL;
detach:
	dma_buf_detach(buffer->dbuf, buffer->attach);
	buffer->attach = NULL;
put_buffer:
	mdw_dma_put_unused(mem);
	return ret;
}

static int mdw_dma_unmap_iova(struct apusys_kmem *mem)
{
	struct mdw_dma_buffer *buffer = (void *)(uintptr_t)mem->khandle;

	if (!buffer || !buffer->attach)
		return -EINVAL;
	dma_buf_unmap_attachment_unlocked(buffer->attach, buffer->sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(buffer->dbuf, buffer->attach);
	buffer->attach = NULL;
	buffer->sgt = NULL;
	mem->iova = 0;
	mem->iova_size = 0;
	mdw_dma_put_unused(mem);
	return 0;
}

static int mdw_dma_flush(struct apusys_kmem *mem)
{
	struct mdw_dma_buffer *buffer = (void *)(uintptr_t)mem->khandle;
	int ret;

	if (!buffer)
		return -EINVAL;
	/* Legacy CACHE_SYNC must clean CPU writes on every call. */
	ret = dma_buf_end_cpu_access(buffer->dbuf, DMA_BIDIRECTIONAL);
	if (!ret)
		buffer->cpu_access = false;
	return ret;
}

static int mdw_dma_invalidate(struct apusys_kmem *mem)
{
	struct mdw_dma_buffer *buffer = (void *)(uintptr_t)mem->khandle;
	int ret;

	if (!buffer)
		return -EINVAL;
	ret = dma_buf_begin_cpu_access(buffer->dbuf, DMA_BIDIRECTIONAL);
	if (!ret)
		buffer->cpu_access = true;
	return ret;
}

static void mdw_dma_destroy(void)
{
	put_device(mdw_dma_device);
	mdw_dma_device = NULL;
}

static struct mdw_mem_ops mdw_dma_ops = {
	.map_kva = mdw_dma_map_kva,
	.unmap_kva = mdw_dma_unmap_kva,
	.map_iova = mdw_dma_map_iova,
	.unmap_iova = mdw_dma_unmap_iova,
	.flush = mdw_dma_flush,
	.invalidate = mdw_dma_invalidate,
	.destroy = mdw_dma_destroy,
};

struct mdw_mem_ops *mdw_mem_dma_init(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	int ret;

	if (!domain)
		return ERR_PTR(-EPROBE_DEFER);
	if (!domain->geometry.force_aperture ||
	    domain->geometry.aperture_start != MTK_APU_IOVA_BANK ||
	    domain->geometry.aperture_end != MTK_APU_IOVA_BANK + U32_MAX)
		return ERR_PTR(-ERANGE);
	/* The legacy HAL uses one contiguous device address for the whole buffer. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(34));
	if (ret)
		return ERR_PTR(ret);
	ret = dma_set_max_seg_size(dev, U32_MAX);
	if (ret)
		return ERR_PTR(ret);
	mdw_dma_device = get_device(dev);
	return &mdw_dma_ops;
}
