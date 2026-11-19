// SPDX-License-Identifier: GPL-2.0
/*
 * oplus_frame_sync_trigger.c —— 音量键触发 + 进程/池自定位（工单 #4）
 *
 * 触发：inline hook input_event（旁路观察，非阻断），过滤 KEY_VOLUMEDOWN，
 *       按下 2 秒触发一次自定位扫描，定位 oplus_frame_sync 进程的 256KB
 *       共享池后调 shm_attach 绑定。
 *
 * 隐蔽性：不注册 kprobe、不注册 input handler、不依赖 kallsyms——
 *   input_event 是导出符号，直接 &input_event 拿地址做入口 patch，
 *   列表全干净，唯一痕迹是入口代码被改为跳转（需 dump 代码段才可见）。
 *
 * 并发：patch 用 stop_machine 冻结其他 CPU，set_memory_rw/ro 切换页权限，
 *   flush_icache_range 保证指令缓存一致。
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/stop_machine.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/mmap_lock.h>
#include <linux/string.h>
#include <asm/set_memory.h>
#include <asm/cacheflush.h>

#include "oplus_frame_sync_shm.h"

#define OPLUS_FRAME_SYNC_VOL_TIMEOUT_MS  2000u
#define OPLUS_FRAME_SYNC_POOL_SIZE       (256u * 1024u)

/* oplus_frame_sync_hook.S 导出的符号 */
extern void oplus_frame_sync_ihook(void);
extern u8 oplus_frame_sync_orig_slot[16];
extern u64 oplus_frame_sync_ret_addr;

static unsigned long input_event_addr;
static u32 input_event_orig[4];
static bool hooked;

/* 长按计时状态 */
static bool vol_down_pressed;
static struct delayed_work scan_dwork;

/*
 * C 处理函数（由 .S 的 ihook 调用，须为全局符号）。
 * 只做整数判断，不用浮点/向量，故 ihook 只保存 x0-x18,x30 即可。
 */
void oplus_frame_sync_input_event(unsigned int type, unsigned int code, int value)
{
	if (type != EV_KEY || code != KEY_VOLUMEDOWN)
		return;

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
}

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

	/* ① 找进程 comm == "oplus_frame_sync" */
	rcu_read_lock();
	for_each_process(task) {
		if (!strncmp(task->comm, "oplus_frame_sync", TASK_COMM_LEN)) {
			pid = task_pid_nr(task);
			break;
		}
	}
	rcu_read_unlock();
	if (!pid)
		return -ESRCH;

	/* ② 拿 mm */
	p = find_get_pid(pid);
	if (!p)
		return -ESRCH;
	t = get_pid_task(p, PIDTYPE_PID);
	put_pid(p);
	if (!t)
		return -ESRCH;
	mm = get_task_mm(t);
	put_task_struct(t);
	if (!mm)
		return -ESRCH;

	/* ③ 找 256KB 匿名可写 VMA = 池 */
	mmap_read_lock(mm);
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if ((vma->vm_end - vma->vm_start) == OPLUS_FRAME_SYNC_POOL_SIZE &&
		    (vma->vm_flags & VM_WRITE) &&
		    !vma->vm_file) {
			pool_addr = vma->vm_start;
			break;
		}
	}
	mmap_read_unlock(mm);
	mmput(mm);

	if (!pool_addr)
		return -ENOENT;

	/* ④ 绑定 */
	return oplus_frame_sync_shm_attach(pid, pool_addr, OPLUS_FRAME_SYNC_POOL_SIZE);
}

/* delayed work：2 秒后检查是否仍在按，触发扫描 */
static void oplus_frame_sync_scan_work(struct work_struct *w)
{
	if (!vol_down_pressed)
		return;
	vol_down_pressed = false;
	oplus_frame_sync_locate_and_attach();
}

/* ------------------------------------------------------------------ *
 * inline hook 安装 / 卸载
 * ------------------------------------------------------------------ */
struct patch_ctx {
	unsigned long addr;
	u32 insn[4];
};

static int oplus_frame_sync_do_patch(void *data)
{
	struct patch_ctx *c = data;
	unsigned long slot = (unsigned long)oplus_frame_sync_orig_slot;
	unsigned long ret = (unsigned long)&oplus_frame_sync_ret_addr;

	/* ① patch input_event 入口（跳转到 ihook） */
	set_memory_rw(c->addr & PAGE_MASK, 1);
	memcpy((void *)c->addr, c->insn, 16);
	flush_icache_range(c->addr, c->addr + 16);
	set_memory_ro(c->addr & PAGE_MASK, 1);

	/* ② 填 hook 内的原始指令槽 + 跳回地址 */
	set_memory_rw(slot & PAGE_MASK, 1);
	memcpy((void *)oplus_frame_sync_orig_slot, input_event_orig, 16);
	if ((ret & PAGE_MASK) != (slot & PAGE_MASK))
		set_memory_rw(ret & PAGE_MASK, 1);
	oplus_frame_sync_ret_addr = input_event_addr + 16;
	flush_icache_range(slot, ret + 8);
	set_memory_ro(slot & PAGE_MASK, 1);
	if ((ret & PAGE_MASK) != (slot & PAGE_MASK))
		set_memory_ro(ret & PAGE_MASK, 1);

	return 0;
}

static int oplus_frame_sync_install_hook(void)
{
	struct patch_ctx c;

	if (hooked)
		return 0;

	input_event_addr = (unsigned long)&input_event;

	/* 保存入口原始 4 条指令 */
	memcpy(input_event_orig, (void *)input_event_addr, 16);

	/* 构造绝对跳转：ldr x16, [pc,#8]; br x16; .quad ihook */
	c.addr = input_event_addr;
	c.insn[0] = 0x58000050;		/* ldr x16, [pc, #8] */
	c.insn[1] = 0xD61F0200;		/* br x16 */
	*(u64 *)&c.insn[2] = (u64)&oplus_frame_sync_ihook;

	stop_machine(oplus_frame_sync_do_patch, &c, NULL);

	hooked = true;
	return 0;
}

static void oplus_frame_sync_remove_hook(void)
{
	struct patch_ctx c;

	if (!hooked)
		return;

	/* 恢复 input_event 入口原始指令 */
	c.addr = input_event_addr;
	memcpy(c.insn, input_event_orig, 16);
	stop_machine(oplus_frame_sync_do_patch, &c, NULL);

	hooked = false;
}

/* ------------------------------------------------------------------ *
 * 对外接口（oplus_frame_sync_core.c 调用）
 * ------------------------------------------------------------------ */
int oplus_frame_sync_trigger_init(void)
{
	vol_down_pressed = false;
	INIT_DELAYED_WORK(&scan_dwork, oplus_frame_sync_scan_work);
	return oplus_frame_sync_install_hook();
}

void oplus_frame_sync_trigger_exit(void)
{
	cancel_delayed_work_sync(&scan_dwork);
	oplus_frame_sync_remove_hook();
}
