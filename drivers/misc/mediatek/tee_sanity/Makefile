# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_MTK_TEE_SANITY) += tee_sanity.o

# timesync to get boot time
CFLAGS_tee_sanity.o := -I$(src)
ccflags-y += -I$(src) -I$(srctree)/drivers/misc/mediatek/timer/timesync
