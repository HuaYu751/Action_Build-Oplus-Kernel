#ifndef MEMKERNEL_MEM_H
#define MEMKERNEL_MEM_H

#include <linux/kernel.h>
#include <linux/sched.h>

/*
 * 全部函数 oplus_dfr_ 前缀（任务书 C3）：
 * 一是风格混入原厂 oplus_* 符号，二是让 kallsyms 过滤表（B1 正则 ^oplus_dfr）
 * 能无遗漏地捕获。
 */
ssize_t oplus_dfr_rw_mem(pid_t pid, uintptr_t addr, void *buffer, size_t size, bool iswrite);

#endif
