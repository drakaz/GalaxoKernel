EXTRA_CFLAGS += -Dlinux -DLINUX -DBDC -DTOE -DBCMDRIVER -DBCMDONGLEHOST -DDHDTHREAD -DBCMWPA2 -DUSE_STOCK_MMC_DRIVER -DDHD_GPL -DDHD_SCHED -DBCMSDIO -DDHD_GPL -DBCMLXSDMMC -DBCMPLATFORM_BUS -DLINUXDIR="/home/Android/LINUX/kernel/" -DDHD_BCMEVENTS -DSHOW_EVENTS -DSRCBASE=\"$(src)/src\" -DANDROID_SPECIFIC -DCONFIG_MMC_MSM7X00A -DDHD_SDALIGN=64 -DMAX_HDR_LEN=64 -DDHD_FIRSTREAD=64 -DSDIO_ISR_THREAD

ifeq ($(CONFIG_BCM_EMBED_FW),y)
	EXTRA_CFLAGS		+= -I$(src)/src/dongle/rte/wl/builds/4325b0/sdio-g-cdc-reclaim-wme/
	EXTRA_CFLAGS		+= -DBCMEMBEDIMAGE
	EXTRA_CFLAGS		+= -DIMAGE_NAME="4325b0/sdio-g-cdc-reclaim-wme"
endif

EXTRA_CFLAGS += -I$(src)/src/include/
EXTRA_CFLAGS += -I$(src)/src/dhd/sys/
EXTRA_CFLAGS += -I$(src)/src/dongle/
EXTRA_CFLAGS += -I$(src)/src/bcmsdio/sys/
EXTRA_CFLAGS += -I$(src)/src/wl/sys/
EXTRA_CFLAGS += -I$(src)/src/shared/

LINUXVER := $(shell uname -r)
KBUILD_CFLAGS += -I$(LINUXDIR)/include -I$(shell pwd)
header-y = typedefs.h

LINUXDIR := /home/broadcom/Android/LINUX/kernel

LINUXVER := $(shell uname -r)

obj-$(CONFIG_BROADCOM_WIFI)	+= bcm4325.o

bcm4325-objs := src/dhd/sys/dhd_linux.o \
		src/shared/bcmutils.o \
		src/dhd/sys/dhd_common.o \
		src/shared/siutils.o \
		src/shared/sbutils.o \
		src/shared/aiutils.o \
		src/shared/hndpmu.o \
		src/wl/sys/wl_iw.o \
		src/shared/bcmwifi.o \
		src/dhd/sys/dhd_cdc.o \
		src/dhd/sys/dhd_linux_sched.o\
		src/dhd/sys/dhd_sdio.o \
		src/dhd/sys/dhd_custom_gpio.o \
		src/bcmsdio/sys/bcmsdh_sdmmc.o \
		src/bcmsdio/sys/bcmsdh.o \
		src/bcmsdio/sys/bcmsdh_linux.o \
		src/bcmsdio/sys/bcmsdh_sdmmc_linux.o \
		src/shared/linux_osl.o
