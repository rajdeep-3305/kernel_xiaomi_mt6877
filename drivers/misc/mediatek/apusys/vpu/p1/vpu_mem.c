// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/mtk_apu_iommu.h>
#include <linux/scatterlist.h>
#include <linux/highmem.h>
#include <mt-plat/aee.h>

#include "vpu_cfg.h"
#include "vpu_mem.h"
#include "vpu_debug.h"

static int vpu_map_kva_to_sgt(
	const char *buf, size_t len, struct sg_table *sgt);

static dma_addr_t vpu_map_sg_to_iova(
	struct platform_device *pdev, struct scatterlist *sg,
	unsigned int nents, size_t len, dma_addr_t given_iova);

static void vpu_dump_sg(struct scatterlist *s)
{
	unsigned int i = 0;

	if (!s || !vpu_debug_on(VPU_DBG_MEM))
		return;

	while (s) {
		struct page *p = sg_page(s);
		phys_addr_t phys;

		if (!p)
			break;
		phys = page_to_phys(p);
		pr_info("%s: s[%d]: pfn: %lx, pa: %lx, len: %lx, dma_addr: %lx\n",
			__func__, i,
			(unsigned long) page_to_pfn(p),
			(unsigned long) phys,
			(unsigned long) s->length,
			(unsigned long) s->dma_address);
		s = sg_next(s);
		i++;
	}
}

static void vpu_dump_sgt(struct sg_table *sgt)
{
	if (!sgt || !sgt->sgl)
		return;

	vpu_dump_sg(sgt->sgl);
}

static int
vpu_mem_alloc(struct platform_device *pdev,
	struct vpu_iova *i, dma_addr_t given_iova)
{
	int ret = 0;
	void *kva;
	dma_addr_t iova;

	if (!i) {
		ret = -EINVAL;
		goto out;
	}

	vpu_mem_debug("%s: size: 0x%x, given iova: 0x%llx (%s alloc)\n",
		__func__, i->size, (u64)given_iova,
		(given_iova == VPU_IOVA_END) ? "dynamic" : "static");

	kva = kvmalloc(i->size, GFP_KERNEL);

	if (!kva) {
		dev_info(&pdev->dev, "%s: kvmalloc: failed\n",
			__func__);
		ret = -ENOMEM;
		goto error;
	}

	vpu_mem_debug("%s: kvmalloc: %llx\n", __func__, (uint64_t)kva);

	ret = vpu_map_kva_to_sgt(kva, i->size, &i->sgt);

	if (ret)
		goto error;

	/* This cached allocation has a linear mapping for DMA cache sync. */
	mtk_apu_iommu_sync(&pdev->dev, i->sgt.sgl, i->sgt.nents,
			   DMA_BIDIRECTIONAL, false);
	iova = vpu_map_sg_to_iova(pdev, i->sgt.sgl, i->sgt.nents,
		i->size, given_iova);

	if (!iova) {
		ret = -EIO;
		sg_free_table(&i->sgt);
		i->sgt.sgl = NULL;
		goto error;
	}

	i->mapped_iova = iova;
	i->m.va = (uint64_t)kva;
	i->m.pa = (uint32_t)iova;
	i->m.length = i->size;

	goto out;
error:
	kvfree(kva);
out:
	return ret;
}

void vpu_mem_free(struct vpu_mem *m)
{
	kvfree((void *)m->va);
}

static int
vpu_map_kva_to_sgt(const char *buf, size_t len, struct sg_table *sgt)
{
	struct page **pages = NULL;
	unsigned int nr_pages;
	unsigned int index;
	const char *p;
	int ret;

	vpu_mem_debug("%s: buf: %p, len: %lx, sgt: %p\n",
		__func__, buf, len, sgt);

	nr_pages = DIV_ROUND_UP((unsigned long)buf + len, PAGE_SIZE)
		- ((unsigned long)buf / PAGE_SIZE);
	pages = kmalloc_array(nr_pages, sizeof(struct page *), GFP_KERNEL);

	if (!pages)
		return -ENOMEM;

	p = buf - offset_in_page(buf);

	for (index = 0; index < nr_pages; index++) {
		if (is_vmalloc_addr(p))
			pages[index] = vmalloc_to_page(p);
		else
			pages[index] = kmap_to_page((void *)p);
		if (!pages[index]) {
			pr_info("%s: map failed\n", __func__);
			ret = -EFAULT;
			goto out;
		}
		p += PAGE_SIZE;
	}

	vpu_mem_debug("%s: nr_pages: %d\n", __func__, nr_pages);

	ret = sg_alloc_table_from_pages(sgt, pages, index,
		offset_in_page(buf), len, GFP_KERNEL);

	if (ret) {
		/* sg_alloc_table_from_pages frees its partial allocation on error. */
		sgt->sgl = NULL;
		pr_info("%s: sg_alloc_table_from_pages: %d\n",
			__func__, ret);
		goto out;
	}

	vpu_dump_sgt(sgt);
out:
	kfree(pages);
	return ret;
}

static dma_addr_t
vpu_map_sg_to_iova(
	struct platform_device *pdev, struct scatterlist *sg,
	unsigned int nents, size_t len, dma_addr_t given_iova)
{
	dma_addr_t iova, requested = 0;
	int ret;

	if (given_iova < VPU_IOVA_END)
		requested = given_iova | VPU_IOVA_BANK;
	ret = mtk_apu_iommu_map(&pdev->dev, sg, nents, len, requested, &iova);
	if (ret) {
		dev_err(&pdev->dev, "VPU IOVA map failed: requested=%pad size=%zx err=%d\n",
			&requested, len, ret);
		return 0;
	}
	dev_dbg(&pdev->dev, "VPU IOVA mapped: requested=%pad address=%pad size=%zx\n",
		&requested, &iova, len);
	return iova;
}

static dma_addr_t
vpu_map_to_iova(struct platform_device *pdev, void *addr, size_t len,
	dma_addr_t given_iova, struct sg_table *sgt)
{
	dma_addr_t iova = 0;
	int ret;

	if (!sgt)
		goto out;

	ret = vpu_map_kva_to_sgt(addr, len, sgt);

	if (ret)
		goto out;

	iova = vpu_map_sg_to_iova(pdev, sgt->sgl, sgt->nents, len, given_iova);
out:
	return iova;
}

dma_addr_t vpu_iova_alloc(struct platform_device *pdev,
	struct vpu_iova *i)
{
	int ret = 0;
	dma_addr_t iova = 0;
	/* mt6885, mt6873 maps va to iova */
	unsigned long base = (unsigned long)vpu_drv->bin_va;

	if (!pdev || !i || !i->size)
		goto out;

	iova = i->addr ? i->addr : VPU_IOVA_END;

	i->mapped_iova = 0;
	i->sgt.sgl = NULL;
	i->m.handle = NULL;
	i->m.va = 0;
	i->m.pa = 0;
	i->m.length = 0;

	/* allocate kvm and map */
	if (i->bin == VPU_MEM_ALLOC) {
		ret = vpu_mem_alloc(pdev, i, iova);
		iova = ret ? 0 : i->mapped_iova;
	/* map from vpu firmware loaded at bootloader */
	} else if (i->size) {
		iova = vpu_map_to_iova(pdev,
			(void *)(base + i->bin), i->size, iova,
			&i->sgt);
		i->mapped_iova = iova;
		if (!iova && i->sgt.sgl) {
			sg_free_table(&i->sgt);
			i->sgt.sgl = NULL;
		}
	} else {
		dev_info(&pdev->dev,
			"%s: unknown setting (%x, %x, %x)\n",
			__func__, i->addr, i->bin, i->size);
		iova = 0;
	}

out:
	return iova;
}

void vpu_iova_free(struct device *dev, struct vpu_iova *i)
{
	if (i->mapped_iova) {
		mtk_apu_iommu_unmap(dev, i->mapped_iova, i->size);
		i->mapped_iova = 0;
	}
	if (i->sgt.sgl) {
		sg_free_table(&i->sgt);
		i->sgt.sgl = NULL;
	}
	vpu_mem_free(&i->m);
	i->m.va = 0;
}

void vpu_iova_sync_for_device(struct device *dev, struct vpu_iova *i)
{
	mtk_apu_iommu_sync(dev, i->sgt.sgl, i->sgt.nents, DMA_TO_DEVICE, false);
}

void vpu_iova_sync_for_cpu(struct device *dev, struct vpu_iova *i)
{
	mtk_apu_iommu_sync(dev, i->sgt.sgl, i->sgt.nents, DMA_FROM_DEVICE, true);
}

int vpu_iova_dts(struct platform_device *pdev,
	const char *name, struct vpu_iova *i)
{
	if (of_property_read_u32_array(pdev->dev.of_node,
			name, &i->addr, 3)) {
		dev_info(&pdev->dev, "%s: vpu: unable to get %s\n",
			__func__, name);
		return -ENODEV;
	}

	dev_info(&pdev->dev, "%s: %s: addr: %08xh, size: %08xh, bin: %08xh\n",
		__func__, name, i->addr, i->size, i->bin);

	return 0;
}

void *vpu_vmap(phys_addr_t start, size_t size,
	unsigned int memtype)
{
	struct page **pages = NULL;
	phys_addr_t page_start = 0;
	unsigned int page_count = 0;
	pgprot_t prot;
	unsigned int i;
	void *vaddr = NULL;

	if (!size) {
		pr_info("%s: input size should not be zero\n", __func__);
		return NULL;
	}

	page_start = start - offset_in_page(start);
	page_count = DIV_ROUND_UP(size + offset_in_page(start), PAGE_SIZE);

	if (memtype)
		prot = pgprot_noncached(PAGE_KERNEL);
	else
		prot = pgprot_writecombine(PAGE_KERNEL);

	pages = kmalloc_array(page_count, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return NULL;

	for (i = 0; i < page_count; i++) {
		phys_addr_t addr = page_start + i * PAGE_SIZE;

		pages[i] = pfn_to_page(addr >> PAGE_SHIFT);
	}

	vaddr = vmap(pages, page_count, VM_MAP, prot);
	kfree(pages);
	if (!vaddr) {
		pr_info("%s: failed to get vaddr from vmap\n", __func__);
		return NULL;
	}
	/*
	 * Since vmap() uses page granularity, we must add the offset
	 * into the page here, to get the byte granularity address
	 * into the mapping to represent the actual "start" location.
	 */
	return vaddr + offset_in_page(start);
}
