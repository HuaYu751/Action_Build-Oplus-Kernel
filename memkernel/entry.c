// MIT License
/*
 * Memory Operation driver for Linux Android — 方案三（built-in 隐蔽形态）
 *
 * Original author:  Jiang-Night
 * Rework: 2026-11-16
 *
 * 与旧版的区别（任务书第四节 C1~C11）：
 *  - 不注册 misc / 字符设备，不建 /dev 节点（C2/C10）
 *  - 通道由 fs/proc/version.c 的 proc_ops.proc_ioctl 挂进来
 *    （见 patches/0001-proc-version-ioctl.patch），本文件只提供分发函数
 *  - 认证握手：OP_AUTHENTICATE 绑定单 tgid，其余进程一律 -ENOTTY（C11）
 *  - 无 module_param（C1）、无 init 注册需求、零 dmesg 输出
 *  - built-in（obj-y）形态，无 module_init/module_exit（C4）
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include "comm.h"
#include "memory.h"

/* ---- 认证状态（C11）：绑定单个用户态进程组 ---- */
static pid_t oplus_dfr_auth_tgid;
static int oplus_dfr_authed;

/*
 * 分发入口：被 fs/proc/version.c 的 version_proc_ioctl() 调用。
 *
 * 行为契约（与正常内核节点无法区分）：
 *  - cmd 不在幻数区间           → -ENOTTY
 *  - 未认证时收到任何 cmd        → -ENOTTY（含除 OP_AUTHENTICATE 外全部）
 *  - 已认证但调用者非绑定 tgid    → -ENOTTY（含 OP_AUTHENTICATE 重试）
 *  - OP_MODULE_BASE 等未实现命令  → -ENOTTY
 *  - 读/写参数非法               → -EINVAL / -EFAULT（语义同正常驱动错误路径）
 */
long oplus_dfr_dispatch(unsigned int cmd, unsigned long arg)
{
	struct CopyMemory cm;

	if (cmd < OPLUS_DFR_CMD_BASE || cmd > OPLUS_DFR_CMD_END)
		return -ENOTTY;

	/* 第一关：认证握手 */
	if (!oplus_dfr_authed) {
		if (cmd != OP_AUTHENTICATE)
			return -ENOTTY;
		if (arg != OPLUS_DFR_AUTH_KEY)
			return -ENOTTY;
		oplus_dfr_auth_tgid = current->tgid;
		oplus_dfr_authed = 1;
		return 0;
	}

	/* 第二关：绑定校验——其他进程看到的永远是 -ENOTTY */
	if (current->tgid != oplus_dfr_auth_tgid)
		return -ENOTTY;

	switch (cmd) {
	case OP_READ_MEM:
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0)
			return -EFAULT;
		return oplus_dfr_rw_mem(cm.pid, cm.addr, cm.buffer, cm.size, false);

	case OP_WRITE_MEM:
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0)
			return -EFAULT;
		return oplus_dfr_rw_mem(cm.pid, cm.addr, cm.buffer, cm.size, true);

	case OP_DEAUTH:
		oplus_dfr_authed = 0;
		oplus_dfr_auth_tgid = 0;
		return 0;

	default:
		break;
	}
	return -ENOTTY;
}
