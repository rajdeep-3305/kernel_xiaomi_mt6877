/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_MTK_APU_IOMMU_H
#define _LINUX_MTK_APU_IOMMU_H

#include <linux/dma-direction.h>
#include <linux/types.h>

struct device;
struct scatterlist;

/* MT6877 firmware stores low 32-bit addresses; reviser selects bank 3. */
#define MTK_APU_IOVA_BANK 0x300000000ULL

/*
 * requested == 0 allocates dynamically; otherwise the exact IOVA is required.
 * Mapping does not transfer CPU cache ownership. Callers sync cached RAM
 * explicitly; bootloader firmware can have no linear mapping for cache sync.
 */
int mtk_apu_iommu_map(struct device *dev, struct scatterlist *sg,
		     unsigned int nents, size_t size, dma_addr_t requested,
		     dma_addr_t *iova);
void mtk_apu_iommu_unmap(struct device *dev, dma_addr_t iova, size_t size);
void mtk_apu_iommu_sync(struct device *dev, struct scatterlist *sg,
		      unsigned int nents, enum dma_data_direction direction,
		      bool for_cpu);

/* Caller holds APU power with shared clocks enabled, before firmware runs. */
int mtk_apu_iommu_prepare(struct device *dev);

#endif
