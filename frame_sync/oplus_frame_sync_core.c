// SPDX-License-Identifier: GPL-2.0
/*
 * oplus_frame_sync.c —— 无痕读取内核模块（工单 #1：最小可加载形态）
 *
 * 由 _src/memkernel/memory.c 移植而来（该实现已实机验证）。
 * 与 built-in 形态的区别：
 *   - 加 module_init/module_exit（built-in 没有）
 *   - 命名统一为 oplus_frame_sync_（对应模块名 oplus_frame_sync）
 *   - 去掉 /proc/version ioctl 通道依赖（本单只做"能 insmod 的最小模块"，
 *     通道在工单 3/4/5 接入）
 *
 * 读路径原理（与 emo/memkernel 同源，无痕关键）：
 *   - 手工页表遍历 PGD->PUD->PMD->PTE，绕开 ptrace//proc/pid/mem 全部监控
 *   - phys_to_virt 物理直读：不置 AF 位、不触发缺页、min_flt 零增长
 *   - pte_present() 预检：未驻留页直接跳过，绝不 fault-in
 *
 * 符号依赖（工单 #2 在 CI 编译时用 modpost 实测核对）：
 *   - 必导出：find_get_pid / get_pid_task / put_pid / get_task_mm /
 *             put_task_struct / mmput / copy_to_user / copy_from_user /
 *             cond_resched / pfn_valid
 *   - 宏/内联（无需导出）：pgd_offset / pte_offset_kernel / phys_to_virt /
 *             __phys_to_pfn / rcu_read_lock
 *   若 modpost 报某符号 undefined，按 emo 做法内联等价实现。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/rcupdate.h>
#include <net/genetlink.h>

#include <asm/cpu.h>
#include <asm/io.h>
#include <asm/memory.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/processor.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#endif

#include "oplus_frame_sync.h"
#include "oplus_frame_sync_shm.h"
#include "oplus_frame_sync_trigger.h"

/*
 * 手工页表遍历（硬件级直读）：
 *  - 绕过 ptrace / /proc/pid/mem / process_vm_readv 全部用户态 API 监控
 *  - pte_present() 预检：未驻留页返回 0 -> 上层跳过，绝不 fault-in
 *  - 大页处理：arm64 上 PUD 可为 1G block、PMD 可为 2M block（THP），
 *    原实现直接 pte_offset_kernel 会把 block 当页表指针 -> 内核崩溃。
 *    A1：按 TYPE_MASK==SECT(0b01) 严格判定 block；A2：TYPE_MASK==TABLE(0b11)
 *    才下钻取 PTE，杜绝 swap/migration 误判与垃圾表解引用。
 */
static phys_addr_t oplus_frame_sync_translate(struct mm_struct *mm, uintptr_t va)
{
	pgd_t *pgd;
#ifdef __PAGETABLE_P4D_FOLDED
	p4d_t *p4d;
#endif
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	phys_addr_t page_addr;
	uintptr_t page_offset;

	pgd = pgd_offset(mm, va);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;

#ifdef __PAGETABLE_P4D_FOLDED
	p4d = p4d_offset(pgd, va);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return 0;
	pud = pud_offset(p4d, va);
#else
	pud = pud_offset(pgd, va);
#endif
	if (pud_none(*pud) || pud_bad(*pud))
		return 0;

	/* 1G block 映射（arm64 大页直映射）。
	 * A1：严格类型判定 type==SECT(0b01)，杜绝 swap/migration entry 被误当块 */
	if ((pud_val(*pud) & PUD_TYPE_MASK) == PUD_TYPE_SECT)
		return (phys_addr_t)pud_pfn(*pud) * PAGE_SIZE +
		       (va & (PUD_SIZE - 1));

	/* A2 同款补漏：只有真表描述符(0b11)才下钻取 PMD，
	 * 0b10 非 present 表/异常形态直接放弃，防止垃圾地址当页表解引用 */
	if ((pud_val(*pud) & PUD_TYPE_MASK) != PUD_TYPE_TABLE)
		return 0;

	pmd = pmd_offset(pud, va);
	if (pmd_none(*pmd))
		return 0;

	/* 2M block 映射（THP 匿名大页）。
	 * A1：严格类型判定 type==SECT(0b01)，杜绝 swap/migration entry 被误当块 */
	if ((pmd_val(*pmd) & PMD_TYPE_MASK) == PMD_TYPE_SECT)
		return (phys_addr_t)pmd_pfn(*pmd) * PAGE_SIZE +
		       (va & (PMD_SIZE - 1));

	/* A2：只有真表描述符(0b11)才下钻取 PTE，0b10 非 present 表直接放弃 */
	if ((pmd_val(*pmd) & PMD_TYPE_MASK) != PMD_TYPE_TABLE)
		return 0;

	pte = pte_offset_kernel(pmd, va);
	if (pte_none(*pte))
		return 0;
	/* 预检：页不在物理内存就直接放弃，不触发缺页 */
	if (!pte_present(*pte))
		return 0;

	page_addr = (phys_addr_t)(pte_pfn(*pte) << PAGE_SHIFT);
	page_offset = va & (PAGE_SIZE - 1);

	return page_addr + page_offset;
}

/*
 * 物理地址范围兜底：addr+size 不得超过线性映射上界 high_memory，
 * 否则 phys_to_virt 会越界访问保留区/设备区，轻则垃圾数据重则崩溃。
 * MODULE 形态下内核不导出 valid_phys_addr_range，故自实现等价判定。
 */
#if !defined(ARCH_HAS_VALID_PHYS_ADDR_RANGE) || defined(MODULE)
static inline int oplus_frame_sync_valid_phys_addr_range(phys_addr_t addr, size_t size)
{
	return addr + size <= __pa(high_memory);
}
#define IS_VALID_PHYS_ADDR_RANGE(x, y) oplus_frame_sync_valid_phys_addr_range((x), (y))
#else
#define IS_VALID_PHYS_ADDR_RANGE(x, y) valid_phys_addr_range((x), (y))
#endif

/*
 * 内核缓冲区直读：物理页 -> 内核缓冲区（不经用户态拷贝）。
 * 供采样循环使用：直接 memcpy 到已 pin 的共享页线性映射地址。
 *
 * 无锁调用：依赖调用方持有 mm 引用，靠 pfn_valid + 地址范围校验兜底垃圾 phys。
 * B1：未驻留/非法页不中断，memset 置零后翻下一页（偏移对齐不错位），
 *     返回值 = 实际有效字节数，全失败才 -EFAULT；短读由调用方判 != size 拦截。
 * F1（工单_读取稳定性_v1）：无锁遍历撞上游戏并发改页表的瞬态失败占丢实体
 *     失败的绝对大头，translate 失败原地重试 3 次（cpu_relax，不睡眠）；
 *     短读（0<有效<请求，跨页撕裂）整体重试 1 次。
 * P1（2026-11-22 文献优化，gup_fast 同款，MMU Handbook ch.9）：每块 translate+
 *     memcpy 用 rcu_read_lock 包裹——遍历期间页表页不会被释放；经典 RCU 读侧
 *     不可睡眠，故锁粒度为"每块"，cond_resched 保持在锁外。
 * P3（同款文献 pte_same 模式）：memcpy 后重翻一次页表比对物理地址，不一致
 *     说明页被换/重映射、数据可能撕裂，整块重试 1 次，仍不一致置零当失败。
 */
#define OPLUS_FRAME_SYNC_READ_RETRY	3

static phys_addr_t oplus_frame_sync_translate_retry(struct mm_struct *mm,
						    uintptr_t va)
{
	phys_addr_t pa;
	u32 t;

	for (t = 0; t < OPLUS_FRAME_SYNC_READ_RETRY; t++) {
		pa = oplus_frame_sync_translate(mm, va);
		if (pa)
			return pa;
		cpu_relax();
	}
	return 0;
}

/*
 * 单块直读（不超一页、不跨页）：翻译 -> 双校验 -> 直读 -> 序校验。
 * 返回实际有效字节数（chunk 或 0）；失败时缓冲区已置零，偏移保持。
 */
static size_t oplus_frame_sync_read_chunk(struct mm_struct *mm,
					  uintptr_t addr, u8 *dst,
					  size_t chunk)
{
	phys_addr_t pa1, pa2;
	u32 t;

	for (t = 0; t < 2; t++) {
		rcu_read_lock();
		pa1 = oplus_frame_sync_translate_retry(mm, addr);
		if (pa1 && pfn_valid(__phys_to_pfn(pa1)) &&
		    IS_VALID_PHYS_ADDR_RANGE(pa1, chunk)) {
			/* 线性映射直读：无 fault、无 GUP、不置 AF 位 */
			memcpy(dst, phys_to_virt(pa1), chunk);
			/* P3 序校验：复读 PTE，物理地址漂移 = 数据可能撕裂 */
			pa2 = oplus_frame_sync_translate(mm, addr);
		} else {
			pa1 = 0;
			pa2 = 0;
		}
		rcu_read_unlock();

		if (!pa1) {
			memset(dst, 0, chunk);
			return 0;
		}
		if (pa2 == pa1)
			return chunk;
		cpu_relax();
	}

	/* 两次序校验都不一致：页在抖动，置零当失败（上层按短读拦截） */
	memset(dst, 0, chunk);
	return 0;
}

static ssize_t oplus_frame_sync_read_kbuf_once(struct mm_struct *mm,
					       uintptr_t addr, void *kbuf,
					       size_t size)
{
	size_t max_chunk;
	size_t count = 0;
	u8 *dst = kbuf;

	while (size > 0) {
		max_chunk = min(PAGE_SIZE - (addr & (PAGE_SIZE - 1)),
				min(size, PAGE_SIZE));

		/* B1：失败页由 read_chunk 置零跳过，保持偏移对齐，不丢洞后数据 */
		count += oplus_frame_sync_read_chunk(mm, addr, dst, max_chunk);

		size -= max_chunk;
		dst += max_chunk;
		addr += max_chunk;

		cond_resched();
	}

	return (count > 0 ? count : -EFAULT);
}

ssize_t oplus_frame_sync_read_kbuf(struct mm_struct *mm, uintptr_t addr,
			     void *kbuf, size_t size)
{
	ssize_t r;

	if (!mm || !kbuf || size == 0)
		return -EINVAL;

	r = oplus_frame_sync_read_kbuf_once(mm, addr, kbuf, size);
	if (r > 0 && (size_t)r < size)
		r = oplus_frame_sync_read_kbuf_once(mm, addr, kbuf, size);
	return r;
}

/*
 * 单实例锁（2026-11-24 用户要求第二次刷入直接“已刷入”）：
 * 模块自隐藏后 /proc/modules 查不到第二份实例，repeat insmod 会静默叠加载。
 * 用一个固定名 genl family 作为内核全局锁：第一份注册成功，第二份同名注册
 * 返回 -EEXIST，init 直接失败；模块 exit 时反注册。该 family 不提供任何 ops。
 */
static struct genl_family oplus_frame_sync_genl_family = {
	.name = "oplus_frame_sync",
	.version = 1,
	.maxattr = 0,
	.netnsok = true,
};

/*
 * 自隐藏（工单 #2）：装载后从三处除名。
 *   - /proc/modules + /proc/kallsyms：摘 modules 链表（二者都遍历这条链表）
 *   - /sys/module/oplus_frame_sync：删 kobject
 *
 * 参考恶魔 emo 反汇编还原（init_module 内）：
 *   手动内联摘链表（等价 list_del_init，零符号依赖，不赌
 *   __list_del_entry_valid_or_report 是否导出）+ kobject_del。
 *   不拿 module_mutex（恶魔同款，init 阶段窗口极小）；只 del 不 put。
 */
static void oplus_frame_sync_hide(void)
{
	struct list_head *l = &THIS_MODULE->list;

	/* ① 摘链表：prev->next = next; next->prev = prev; 再自环 */
	l->next->prev = l->prev;
	l->prev->next = l->next;
	l->next = l;
	l->prev = l;

	/* ② 删 kobject（只 del 不 put，保留模块对象） */
	kobject_del(&THIS_MODULE->mkobj.kobj);
}

static int __init oplus_frame_sync_init(void)
{
	int ret;

	/* 单例锁：第二份实例在这里直接失败，不会走到 kprobe/shm_attach */
	ret = genl_register_family(&oplus_frame_sync_genl_family);
	if (ret)
		return ret;

	/* 工单 #4：kprobe hook input_event（音量键触发 + 自定位） */
	ret = oplus_frame_sync_trigger_init();
	if (ret) {
		genl_unregister_family(&oplus_frame_sync_genl_family);
		return ret;
	}

	/* 工单 #2：最后一步自隐藏（hook 就位后再从三处除名） */
	oplus_frame_sync_hide();

	return 0;
}

static void __exit oplus_frame_sync_exit(void)
{
	/* 卸载：先恢复 input_event 入口，再清共享池。自隐藏后 rmmod 失效，
	 * 正常不会走到这里，保留作为防御性清理。 */
	oplus_frame_sync_trigger_exit();
	oplus_frame_sync_shm_detach();
	genl_unregister_family(&oplus_frame_sync_genl_family);
}

module_init(oplus_frame_sync_init);
module_exit(oplus_frame_sync_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OPlus BSP");
MODULE_DESCRIPTION("OPlus frame sync");
MODULE_VERSION("1.0");
