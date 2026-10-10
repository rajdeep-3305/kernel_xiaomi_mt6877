// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF System heap exporter
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2019, 2020 Linaro Ltd.
 *
 * Portions based off of Andrew Davis' SRAM heap:
 * Copyright (C) 2019 Texas Instruments Incorporated - http://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 */

#include <linux/dma-buf.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/dma-heap.h>
#include <linux/err.h>
#include <linux/highmem.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mtk_dma_heap.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

static struct dma_heap *sys_heap;
static struct dma_heap *sys_uncached_heap;
static struct dma_heap *mtk_mm_heap;
static struct dma_heap *mtk_mm_uncached_heap;
static const struct dma_buf_ops system_heap_buf_ops;

struct system_heap_iova_mapping {
	struct list_head list;
	struct device *dev;
	struct iommu_domain *domain;
	const struct dma_map_ops *dma_ops;
	struct sg_table *table;
	dma_addr_t iova;
	size_t mapped_len;
	unsigned long attrs;
	bool coherent;
};

struct system_heap_buffer {
	struct dma_heap *heap;
	struct list_head attachments;
	struct list_head iova_mappings;
	struct mutex lock;
	unsigned long len;
	struct sg_table sg_table;
	int vmap_cnt;
	void *vaddr;

	bool uncached;
	bool legacy_manual_sync;
};

struct dma_heap_attachment {
	struct device *dev;
	struct sg_table *table;
	struct system_heap_iova_mapping *iova_mapping;
	struct list_head list;
	bool mapped;

	bool uncached;
};

#define LOW_ORDER_GFP (GFP_HIGHUSER | __GFP_ZERO | __GFP_RETRY_MAYFAIL)
#define HIGH_ORDER_GFP  (((GFP_HIGHUSER | __GFP_ZERO | __GFP_NOWARN \
				| __GFP_NORETRY) & ~__GFP_RECLAIM) \
				| __GFP_COMP)
static gfp_t order_flags[] = {HIGH_ORDER_GFP, HIGH_ORDER_GFP, LOW_ORDER_GFP};
/*
 * The selection of the orders used for allocation (1MB, 64K, 4K) is designed
 * to match with the sizes often found in IOMMUs. Using order 4 pages instead
 * of order 0 pages can significantly improve the performance of many IOMMUs
 * by reducing TLB pressure and time spent updating page tables.
 *
 * Note: When the order is 0, the minimum allocation is PAGE_SIZE. The possible
 * page sizes for ARM devices could be 4K, 16K and 64K.
 */
#define ORDER_1M (20 - PAGE_SHIFT)
#define ORDER_64K (16 - PAGE_SHIFT)
#define ORDER_FOR_PAGE_SIZE (0)
static const unsigned int orders[] = {ORDER_1M, ORDER_64K, ORDER_FOR_PAGE_SIZE};

#define NUM_ORDERS ARRAY_SIZE(orders)

static struct sg_table *dup_sg_table(struct sg_table *table)
{
	struct sg_table *new_table;
	int ret, i;
	struct scatterlist *sg, *new_sg;

	new_table = kzalloc(sizeof(*new_table), GFP_KERNEL);
	if (!new_table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(new_table, table->orig_nents, GFP_KERNEL);
	if (ret) {
		kfree(new_table);
		return ERR_PTR(-ENOMEM);
	}

	new_sg = new_table->sgl;
	for_each_sgtable_sg(table, sg, i) {
		sg_set_page(new_sg, sg_page(sg), sg->length, sg->offset);
		new_sg = sg_next(new_sg);
	}

	return new_table;
}

static struct iommu_domain *
system_heap_mapping_domain(struct system_heap_buffer *buffer, struct device *dev)
{
	struct iommu_domain *domain;

	if (buffer->heap != mtk_mm_heap && buffer->heap != mtk_mm_uncached_heap)
		return NULL;
	domain = iommu_get_domain_for_dev(dev);
	return domain && iommu_is_dma_domain(domain) ? domain : NULL;
}

/* A shared IOVA must also satisfy the importing device's DMA constraints. */
static bool system_heap_iova_compatible(struct system_heap_iova_mapping *mapping,
				       struct device *dev)
{
	u64 limit = min_not_zero(dma_get_mask(dev), dev->bus_dma_limit);
	unsigned long boundary = dma_get_seg_boundary(dev);
	struct scatterlist *sg;
	unsigned int i;

	if (mapping->iova > limit ||
	    mapping->mapped_len - 1 > limit - mapping->iova)
		return false;
	for_each_sgtable_dma_sg(mapping->table, sg, i) {
		dma_addr_t start = sg_dma_address(sg);
		dma_addr_t end = start + sg_dma_len(sg) - 1;

		if (sg_dma_is_swiotlb(sg) || sg_dma_is_bus_address(sg) ||
		    sg_dma_len(sg) > dma_get_max_seg_size(dev) ||
		    (start & ~boundary) != (end & ~boundary))
			return false;
	}
	return true;
}

static struct system_heap_iova_mapping *
system_heap_find_iova_mapping(struct system_heap_buffer *buffer,
			     struct device *dev, unsigned long attrs)
{
	struct iommu_domain *domain = system_heap_mapping_domain(buffer, dev);
	struct system_heap_iova_mapping *mapping;

	list_for_each_entry(mapping, &buffer->iova_mappings, list) {
		if ((mapping->attrs ^ attrs) & ~DMA_ATTR_SKIP_CPU_SYNC)
			continue;
		if (!domain) {
			if (mapping->dev == dev)
				return mapping;
			continue;
		}
		if (mapping->domain == domain &&
		    mapping->dma_ops == get_dma_ops(dev) &&
		    mapping->coherent == dev_is_dma_coherent(dev) &&
		    system_heap_iova_compatible(mapping, dev))
			return mapping;
	}
	return NULL;
}

static void
system_heap_free_iova_mapping(struct system_heap_iova_mapping *mapping)
{
	dma_unmap_sgtable(mapping->dev, mapping->table, DMA_BIDIRECTIONAL,
			  mapping->attrs);
	if (!mapping->domain)
		pm_runtime_put_sync_suspend(mapping->dev);
	put_device(mapping->dev);
	sg_free_table(mapping->table);
	kfree(mapping->table);
	kfree(mapping);
}

static int system_heap_create_iova_mapping(struct system_heap_buffer *buffer,
					   struct device *dev,
					   unsigned long attrs,
					   struct system_heap_iova_mapping **out)
{
	struct system_heap_iova_mapping *mapping;
	struct scatterlist *sg;
	dma_addr_t base, expected;
	size_t total = 0;
	unsigned int i;
	int ret;

	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (!mapping)
		return -ENOMEM;

	mapping->table = dup_sg_table(&buffer->sg_table);
	if (IS_ERR(mapping->table)) {
		ret = PTR_ERR(mapping->table);
		mapping->table = NULL;
		goto err_free;
	}
	mapping->domain = system_heap_mapping_domain(buffer, dev);
	mapping->dma_ops = get_dma_ops(dev);
	mapping->coherent = dev_is_dma_coherent(dev);
	mapping->attrs = attrs;
	if (buffer->uncached || mapping->domain)
		mapping->attrs |= DMA_ATTR_SKIP_CPU_SYNC;

	/*
	 * MTK DMA domains retain their software page tables while powered
	 * down, and their TLB callbacks handle inactive hardware. Leave power
	 * ownership with the importer instead of pinning its larb awake for
	 * the buffer's life, including importers without runtime PM enabled.
	 */
	if (!mapping->domain) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			goto err_table;
	}

	ret = dma_map_sgtable(dev, mapping->table, DMA_BIDIRECTIONAL,
			      mapping->attrs);
	if (ret)
		goto err_pm;

	base = sg_dma_address(mapping->table->sgl);
	for_each_sgtable_dma_sg(mapping->table, sg, i) {
		expected = base + total;
		if (expected < base || sg_dma_address(sg) != expected) {
			ret = -ERANGE;
			goto err_unmap;
		}
		if (sg_dma_len(sg) > SIZE_MAX - total) {
			ret = -EOVERFLOW;
			goto err_unmap;
		}
		total += sg_dma_len(sg);
	}
	if (!total || total < buffer->len) {
		ret = -ERANGE;
		goto err_unmap;
	}

	mapping->dev = get_device(dev);
	mapping->iova = base;
	mapping->mapped_len = total;
	if (mapping->domain && !system_heap_iova_compatible(mapping, dev)) {
		put_device(mapping->dev);
		ret = -ERANGE;
		goto err_unmap;
	}
	*out = mapping;
	return 0;

err_unmap:
	dma_unmap_sgtable(dev, mapping->table, DMA_BIDIRECTIONAL,
			  mapping->attrs);
err_pm:
	if (!mapping->domain)
		pm_runtime_put_sync_suspend(dev);
err_table:
	sg_free_table(mapping->table);
	kfree(mapping->table);
err_free:
	kfree(mapping);
	return ret;
}

/* Caller holds buffer->lock; attachment and legacy ION users share this cache. */
static int system_heap_get_iova_mapping(struct system_heap_buffer *buffer,
				       struct device *dev, unsigned long attrs,
				       struct system_heap_iova_mapping **out,
				       bool *created)
{
	struct system_heap_iova_mapping *mapping;
	int ret;

	*created = false;
	mapping = system_heap_find_iova_mapping(buffer, dev, attrs);
	if (!mapping) {
		ret = system_heap_create_iova_mapping(buffer, dev, attrs, &mapping);
		if (ret)
			return ret;
		list_add_tail(&mapping->list, &buffer->iova_mappings);
		*created = true;
	}
	*out = mapping;
	return 0;
}

bool mtk_dma_heap_is_system_heap(struct dma_buf *dmabuf)
{
	return dmabuf && dmabuf->ops == &system_heap_buf_ops;
}
EXPORT_SYMBOL_GPL(mtk_dma_heap_is_system_heap);

/* Called only by the legacy allocator, before publishing a new buffer. */
int mtk_dma_heap_set_legacy_manual_sync(struct dma_buf *dmabuf)
{
	struct system_heap_buffer *buffer;
	struct device *dev;
	int ret = 0;

	if (!mtk_dma_heap_is_system_heap(dmabuf))
		return -EXDEV;
	buffer = dmabuf->priv;
	if (buffer->heap != mtk_mm_heap || buffer->uncached)
		return -EINVAL;

	mutex_lock(&buffer->lock);
	if (!list_empty(&buffer->attachments) ||
	    !list_empty(&buffer->iova_mappings) || buffer->vmap_cnt) {
		ret = -EBUSY;
		goto out_unlock;
	}
	if (buffer->legacy_manual_sync)
		goto out_unlock;

	/* Publish the allocator's CPU-written zeroes before skipping map sync. */
	dev = dma_heap_get_dev(buffer->heap);
	ret = dma_map_sgtable(dev, &buffer->sg_table, DMA_BIDIRECTIONAL, 0);
	if (ret)
		goto out_unlock;
	dma_unmap_sgtable(dev, &buffer->sg_table, DMA_BIDIRECTIONAL, 0);
	buffer->legacy_manual_sync = true;
out_unlock:
	mutex_unlock(&buffer->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_dma_heap_set_legacy_manual_sync);

int mtk_dma_heap_get_iova(struct dma_buf *dmabuf, struct device *dev,
			  dma_addr_t *iova, size_t *mapped_len)
{
	struct system_heap_iova_mapping *mapping;
	struct system_heap_buffer *buffer;
	bool created;
	int ret = 0;

	if (!dmabuf || !dev || !iova || !mapped_len)
		return -EINVAL;
	if (dmabuf->ops != &system_heap_buf_ops)
		return -EXDEV;

	buffer = dmabuf->priv;
	mutex_lock(&buffer->lock);
	ret = system_heap_get_iova_mapping(buffer, dev, 0, &mapping, &created);
	if (ret)
		goto out_unlock;

	if (created && mapping->domain && !buffer->uncached &&
	    !buffer->legacy_manual_sync)
		dma_sync_sgtable_for_device(dev, mapping->table, DMA_BIDIRECTIONAL);
	*iova = mapping->iova;
	*mapped_len = mapping->mapped_len;
out_unlock:
	mutex_unlock(&buffer->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_dma_heap_get_iova);

/* Legacy manual-sync ION imports retain device ownership across MDP jobs. */
int mtk_dma_heap_sync_iova(struct dma_buf *dmabuf, struct device *dev,
			   bool for_cpu)
{
	struct system_heap_iova_mapping *mapping;
	struct system_heap_buffer *buffer;
	int ret = -ENOENT;

	if (!dmabuf || !dev)
		return -EINVAL;
	if (dmabuf->ops != &system_heap_buf_ops)
		return -EXDEV;

	buffer = dmabuf->priv;
	mutex_lock(&buffer->lock);
	mapping = system_heap_find_iova_mapping(buffer, dev, 0);
	if (mapping) {
		if (!buffer->uncached &&
		    !(buffer->legacy_manual_sync && mapping->domain)) {
			if (for_cpu)
				dma_sync_sgtable_for_cpu(dev, mapping->table,
							 DMA_BIDIRECTIONAL);
			else
				dma_sync_sgtable_for_device(dev, mapping->table,
							    DMA_BIDIRECTIONAL);
		}
		ret = 0;
	}
	mutex_unlock(&buffer->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_dma_heap_sync_iova);

static int system_heap_attach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attachment)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;
	struct sg_table *table;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	table = dup_sg_table(&buffer->sg_table);
	if (IS_ERR(table)) {
		kfree(a);
		return -ENOMEM;
	}

	a->table = table;
	a->dev = attachment->dev;
	INIT_LIST_HEAD(&a->list);
	a->mapped = false;
	a->uncached = buffer->uncached;
	attachment->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void system_heap_detach(struct dma_buf *dmabuf,
			       struct dma_buf_attachment *attachment)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a = attachment->priv;

	mutex_lock(&buffer->lock);
	list_del(&a->list);
	mutex_unlock(&buffer->lock);

	sg_free_table(a->table);
	kfree(a->table);
	kfree(a);
}

static struct sg_table *system_heap_map_dma_buf(struct dma_buf_attachment *attachment,
						enum dma_data_direction direction)
{
	struct system_heap_buffer *buffer = attachment->dmabuf->priv;
	struct dma_heap_attachment *a = attachment->priv;
	struct sg_table *table = a->table;
	struct system_heap_iova_mapping *mapping;
	struct scatterlist *sg, *mapped_sg;
	unsigned long attr = attachment->dma_map_attrs;
	bool created;
	unsigned int i;
	int ret;

	if (a->uncached)
		attr |= DMA_ATTR_SKIP_CPU_SYNC;

	mutex_lock(&buffer->lock);
	if (system_heap_mapping_domain(buffer, attachment->dev)) {
		ret = system_heap_get_iova_mapping(buffer, attachment->dev, attr,
						   &mapping, &created);
		if (ret && ret != -ERANGE)
			goto out_unlock;
		if (!ret) {
			/* Keep attachment tables private, including their DMA metadata. */
			mapped_sg = mapping->table->sgl;
			for_each_sgtable_sg(table, sg, i) {
				sg_dma_address(sg) = sg_dma_address(mapped_sg);
				sg_dma_len(sg) = sg_dma_len(mapped_sg);
				mapped_sg = sg_next(mapped_sg);
			}
			table->nents = mapping->table->nents;
			a->iova_mapping = mapping;
			a->mapped = true;
			if (!(attr & DMA_ATTR_SKIP_CPU_SYNC) &&
			    !buffer->legacy_manual_sync)
				dma_sync_sgtable_for_device(attachment->dev, table,
							    direction);
			goto out_unlock;
		}
	}
	a->iova_mapping = NULL;
	ret = dma_map_sgtable(attachment->dev, table, direction, attr);
	if (!ret)
		a->mapped = true;
out_unlock:
	mutex_unlock(&buffer->lock);
	return ret ? ERR_PTR(ret) : table;
}

static void system_heap_unmap_dma_buf(struct dma_buf_attachment *attachment,
				      struct sg_table *table,
				      enum dma_data_direction direction)
{
	struct system_heap_buffer *buffer = attachment->dmabuf->priv;
	struct dma_heap_attachment *a = attachment->priv;
	unsigned long attr = attachment->dma_map_attrs;

	if (a->uncached)
		attr |= DMA_ATTR_SKIP_CPU_SYNC;
	mutex_lock(&buffer->lock);
	a->mapped = false;
	if (a->iova_mapping) {
		if (!(attr & DMA_ATTR_SKIP_CPU_SYNC) &&
		    !buffer->legacy_manual_sync)
			dma_sync_sgtable_for_cpu(attachment->dev, table, direction);
	} else {
		dma_unmap_sgtable(attachment->dev, table, direction, attr);
	}
	mutex_unlock(&buffer->lock);
}

/* All tables below describe the same buffer's original physical SG extents. */
static void system_heap_sync_sgtable(struct device *dev, struct sg_table *table,
				     enum dma_data_direction direction,
				     bool for_cpu, bool *synced)
{
	bool cache_only = iommu_dma_sync_sgtable_is_cache_only(dev, table);

	if (cache_only && *synced)
		return;

	if (for_cpu)
		dma_sync_sgtable_for_cpu(dev, table, direction);
	else
		dma_sync_sgtable_for_device(dev, table, direction);
	if (cache_only)
		*synced = true;
}

static int system_heap_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
						enum dma_data_direction direction)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;
	struct system_heap_iova_mapping *mapping;
	bool synced = false;

	mutex_lock(&buffer->lock);

	if (buffer->vmap_cnt)
		invalidate_kernel_vmap_range(buffer->vaddr, buffer->len);

	if (!buffer->uncached) {
		list_for_each_entry(a, &buffer->attachments, list) {
			if (!a->mapped || a->iova_mapping)
				continue;
			system_heap_sync_sgtable(a->dev, a->table, direction,
						 true, &synced);
		}
		list_for_each_entry(mapping, &buffer->iova_mappings, list)
			system_heap_sync_sgtable(mapping->dev, mapping->table,
						 direction, true, &synced);
	}
	mutex_unlock(&buffer->lock);

	return 0;
}

static int system_heap_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
					      enum dma_data_direction direction)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	struct dma_heap_attachment *a;
	struct system_heap_iova_mapping *mapping;
	bool synced = false;

	mutex_lock(&buffer->lock);

	if (buffer->vmap_cnt)
		flush_kernel_vmap_range(buffer->vaddr, buffer->len);

	if (!buffer->uncached) {
		list_for_each_entry(a, &buffer->attachments, list) {
			if (!a->mapped || a->iova_mapping)
				continue;
			system_heap_sync_sgtable(a->dev, a->table, direction,
						 false, &synced);
		}
		list_for_each_entry(mapping, &buffer->iova_mappings, list)
			system_heap_sync_sgtable(mapping->dev, mapping->table,
						 direction, false, &synced);
	}
	mutex_unlock(&buffer->lock);

	return 0;
}

static int system_heap_sync_range(struct device *dev, struct sg_table *table,
				 enum dma_data_direction direction,
				 unsigned int offset, unsigned int len,
				 bool for_cpu, bool *synced)
{
	bool cache_only = iommu_dma_sync_sgtable_is_cache_only(dev, table);
	int ret;

	if (cache_only && *synced)
		return 0;

	/*
	 * Match the IOMMU SG sync path using the known physical extents. A
	 * single-range sync instead depends on a live IOVA-to-PA translation,
	 * which is unnecessary for normal SG mappings and can yield PA zero.
	 * Other DMA backends need the complete original table and metadata.
	 */
	ret = iommu_dma_sync_sgtable_range(dev, table, direction, offset, len,
					   for_cpu);
	if (ret && ret != -EOPNOTSUPP)
		return ret;

	if (ret == -EOPNOTSUPP) {
		if (for_cpu)
			dma_sync_sgtable_for_cpu(dev, table, direction);
		else
			dma_sync_sgtable_for_device(dev, table, direction);
	}
	if (cache_only)
		*synced = true;
	return 0;
}

static int system_heap_dma_buf_cpu_access_partial(struct dma_buf *dmabuf,
					 enum dma_data_direction direction,
					 unsigned int offset, unsigned int len,
					 bool for_cpu)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	struct system_heap_iova_mapping *mapping;
	struct dma_heap_attachment *a;
	bool synced = false;
	int ret = 0;

	if (!len || offset >= buffer->len || len > buffer->len - offset)
		return -EINVAL;

	mutex_lock(&buffer->lock);
	if (buffer->vmap_cnt) {
		void *vaddr = (char *)buffer->vaddr + offset;

		if (for_cpu)
			invalidate_kernel_vmap_range(vaddr, len);
		else
			flush_kernel_vmap_range(vaddr, len);
	}
	if (!buffer->uncached) {
		list_for_each_entry(a, &buffer->attachments, list) {
			if (!a->mapped || a->iova_mapping)
				continue;
			ret = system_heap_sync_range(a->dev, a->table,
						    direction, offset, len, for_cpu,
						    &synced);
			if (ret)
				goto out_unlock;
		}
		list_for_each_entry(mapping, &buffer->iova_mappings, list) {
			ret = system_heap_sync_range(mapping->dev, mapping->table,
						    direction, offset, len, for_cpu,
						    &synced);
			if (ret)
				goto out_unlock;
		}
	}
out_unlock:
	mutex_unlock(&buffer->lock);
	return ret;
}

static int system_heap_dma_buf_begin_cpu_access_partial(struct dma_buf *dmabuf,
					 enum dma_data_direction direction,
					 unsigned int offset, unsigned int len)
{
	return system_heap_dma_buf_cpu_access_partial(dmabuf, direction,
						     offset, len, true);
}

static int system_heap_dma_buf_end_cpu_access_partial(struct dma_buf *dmabuf,
				       enum dma_data_direction direction,
				       unsigned int offset, unsigned int len)
{
	return system_heap_dma_buf_cpu_access_partial(dmabuf, direction,
						     offset, len, false);
}

static int system_heap_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	struct sg_table *table = &buffer->sg_table;
	unsigned long addr = vma->vm_start;
	struct sg_page_iter piter;
	int ret;

	if (buffer->uncached)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	for_each_sgtable_page(table, &piter, vma->vm_pgoff) {
		struct page *page = sg_page_iter_page(&piter);

		ret = remap_pfn_range(vma, addr, page_to_pfn(page), PAGE_SIZE,
				      vma->vm_page_prot);
		if (ret)
			return ret;
		addr += PAGE_SIZE;
		if (addr >= vma->vm_end)
			return 0;
	}
	return 0;
}

static void *system_heap_do_vmap(struct system_heap_buffer *buffer)
{
	struct sg_table *table = &buffer->sg_table;
	int npages = PAGE_ALIGN(buffer->len) / PAGE_SIZE;
	struct page **pages = vmalloc(sizeof(struct page *) * npages);
	struct page **tmp = pages;
	struct sg_page_iter piter;
	pgprot_t pgprot = PAGE_KERNEL;
	void *vaddr;

	if (!pages)
		return ERR_PTR(-ENOMEM);

	if (buffer->uncached)
		pgprot = pgprot_writecombine(PAGE_KERNEL);

	for_each_sgtable_page(table, &piter, 0) {
		WARN_ON(tmp - pages >= npages);
		*tmp++ = sg_page_iter_page(&piter);
	}

	vaddr = vmap(pages, npages, VM_MAP, pgprot);
	vfree(pages);

	if (!vaddr)
		return ERR_PTR(-ENOMEM);

	return vaddr;
}

static int system_heap_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	void *vaddr;
	int ret = 0;

	mutex_lock(&buffer->lock);
	if (buffer->vmap_cnt) {
		buffer->vmap_cnt++;
		iosys_map_set_vaddr(map, buffer->vaddr);
		goto out;
	}

	vaddr = system_heap_do_vmap(buffer);
	if (IS_ERR(vaddr)) {
		ret = PTR_ERR(vaddr);
		goto out;
	}

	buffer->vaddr = vaddr;
	buffer->vmap_cnt++;
	iosys_map_set_vaddr(map, buffer->vaddr);
out:
	mutex_unlock(&buffer->lock);

	return ret;
}

static void system_heap_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct system_heap_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	if (!--buffer->vmap_cnt) {
		vunmap(buffer->vaddr);
		buffer->vaddr = NULL;
	}
	mutex_unlock(&buffer->lock);
	iosys_map_clear(map);
}

static void system_heap_dma_buf_release(struct dma_buf *dmabuf)
{
	struct system_heap_buffer *buffer = dmabuf->priv;
	struct system_heap_iova_mapping *mapping, *tmp_mapping;
	struct sg_table *table;
	struct scatterlist *sg;
	int i;

	list_for_each_entry_safe(mapping, tmp_mapping,
				 &buffer->iova_mappings, list) {
		list_del(&mapping->list);
		system_heap_free_iova_mapping(mapping);
	}

	table = &buffer->sg_table;
	for_each_sgtable_sg(table, sg, i) {
		struct page *page = sg_page(sg);

		__free_pages(page, compound_order(page));
	}
	sg_free_table(table);
	kfree(buffer);
}

static const struct dma_buf_ops system_heap_buf_ops = {
	.attach = system_heap_attach,
	.detach = system_heap_detach,
	.map_dma_buf = system_heap_map_dma_buf,
	.unmap_dma_buf = system_heap_unmap_dma_buf,
	.begin_cpu_access = system_heap_dma_buf_begin_cpu_access,
	.end_cpu_access = system_heap_dma_buf_end_cpu_access,
	.begin_cpu_access_partial = system_heap_dma_buf_begin_cpu_access_partial,
	.end_cpu_access_partial = system_heap_dma_buf_end_cpu_access_partial,
	.mmap = system_heap_mmap,
	.vmap = system_heap_vmap,
	.vunmap = system_heap_vunmap,
	.release = system_heap_dma_buf_release,
};

static struct page *alloc_largest_available(unsigned long size,
					    unsigned int max_order)
{
	struct page *page;
	int i;

	for (i = 0; i < NUM_ORDERS; i++) {
		if (size < (PAGE_SIZE << orders[i]))
			continue;
		if (max_order < orders[i])
			continue;

		page = alloc_pages(order_flags[i], orders[i]);
		if (!page)
			continue;
		return page;
	}
	return NULL;
}

static struct dma_buf *system_heap_do_allocate(struct dma_heap *heap,
					       unsigned long len,
					       u32 fd_flags,
					       u64 heap_flags,
					       bool uncached)
{
	struct system_heap_buffer *buffer;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	unsigned long size_remaining = len;
	unsigned int max_order = orders[0];
	struct dma_buf *dmabuf;
	struct sg_table *table;
	struct scatterlist *sg;
	struct list_head pages;
	struct page *page, *tmp_page;
	int i, ret = -ENOMEM;

	if (len / PAGE_SIZE > totalram_pages())
		return ERR_PTR(-ENOMEM);

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&buffer->attachments);
	INIT_LIST_HEAD(&buffer->iova_mappings);
	mutex_init(&buffer->lock);
	buffer->heap = heap;
	buffer->len = len;
	buffer->uncached = uncached;

	INIT_LIST_HEAD(&pages);
	i = 0;
	while (size_remaining > 0) {
		/*
		 * Avoid trying to allocate memory if the process
		 * has been killed by SIGKILL
		 */
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			goto free_buffer;
		}

		page = alloc_largest_available(size_remaining, max_order);
		if (!page)
			goto free_buffer;

		list_add_tail(&page->lru, &pages);
		size_remaining -= page_size(page);
		max_order = compound_order(page);
		i++;
	}

	table = &buffer->sg_table;
	if (sg_alloc_table(table, i, GFP_KERNEL))
		goto free_buffer;

	sg = table->sgl;
	list_for_each_entry_safe(page, tmp_page, &pages, lru) {
		sg_set_page(sg, page, page_size(page), 0);
		sg = sg_next(sg);
		list_del(&page->lru);
	}

	/* create the dmabuf */
	exp_info.exp_name = dma_heap_get_name(heap);
	exp_info.ops = &system_heap_buf_ops;
	exp_info.size = buffer->len;
	exp_info.flags = fd_flags;
	exp_info.priv = buffer;
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto free_pages;
	}

	/*
	 * For uncached buffers, we need to initially flush cpu cache, since
	 * the __GFP_ZERO on the allocation means the zeroing was done by the
	 * cpu and thus it is likely cached. Map (and implicitly flush) and
	 * unmap it now so we don't get corruption later on.
	 */
	if (buffer->uncached) {
		dma_map_sgtable(dma_heap_get_dev(heap), table, DMA_BIDIRECTIONAL, 0);
		dma_unmap_sgtable(dma_heap_get_dev(heap), table, DMA_BIDIRECTIONAL, 0);
	}

	return dmabuf;

free_pages:
	for_each_sgtable_sg(table, sg, i) {
		struct page *p = sg_page(sg);

		__free_pages(p, compound_order(p));
	}
	sg_free_table(table);
free_buffer:
	list_for_each_entry_safe(page, tmp_page, &pages, lru)
		__free_pages(page, compound_order(page));
	kfree(buffer);

	return ERR_PTR(ret);
}

static struct dma_buf *system_heap_allocate(struct dma_heap *heap,
					    unsigned long len,
					    u32 fd_flags,
					    u64 heap_flags)
{
	return system_heap_do_allocate(heap, len, fd_flags, heap_flags, false);
}

static const struct dma_heap_ops system_heap_ops = {
	.allocate = system_heap_allocate,
};

static struct dma_buf *system_uncached_heap_allocate(struct dma_heap *heap,
						     unsigned long len,
						     u32 fd_flags,
						     u64 heap_flags)
{
	return system_heap_do_allocate(heap, len, fd_flags, heap_flags, true);
}

/* Dummy function to be used until we can call coerce_mask_and_coherent */
static struct dma_buf *system_uncached_heap_not_initialized(struct dma_heap *heap,
							    unsigned long len,
							    u32 fd_flags,
							    u64 heap_flags)
{
	return ERR_PTR(-EBUSY);
}

static struct dma_heap_ops system_uncached_heap_ops = {
	/* After system_heap_create is complete, we will swap this */
	.allocate = system_uncached_heap_not_initialized,
};

static int system_heap_create(void)
{
	struct dma_heap_export_info exp_info;
	struct device *heap_dev;
	int ret;

	exp_info.name = "system";
	exp_info.ops = &system_heap_ops;
	exp_info.priv = NULL;

	sys_heap = dma_heap_add(&exp_info);
	if (IS_ERR(sys_heap))
		return PTR_ERR(sys_heap);

	exp_info.name = "system-uncached";
	exp_info.ops = &system_uncached_heap_ops;
	exp_info.priv = NULL;

	sys_uncached_heap = dma_heap_add(&exp_info);
	if (IS_ERR(sys_uncached_heap))
		return PTR_ERR(sys_uncached_heap);

	/* Legacy MediaTek userspace selects these heaps by name. */
	exp_info.name = "mtk_mm";
	exp_info.ops = &system_heap_ops;
	mtk_mm_heap = dma_heap_add(&exp_info);
	if (IS_ERR(mtk_mm_heap))
		return PTR_ERR(mtk_mm_heap);
	heap_dev = dma_heap_get_dev(mtk_mm_heap);
	ret = dma_coerce_mask_and_coherent(heap_dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	exp_info.name = "mtk_mm-uncached";
	exp_info.ops = &system_uncached_heap_ops;
	mtk_mm_uncached_heap = dma_heap_add(&exp_info);
	if (IS_ERR(mtk_mm_uncached_heap))
		return PTR_ERR(mtk_mm_uncached_heap);

	heap_dev = dma_heap_get_dev(sys_uncached_heap);
	ret = dma_coerce_mask_and_coherent(heap_dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	heap_dev = dma_heap_get_dev(mtk_mm_uncached_heap);
	ret = dma_coerce_mask_and_coherent(heap_dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	mb(); /* make sure we only set allocate after dma_mask is set */
	system_uncached_heap_ops.allocate = system_uncached_heap_allocate;

	return 0;
}
module_init(system_heap_create);
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(DMA_BUF);
