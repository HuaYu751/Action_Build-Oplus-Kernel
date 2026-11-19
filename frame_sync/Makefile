# oplus_frame_sync —— 无痕读取内核模块（工单 #1：最小可加载形态）
#
# 编译方式（只编驱动，不编内核镜像，恶魔同款）：
#   make -C <内核源码树> M=<本目录> modules
# 或：
#   make KDIR=<内核源码树> all
#
# 内核源码树需已 prepare（有 Module.symvers / scripts/mod 等），
# 本单在 CI 内跑 make modules_prepare 后执行，只产 .ko，不产 Image。
#
# 注意：内核源码树本身带 modversions，符号 CRC 必须在 CI 内用 modpost
# 实测核对（工单 #2）。本 Makefile 不写死任何厂商路径，靠 KDIR 注入。

obj-m += oplus_frame_sync.o
oplus_frame_sync-objs := oplus_frame_sync_core.o oplus_frame_sync_shm.o oplus_frame_sync_trigger.o oplus_frame_sync_hook.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
