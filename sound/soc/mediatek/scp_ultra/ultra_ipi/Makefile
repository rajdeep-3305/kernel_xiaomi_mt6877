# SPDX-License-Identifier: GPL-2.0

ifeq ($(CONFIG_MTK_ULTRASND_PROXIMITY),y)
ccflags-y += -I$(srctree)/sound/soc/mediatek/common
ccflags-y += -I$(srctree)/sound/soc/mediatek/utility
ccflags-y += -I$(srctree)/drivers/misc/mediatek/scp/include
endif

snd-soc-mtk-scp-ultra-objs := ultra_ipi.o
obj-$(CONFIG_MTK_ULTRASND_PROXIMITY) += snd-soc-mtk-scp-ultra.o
