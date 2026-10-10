# SPDX-License-Identifier: GPL-2.0

MTK_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))
ifeq ($(MTK_PLATFORM),)
ifeq ($(CONFIG_SND_SOC_MT6877),y)
MTK_PLATFORM := mt6877
endif
endif

ADSP_PLATFORM := $(MTK_PLATFORM)
ifeq ($(ADSP_PLATFORM),mt6877)
ADSP_PLATFORM := mt6853
endif

ifeq ($(CONFIG_SND_SOC_MTK_AUDIO_DSP),y)
subdir-ccflags-y += -I$(srctree)/sound/soc/mediatek/audio_dsp/v2
subdir-ccflags-y += -I$(srctree)/sound/soc/mediatek/audio_dsp/$(MTK_PLATFORM)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/audio_ipi/common_v2/framework
endif

ifeq ($(CONFIG_MTK_AUDIODSP_SUPPORT),y)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/adsp/$(ADSP_PLATFORM)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/adsp/common_v2
endif

# platform driver
snd-soc-mtk-common-objs := mtk-afe-platform-driver.o mtk-afe-fe-dai.o mtk-dsp-sof-common.o mtk-soundcard-driver.o
obj-$(CONFIG_SND_SOC_MEDIATEK) += snd-soc-mtk-common.o
obj-$(CONFIG_SND_SOC_MTK_SRAM) += mtk-sram-manager.o

snd-soc-mtk-sp-common-objs := mtk-sp-pcm-ops.o mtk-afe-debug.o mtk-sp-spk-amp.o
obj-$(CONFIG_SND_SOC_MTK_SMART_PHONE) += snd-soc-mtk-sp-common.o

obj-$(CONFIG_SND_SOC_MTK_BTCVSD) += mtk-btcvsd.o
