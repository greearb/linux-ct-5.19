# SPDX-License-Identifier: GPL-2.0
zr36067-objs	:=	zoran_device.o \
			zoran_driver.o zoran_card.o videocodec.o

obj-$(CONFIG_VIDEO_ZORAN) += zr36067.o
zr36067-$(CONFIG_VIDEO_ZORAN_DC30) += zr36050.o zr36016.o
zr36067-$(CONFIG_VIDEO_ZORAN_ZR36060) += zr36060.o
