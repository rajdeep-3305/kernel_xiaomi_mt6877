# SPDX-License-Identifier: GPL-2.0

MTK_PLATFORM := $(subst ",,$(CONFIG_MTK_PLATFORM))
ifeq ($(MTK_PLATFORM),)
MTK_PLATFORM := mt6877
endif

ADSP_PLATFORM := $(MTK_PLATFORM)
ifeq ($(ADSP_PLATFORM),mt6877)
ADSP_PLATFORM := mt6853
endif

# platform driver
snd-soc-mt6877-afe-objs := \
	mt6877-afe-pcm.o \
	mt6877-afe-clk.o \
	mt6877-afe-gpio.o \
	mt6877-dai-adda.o \
	mt6877-afe-control.o \
	mt6877-dai-i2s.o \
	mt6877-dai-hw-gain.o \
	mt6877-dai-src.o \
	mt6877-dai-pcm.o \
	mt6877-dai-hostless.o \
	mt6877-misc-control.o \
	mt6877-reg-autogen.o

obj-$(CONFIG_SND_SOC_MT6877) += snd-soc-mt6877-afe.o

# machine driver
obj-$(CONFIG_SND_SOC_MT6877_MT6359) += mt6877-mt6359.o

# MENU Governor Predict(low power)
ifeq ($(CONFIG_MTK_ACAO_SUPPORT),y)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/mcdi/include/
endif

ifeq ($(CONFIG_MTK_AUDIODSP_SUPPORT),y)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/adsp/$(ADSP_PLATFORM)
ccflags-y += -I$(srctree)/drivers/misc/mediatek/adsp/common_v2
endif

# spm
ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include

# external spk amp
ifeq ($(CONFIG_SND_SOC_AW87339),y)
subdir-ccflags-y += -I$(srctree)/sound/soc/codecs/aw87339/$(MTK_PROJECT)/
endif
