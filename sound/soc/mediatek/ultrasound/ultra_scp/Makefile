# SPDX-License-Identifier: GPL-2.0

# MTK Platform Support

# include path
ccflags-y += -I$(srctree)/sound/soc/mediatek/common/
ccflags-y += -I$(srctree)/sound/soc/mediatek/ultrasound/ultra_common

ifneq ($(CONFIG_MTK_TINYSYS_SCP_SUPPORT),)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/scp/include
endif

# common code
snd-soc-mtk-scp-ultra-objs := \
	mtk-scp-ultra-pcm.o \
	mtk-scp-ultra-mem-control.o \
	mtk-scp-ultra-platform-driver.o \
	mtk-scp-ultra-common.o \
	ultra_ipi.o

obj-$(CONFIG_MTK_ULTRASND_PROXIMITY) += snd-soc-mtk-scp-ultra.o
