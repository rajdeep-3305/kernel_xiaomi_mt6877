# SPDX-License-Identifier: GPL-2.0
ccflags-y += -I$(src)			# needed for trace events

obj-y					+= mtk_ion/

obj-$(CONFIG_MTK_ION_COMPAT)		+= mtk_ion_compat.o

obj-$(CONFIG_ASHMEM)			+= ashmem.o
obj-$(CONFIG_ANDROID_VSOC)		+= vsoc.o
obj-$(CONFIG_ANDROID_INTF_ALARM_DEV)	+= alarm-dev.o
obj-$(CONFIG_DEBUG_KINFO)		+= debug_kinfo.o

## ION was removed in 6.6; replaced by dmabuf heaps.
## The MTK compatibility driver imports dmabufs; it is not an allocator.
