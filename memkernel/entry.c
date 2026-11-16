// MIT License
/*
 * Memory Operation driver for Linux Android — 方案三（built-in 隐蔽形态）
 *
 * Original author:  Jiang-Night
 * Rework: 2026-11-16
 * v3:     2026-11-17 接入共享内存通道（方案书 v2.2.1 §3/§4）
 *
 * 与旧版的区别（任务书第四节 C1~C11）：
 *  - 不注册 misc / 字符设备，不建 /dev 节点（C2/C10）
 *  - 通道由 fs/proc/version.c 的 proc_ops.proc_ioctl 挂进来
 *    （见 patches/0001-proc-version-ioctl.patch），本文件只提供分发函数
 *  - 认证模型（2026-11-17 修订，适配一次性 root helper）：
 *      OP_ATTACH 免预认证：helper 每次都是新 tgid，旧的「单 tgid 永久
 *      绑定」会导致第二次启动永久锁死；key 在 AttachReq 内由 shm.c
 *      校验，失败一律 -ENOTTY（与正常节点行为零差异）
 *      OP_AUTHENTICATE 保留给旧读写工具：凭 key 绑定/重绑定当前 tgid
 *      ATTACH/AUTH 成功的 tgid 记入 auth_tgid，仅用于 STATUS/DETACH/
 *      READ/WRITE 的调用者校验（生产稳态全走共享页命令池，用不到）
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
#include "shm.h"

/* ---- 最近一次 AUTH/ATTACH 成功的 tgid（仅供调试类命令校验调用者） ---- */
static pid_t oplus_dfr_auth_tgid;
static int oplus_dfr_authed;

/*
 * 分发入口：被 fs/proc/version.c 的 version_proc_ioctl() 调用。
 *
 * 行为契约（与正常内核节点无法区分）：
 *  - cmd 不在幻数区间              → -ENOTTY
 *  - 无 key 的探测（含 OP_ATTACH） → -ENOTTY（copy 失败同样 -ENOTTY，
 *    不暴露 -EFAULT，避免与原厂节点产生行为差异）
 *  - 已认证但调用者非记录 tgid      → -ENOTTY
 *  - OP_MODULE_BASE 等未实现命令    → -ENOTTY
 */
long oplus_dfr_dispatch(unsigned int cmd, unsigned long arg)
{
	struct CopyMemory cm;

	if (cmd < OPLUS_DFR_CMD_BASE || cmd > OPLUS_DFR_CMD_END)
		return -ENOTTY;

	switch (cmd) {
	/*
	 * 一次性握手：helper 每次都是新 tgid，不能依赖预认证绑定。
	 * key 在 oplus_dfr_shm_attach() 内校验，失败 -ENOTTY。
	 */
	case OP_ATTACH: {
		struct AttachIo io;
		int ret;

		if (copy_from_user(&io.req, (void __user *)arg,
				   sizeof(io.req)) != 0)
			return -ENOTTY;	/* 伪装一致性 */

		ret = oplus_dfr_shm_attach(&io.req, &io.ack);
		if (ret)
			return ret;

		if (copy_to_user((void __user *)arg, &io, sizeof(io)) != 0)
			return -EFAULT;

		oplus_dfr_auth_tgid = current->tgid;
		oplus_dfr_authed = 1;
		return 0;
	}

	/* 旧认证握手：凭 key 绑定/重绑定当前 tgid（kd 等工具兼容） */
	case OP_AUTHENTICATE:
		if (arg != OPLUS_DFR_AUTH_KEY)
			return -ENOTTY;
		oplus_dfr_auth_tgid = current->tgid;
		oplus_dfr_authed = 1;
		return 0;

	default:
		break;
	}

	/* 其余命令要求调用者是最近一次 AUTH/ATTACH 的 tgid */
	if (!oplus_dfr_authed || current->tgid != oplus_dfr_auth_tgid)
		return -ENOTTY;

	switch (cmd) {
	case OP_READ_MEM:
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0)
			return -EFAULT;
		return oplus_dfr_rw_mem(cm.pid, cm.addr, cm.buffer, cm.size,
					false);

	case OP_WRITE_MEM:
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0)
			return -EFAULT;
		return oplus_dfr_rw_mem(cm.pid, cm.addr, cm.buffer, cm.size,
					true);

	case OP_SHM_STATUS: {
		struct ShmStatus st;
		int ret = oplus_dfr_shm_status(&st);

		if (ret == 0 &&
		    copy_to_user((void __user *)arg, &st, sizeof(st)) != 0)
			return -EFAULT;
		return ret;
	}

	case OP_SHM_DETACH:
		oplus_dfr_shm_detach();
		oplus_dfr_authed = 0;
		oplus_dfr_auth_tgid = 0;
		return 0;

	case OP_DEAUTH:
		oplus_dfr_authed = 0;
		oplus_dfr_auth_tgid = 0;
		return 0;

	default:
		break;
	}
	return -ENOTTY;
}
