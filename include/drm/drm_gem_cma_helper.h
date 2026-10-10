/* SPDX-License-Identifier: GPL-2.0 */
/* Compat shim: CMA was renamed to DMA in 6.x */
#ifndef _DRM_GEM_CMA_HELPER_H_
#define _DRM_GEM_CMA_HELPER_H_

#include <drm/drm_gem_dma_helper.h>

#define drm_gem_cma_object		drm_gem_dma_object
#define drm_gem_cma_object_free		drm_gem_dma_object_free
#define drm_gem_cma_dumb_create		drm_gem_dma_dumb_create
#define drm_gem_cma_prime_import_sg_table drm_gem_dma_prime_import_sg_table
#define drm_gem_cma_vmap		drm_gem_dma_vmap
#define drm_gem_cma_vunmap		drm_gem_dma_vunmap
#define drm_gem_cma_mmap		drm_gem_dma_mmap
#define drm_gem_cma_create		drm_gem_dma_create
#define drm_gem_cma_prime_get_sg_table	drm_gem_dma_prime_get_sg_table
#define drm_gem_cma_vm_ops		drm_gem_dma_vm_ops

#endif
