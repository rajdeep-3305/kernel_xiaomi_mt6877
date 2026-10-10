// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/dma-direction.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/mtk_apu_iommu.h>
#include <linux/highmem.h>
#include <linux/kmemleak.h>
#include <asm/mman.h>

#include "reviser_cmn.h"
#include "reviser_mem.h"
#include "reviser_ioctl.h"
#include "reviser_reg.h"

static int __reviser_get_sgt(const char *buf,
		size_t len, struct sg_table *sgt)
{
	struct page **pages = NULL;
	unsigned int nr_pages;
	unsigned int index;
	const char *p;
	int ret;

	nr_pages = DIV_ROUND_UP((unsigned long)buf + len, PAGE_SIZE)
		- ((unsigned long)buf / PAGE_SIZE);
	pages = kmalloc_array(nr_pages, sizeof(struct page *), GFP_KERNEL);

	if (!pages)
		return -ENOMEM;

	p = buf - offset_in_page(buf);
	LOG_DEBUG("start p: %llx buf: %llx\n",
			(uint64_t)p, (uint64_t)buf);

	for (index = 0; index < nr_pages; index++) {
		if (is_vmalloc_addr(p))
			pages[index] = vmalloc_to_page(p);
		else
			pages[index] = kmap_to_page((void *)p);
		if (!pages[index]) {
			kfree(pages);
			LOG_ERR("map failed\n");
			return -EFAULT;
		}
		p += PAGE_SIZE;
		//LOG_DEBUG("p: %llx PAGE_SIZE: %llx\n",
		//		(uint64_t)p, (uint64_t)PAGE_SIZE);
	}


	ret = sg_alloc_table_from_pages(sgt, pages, index,
		offset_in_page(buf), len, GFP_KERNEL);
	kfree(pages);
	if (ret) {
		LOG_ERR("sg_alloc_table_from_pages: %d\n", ret);
		return ret;
	}



	LOG_DEBUG("buf: %p, len: %lx, sgt: %p nr_pages: %d\n",
		buf, len, sgt, nr_pages);

	return 0;
}

int reviser_mem_free(struct device *dev, struct reviser_mem *mem)
{
	if (mem->kva) {
		mtk_apu_iommu_unmap(dev, mem->iova | MTK_APU_IOVA_BANK,
				    mem->size);
		sg_free_table(&mem->sgt);
		kvfree((void *)mem->kva);
		mem->kva = 0;
		mem->iova = 0;
	}
	return 0;
}

int reviser_mem_alloc(struct device *dev, struct reviser_mem *mem)
{
	void *kva;
	dma_addr_t iova;
	int ret;

	kva = kvzalloc(mem->size, GFP_KERNEL);
	if (!kva)
		return -ENOMEM;
	ret = __reviser_get_sgt(kva, mem->size, &mem->sgt);
	if (ret)
		goto free_kva;
	mtk_apu_iommu_sync(dev, mem->sgt.sgl, mem->sgt.nents,
			   DMA_BIDIRECTIONAL, false);
	ret = mtk_apu_iommu_map(dev, mem->sgt.sgl, mem->sgt.nents,
			       mem->size, REMAP_DRAM_BASE | MTK_APU_IOVA_BANK,
			       &iova);
	if (ret)
		goto free_sgt;
	mem->kva = (uint64_t)kva;
	mem->iova = lower_32_bits(iova);
	kmemleak_no_scan(kva);
	LOG_DEBUG("mem(0x%x/%d/0x%llx)\n", mem->iova, mem->size, mem->kva);
	return 0;

free_sgt:
	sg_free_table(&mem->sgt);
free_kva:
	kvfree(kva);
	return ret;
}

int reviser_mem_invalidate(struct device *dev, struct reviser_mem *mem)
{
	mtk_apu_iommu_sync(dev, mem->sgt.sgl, mem->sgt.nents, DMA_FROM_DEVICE, true);
	return 0;
}
