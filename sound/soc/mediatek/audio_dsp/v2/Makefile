#
# Copyright (C) 2015 MediaTek Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#

# MTK Platform Support
ifeq ($(CONFIG_MACH_MT6893),y)
SCP_PLATFORM := mt6885
MTK_PLATFORM := mt6885
else ifeq ($(CONFIG_MACH_MT6781),y)
SCP_PLATFORM := mt6785
MTK_PLATFORM := mt6781
else
SCP_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))
MTK_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))
endif

# Resolve platform for 6.6 kernels where CONFIG_MTK_PLATFORM may be unset
ifeq ($(MTK_PLATFORM),)
ifeq ($(CONFIG_SND_SOC_MT6877),y)
MTK_PLATFORM := mt6877
SCP_PLATFORM := mt6853
endif
endif

# adsp include path
ifeq ($(CONFIG_SND_SOC_MTK_AUDIO_DSP),y)
subdir-ccflags-y += -I$(srctree)/sound/soc/mediatek/audio_dsp/$(MTK_PLATFORM)
ifeq ($(CONFIG_MACH_MT6779),y)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/audio_ipi/common_v1/framework
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/audio_ipi/common_v1/tasks/audplayback/
else
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/audio_ipi/common_v2/framework
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/audio_ipi/common_v2/tasks/audplayback/
endif
subdir-ccflags-y += -I$(srctree)/sound/soc/mediatek/common
subdir-ccflags-y += -I$(srctree)/sound/soc/mediatek/$(MTK_PLATFORM)/
endif

# adsp driver path — mt6877 has no adsp platform dir; use mt6853 (same family)
ADSP_PLATFORM := $(MTK_PLATFORM)
ifeq ($(ADSP_PLATFORM),mt6877)
ADSP_PLATFORM := mt6853
endif
ifeq ($(CONFIG_MTK_AUDIODSP_SUPPORT),y)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/adsp/$(ADSP_PLATFORM)
endif

ifeq ($(CONFIG_MTK_TINYSYS_SCP_SUPPORT),y)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/scp/$(SCP_PLATFORM)
endif

# platform code — only include if MTK_PLATFORM is set (parent Makefile handles it otherwise)
ifneq ($(MTK_PLATFORM),)
obj-y += ../$(MTK_PLATFORM)/
endif

# spm
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include

# MENU Governor Predict(low power)
ifeq ($(CONFIG_MTK_ACAO_SUPPORT),y)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/mcdi/include/
endif

# common code
obj-$(CONFIG_SND_SOC_MTK_AUDIO_DSP) += audio_buf.o
obj-$(CONFIG_SND_SOC_MTK_AUDIO_DSP) += mtk-dsp-dai.o mtk-dsp-pcm.o mtk-dsp-common.o mtk-dsp-platform-driver.o mtk-dsp-mem-control.o
