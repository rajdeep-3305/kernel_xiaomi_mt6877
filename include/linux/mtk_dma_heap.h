/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MTK_DMA_HEAP_H
#define _LINUX_MTK_DMA_HEAP_H

#include <linux/types.h>

struct device;
struct dma_buf;

bool mtk_dma_heap_is_system_heap(struct dma_buf *dmabuf);
int mtk_dma_heap_set_legacy_manual_sync(struct dma_buf *dmabuf);
int mtk_dma_heap_get_iova(struct dma_buf *dmabuf, struct device *dev,
			  dma_addr_t *iova, size_t *mapped_len);
int mtk_dma_heap_sync_iova(struct dma_buf *dmabuf, struct device *dev,
			   bool for_cpu);

#endif
