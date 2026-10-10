# SPDX-License-Identifier: GPL-2.0
export DEVICE_MODULES_PATH ?= $(srctree)
subdir-ccflags-y := -Werror $(call cc-disable-warning, unused-but-set-variable)

# scp smartpa related code
obj-$(CONFIG_MTK_AUDIO_SCP_SPKPROTECT_SUPPORT) += scp_spk/
obj-$(CONFIG_MTK_AUDIO_SCP_SPKPROTECT_SUPPORT) += utility/

# scp vow bargein related code
obj-$(CONFIG_MTK_VOW_SUPPORT) += scp_vow/

# scp ultra sound related code
obj-$(CONFIG_MTK_ULTRASND_PROXIMITY) += scp_ultra/

# dsp related code
ifeq ($(CONFIG_SND_SOC_MTK_AUDIO_DSP),y)
obj-y += audio_dsp/v2/
obj-y += common_int/
endif

# btcvsd
obj-$(CONFIG_SND_SOC_MTK_BTCVSD) += common/
obj-$(CONFIG_SND_SOC_MEDIATEK) += common/
obj-$(CONFIG_SND_SOC_MT2701) += mt2701/
obj-$(CONFIG_SND_SOC_MT6797) += mt6797/
obj-$(CONFIG_SND_SOC_MT6853) += mt6853/
obj-$(CONFIG_SND_SOC_MT6877) += mt6877/
obj-$(CONFIG_SND_SOC_MT6885) += mt6885/
obj-$(CONFIG_SND_SOC_MT7986) += mt7986/
obj-$(CONFIG_SND_SOC_MT8173) += mt8173/
obj-$(CONFIG_SND_SOC_MT8183) += mt8183/
obj-$(CONFIG_SND_SOC_MT8186) += mt8186/
obj-$(CONFIG_SND_SOC_MT8188) += mt8188/
obj-$(CONFIG_SND_SOC_MT8192) += mt8192/
obj-$(CONFIG_SND_SOC_MT8195) += mt8195/
obj-$(CONFIG_SND_SOC_AW87519) += aw87519/

# mtk new arch
ifeq ($(CONFIG_MTK_SND_SOC_NEW_ARCH),y)

MTK_PLATFORM := $(subst $(quote),,$(CONFIG_MTK_PLATFORM))
MTK_CODEC := $(subst $(quote),,$(CONFIG_MTK_SND_SOC_CODEC))

subdir-ccflags-y += -DAUDIO_USING_WRAP_DRIVER
subdir-ccflags-y += -D_MT_IDLE_HEADER
subdir-ccflags-y += -D_REG_SYNC_WRITE

ifeq ($(CONFIG_SND_SOC_AW87519),y)
subdir-ccflags-y += -DSND_SOC_AW87519
endif

subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/accdet/mt6358
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/include
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/base/power/$(MTK_PLATFORM)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/dramc/$(MTK_PLATFORM)/

subdir-ccflags-y += -I$(src)/common_int

ifneq ($(MTK_CODEC),)
subdir-ccflags-y += -I$(src)/codec/$(MTK_CODEC)
obj-y += codec/$(MTK_CODEC)/
endif
obj-y += common_int/

endif

# include path
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat

# adsp
ifeq ($(CONFIG_MTK_AURISYS_PHONE_CALL_SUPPORT),y)
MTK_PLATFORM := $(subst $(quote),,$(CONFIG_MTK_PLATFORM))
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/adsp/$(MTK_PLATFORM)/
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/audio_ipi/common_v2/framework/
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/audio_ipi/common_v2/tasks/call/
subdir-ccflags-y += -I$(srctree)/sound/soc/mediatek/audio_dsp
endif

ifeq ($(CONFIG_SND_SOC_MTK_AUDIO_DSP),y)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/audio_ipi/common_v2/tasks/audplayback/
endif

ifeq ($(CONFIG_SND_SOC_MTK_SMART_PHONE),y)
subdir-ccflags-y += -I$(srctree)/drivers/misc/mediatek/include
endif
