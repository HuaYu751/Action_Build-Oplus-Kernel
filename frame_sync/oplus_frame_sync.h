#ifndef OPLUS_FRAME_SYNC_H
#define OPLUS_FRAME_SYNC_H

#include <linux/types.h>

/* 头文件自包含：frame_sync_read_kbuf 的形参用到 struct mm_struct 指针，
 * 只做前置声明即可（完整定义在 .c 侧由 <linux/mm.h> 提供）。 */
struct mm_struct;

/*
 * oplus_frame_sync —— 无痕读取内核模块（工单 #1 最小可加载形态）
 *
 * 本头文件只暴露对外接口；读取核心在 oplus_frame_sync.c 内。
 * 后续工单（共享池 / 音量键 / 采样）会在此基础上扩展，不会改动本文件的
 * 读取函数签名，保证已实机验证的读路径不回退。
 *
 * 函数命名全部 frame_sync_ 前缀，与模块名 oplus_frame_sync 对应，
 * 便于工单 #2 自隐藏时按前缀一次性从 kallsyms 除名。
 */

/*
 * 读/写目标进程物理内存（页表遍历 + phys_to_virt 直读）。
 *   pid      目标进程
 *   addr     目标虚拟地址
 *   buffer   用户态缓冲区
 *   size     字节数
 *   iswrite  false=读 true=写
 * 返回：>0 实际字节数；-EINVAL 参数错；-ESRCH 进程不存在；-EFAULT 失败。
 * 跨页时遇未驻留页即停（返回已成功部分），不触发缺页、不置 AF 位。
 */
ssize_t frame_sync_rw_mem(pid_t pid, uintptr_t addr, void *buffer,
			  size_t size, bool iswrite);

/*
 * 内核缓冲区直读（供采样循环使用，不经过 copy_to_user）。
 *   mm   目标 mm（调用方持有引用）
 *   addr 目标虚拟地址
 *   kbuf 内核缓冲区（通常是已 pin 的共享页线性映射地址）
 *   size 字节数
 * 返回：>0 实际字节数；负 errno。
 */
ssize_t frame_sync_read_kbuf(struct mm_struct *mm, uintptr_t addr,
			     void *kbuf, size_t size);

#endif /* OPLUS_FRAME_SYNC_H */
