# 方案三 built-in 形态（默认）：CONFIG_OPLUS_DFR=y 时静态编进内核
# 集成方式：把本目录复制为内核源码树 drivers/oplus_dfr/，
# 在 drivers/Makefile 加 obj-$(CONFIG_OPLUS_DFR) += oplus_dfr/，
# 在 drivers/Kconfig 加 source "drivers/oplus_dfr/Kconfig"。
obj-$(CONFIG_OPLUS_DFR) += memkernel.o
memkernel-y := memory.o entry.o

# —— 以下为调试期备用：外部 LKM 编译（会破坏隐蔽性，仅调试用） ——
# make -C $(KDIR) M=$(PWD) KDIR=<内核源码树> modules
ifeq ($(CONFIG_OPLUS_DFR),m)
	obj-m := memkernel.o
endif
