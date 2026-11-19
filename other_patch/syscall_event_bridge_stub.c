// SPDX-License-Identifier: GPL-2.0
/*
 * syscall_event_bridge 兼容桩（SukiSU Nova v3.0.0 + susfs 10_enable 新 VFS 架构）
 *
 * 背景：10_enable 补丁把 sucompat/adb_root/sulog 改写为 VFS 级新流（由
 * 50_add_susfs 补丁挂入 fs/exec.c、fs/open.c、fs/stat.c），原 event_bridge 的
 * pt_regs 旧流整体废弃；但 hook/syscall_hook_manager.c 仍按名注册 5 个 hook
 * 处理函数，本文件提供与新架构兼容的实现：
 *
 * - ksu_hook_setresuid: 功能完整。uid 变化 → ksu_handle_setresuid →
 *   kernel_umount 触发链 + seccomp 缓存放行 + tracepoint 任务标记。
 * - execve/execveat/newfstatat/faccessat: 直通。对应拦截已由 VFS 层
 *   ksu_handle_execveat / ksu_handle_faccessat / ksu_handle_stat 接管，
 *   这里只调原始 syscall，零行为差异。
 */
#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/ptrace.h>
#include <linux/uidgid.h>
#include <asm/syscall.h>

#include "hook/syscall_hook.h"
#include "hook/syscall_event_bridge.h"
#include "hook/setuid_hook.h"

/* setuid_hook.c 提供；其头文件未导出此声明，在此显式声明 */
int ksu_handle_setresuid(uid_t old_uid, uid_t new_uid);

long __nocfi ksu_hook_setresuid(int orig_nr, const struct pt_regs *regs)
{
	uid_t old_uid = current_uid().val;
	long ret = ksu_syscall_table[orig_nr](regs);

	if (ret < 0)
		return ret;

	ksu_handle_setresuid(old_uid, current_uid().val);
	return ret;
}

long __nocfi ksu_hook_execve(int orig_nr, const struct pt_regs *regs)
{
	return ksu_syscall_table[orig_nr](regs);
}

long __nocfi ksu_hook_execveat(int orig_nr, const struct pt_regs *regs)
{
	return ksu_syscall_table[orig_nr](regs);
}

long __nocfi ksu_hook_newfstatat(int orig_nr, const struct pt_regs *regs)
{
	return ksu_syscall_table[orig_nr](regs);
}

long __nocfi ksu_hook_faccessat(int orig_nr, const struct pt_regs *regs)
{
	return ksu_syscall_table[orig_nr](regs);
}

void ksu_stop_ksud_execve_hook(void)
{
	/* 新架构无 ksud execve 静态分支；空实现满足头文件声明 */
}
