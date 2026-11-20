// SPDX-License-Identifier: GPL-2.0
/*
 * oplus_frame_sync_trigger.c —— 音量键触发 + 进程/池自定位（工单 #4）
 *
 * 触发：kprobe hook input_event（旁路观察，非阻断），过滤 KEY_VOLUMEDOWN，
 *       按下 2 秒触发一次自定位扫描，定位 oplus_frame_sync 进程的 256KB
 *       共享池后调 shm_attach 绑定。
 *
 * 隐蔽性：不注册 input handler、不改 text 段、不依赖 inline patch——
 *   input_event 是导出符号，register_kprobe 只挂 pre_handler 旁路观察，
 *   不改写任何代码。kprobe 在 GKI 下是唯一可用且被大量系统组件（含
 *   SUSFS）使用的机制，混入其中不引入新的注册面。
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/string.h>
#include <linux/kprobes.h>

#include "oplus_frame_sync_shm.h"
#include "oplus_frame_sync_uapi.h"

#define OPLUS_FRAME_SYNC_VOL_TIMEOUT_MS  2000u
/* 池尺寸与 ABI 单一来源 uapi.h（258048 = 252KB/63页，避开 256KB 整数尺寸撞车） */
#define OPLUS_FRAME_SYNC_POOL_SIZE       OPLUS_FRAME_SYNC_MAP_SIZE

/* 长按计时状态 */
static bool vol_down_pressed;
static struct delayed_work scan_dwork;

/*
 * kprobe pre_handler（input_event 第 0 参是 dev，第 1~3 参是 type/code/value）。
 * 只做整数判断，不阻断、不修改原始调用，过滤后交给 delayed work。
 */
static int oplus_frame_sync_input_pre(struct kprobe *p, struct pt_regs *regs)
{
	unsigned int type = (unsigned int)regs->regs[1];
	unsigned int code = (unsigned int)regs->regs[2];
	int value = (int)regs->regs[3];

	if (type != EV_KEY || code != KEY_VOLUMEDOWN)
		return 0;

	if (value == 1) {
		/* 按下：2 秒后检查是否仍在按 */
		vol_down_pressed = true;
		mod_delayed_work(system_wq, &scan_dwork,
				 msecs_to_jiffies(OPLUS_FRAME_SYNC_VOL_TIMEOUT_MS));
	} else if (value == 0) {
		/* 抬起：取消 */
		vol_down_pressed = false;
		cancel_delayed_work(&scan_dwork);
	}
	/* value == 2（autorepeat）忽略，由 delayed work 处理 */
	return 0;
}

static struct kprobe vol_kp = {
	.symbol_name = "input_event",
	.pre_handler = oplus_frame_sync_input_pre,
};

/* ------------------------------------------------------------------ *
 * 自定位扫描：找进程 + 找池 + attach
 * ------------------------------------------------------------------ */
static int oplus_frame_sync_locate_and_attach(void)
{
	struct task_struct *task, *t;
	struct pid *p;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	pid_t pid = 0;
	unsigned long pool_addr = 0;

	/* ① 找进程 comm == "oplus_gfxserver"（15 字符，与前端 prctl 设置一致；
	 *    跳过内核线程，避免匹配到采样 kthread） */
	rcu_read_lock();
	for_each_process(task) {
		if (task->flags & PF_KTHREAD)
			continue;
		if (!strncmp(task->comm, "oplus_gfxserver", TASK_COMM_LEN)) {
			pid = task_pid_nr(task);
			break;
		}
	}
	rcu_read_unlock();
	if (!pid)
		return -ESRCH;

	/* ② 拿 mm（task 引用保留到魔数校验完成，access_process_vm 需要 task） */
	p = find_get_pid(pid);
	if (!p)
		return -ESRCH;
	t = get_pid_task(p, PIDTYPE_PID);
	put_pid(p);
	if (!t)
		return -ESRCH;
	mm = get_task_mm(t);
	if (!mm) {
		put_task_struct(t);
		return -ESRCH;
	}

	/* ③ 找 258048B(63页) 匿名可写 VMA = 池候选（6.6 用 maple tree 遍历）；
	 *    先收集全部尺寸匹配候选，解锁后再逐个用 access_process_vm 校验归属魔数
	 *    （工单_共享池误绑修复_v1.md D2）。不能持 mmap_read_lock 时调
	 *    access_process_vm（内部会再拿 mmap 锁，会死锁）。 */
	{
		unsigned long cand[8];
		unsigned int n = 0, i;

		mmap_read_lock(mm);
		{
			VMA_ITERATOR(vmi, mm, 0);
			for_each_vma(vmi, vma) {
				if (n >= ARRAY_SIZE(cand))
					break;
				if ((vma->vm_end - vma->vm_start) == OPLUS_FRAME_SYNC_POOL_SIZE &&
				    (vma->vm_flags & VM_WRITE) &&
				    !vma->vm_file)
					cand[n++] = vma->vm_start;
			}
		}
		mmap_read_unlock(mm);

		for (i = 0; i < n && !pool_addr; i++) {
			u64 magic = 0;

			if (access_process_vm(t,
					      cand[i] + OPLUS_FRAME_SYNC_MAGIC_OFF,
					      &magic, sizeof(magic), 0) != sizeof(magic))
				continue;
			if (magic == OPLUS_FRAME_SYNC_POOL_MAGIC)
				pool_addr = cand[i];
		}
	}
	mmput(mm);
	put_task_struct(t);

	if (!pool_addr)
		return -ENOENT;

	/* ④ 绑定 */
	return oplus_frame_sync_shm_attach(pid, pool_addr, OPLUS_FRAME_SYNC_POOL_SIZE);
}

/* delayed work：2 秒后检查是否仍在按，按状态分流（用户决策 2026-11-21）：
 *   未绑定 → 自定位扫描 + attach（原有路径）
 *   已绑定 + 待命 → 唤醒（重新挂接记忆的游戏进程）
 *   已绑定 + 运行/暂停 → 忽略（不再重扫重绑，池不被重置）
 */
static void oplus_frame_sync_scan_work(struct work_struct *w)
{
	if (!vol_down_pressed)
		return;
	vol_down_pressed = false;

	if (oplus_frame_sync_shm_bound()) {
		oplus_frame_sync_shm_wake();
		return;
	}
	oplus_frame_sync_locate_and_attach();
}

/* ------------------------------------------------------------------ *
 * 对外接口（oplus_frame_sync_core.c 调用）
 * ------------------------------------------------------------------ */
int oplus_frame_sync_trigger_init(void)
{
	int ret;

	vol_down_pressed = false;
	INIT_DELAYED_WORK(&scan_dwork, oplus_frame_sync_scan_work);

	ret = register_kprobe(&vol_kp);
	if (ret)
		return ret;

	return 0;
}

void oplus_frame_sync_trigger_exit(void)
{
	unregister_kprobe(&vol_kp);
	cancel_delayed_work_sync(&scan_dwork);
}
