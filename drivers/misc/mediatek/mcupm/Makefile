ifeq ($(CONFIG_MACH_MT6833),y)
MTK_PLATFORM := mt6833
else
MTK_PLATFORM := $(CONFIG_MTK_PLATFORM)
endif

# mt6877 has no mcupm platform dir; use mt6853 (same family)
ifeq ($(subst ",,$(MTK_PLATFORM)),mt6877)
MTK_PLATFORM := mt6853
endif

obj-y += $(subst ",,$(MTK_PLATFORM))/

