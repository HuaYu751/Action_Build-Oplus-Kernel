#ifndef MEMKERNEL_MEM_H
#define MEMKERNEL_MEM_H

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm_types.h>

/*
 * 全部函数 oplus_dfr_ 前缀（任务书 C3）：
 * 一是风格混入原厂 oplus_* 符号，二是让 kallsyms 过滤表（B1 正则 ^oplus_dfr）
 * 能无遗漏地捕获。
 *
 * v3（2026-11-17）新增：
 *   - oplus_dfr_read_kbuf()  —— 内核缓冲区直读（供共享内存采样器调用，
 *     不经过 copy_to_user，写入已 pin 的共享页线性映射地址）
 *   - oplus_dfr_mm_get/put   —— 目标 mm 复用（采样循环避免每帧查找进程）
 *   - THP 大页处理           —— 修 pmd_trans_huge 未判导致的内核崩溃风险
 */
ssize_t oplus_dfr_rw_mem(pid_t pid, uintptr_t addr, void *buffer, size_t size, bool iswrite);

/* 内核缓冲区读：成功返回字节数，失败返回负 errno。需调用方持有 mm 引用。 */
ssize_t oplus_dfr_read_kbuf(struct mm_struct *mm, uintptr_t addr, void *kbuf, size_t size);

#endif
