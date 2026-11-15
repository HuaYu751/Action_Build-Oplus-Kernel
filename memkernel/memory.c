#include "memory.h"
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/kernel.h>

#include <asm/cpu.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/pgtable.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0))
#include <linux/mmap_lock.h>
#define MM_READ_LOCK(mm) mmap_read_lock(mm);
#define MM_READ_UNLOCK(mm) mmap_read_unlock(mm);
#else
#include <linux/rwsem.h>
#define MM_READ_LOCK(mm) down_read(&(mm)->mmap_sem);
#define MM_READ_UNLOCK(mm) up_read(&(mm)->mmap_sem);
#endif

/*
 * 手工页表遍历（硬件级直读，方案三核心）：
 *  - 绕过 ptrace / /proc/pid/mem / process_vm_readv 全部用户态 API 监控
 *  - pte_present() 预检（任务书 C8）：未驻留页返回 0 → 上层跳过，
 *    绝不 fault-in → mincore 蜜罐失效、目标进程 min_flt 零增长
 *  - 禁 ioremap（任务书 C9）：物理页经 phys_to_virt 直读
 */
static phys_addr_t oplus_dfr_translate(struct mm_struct *mm, uintptr_t va)
{
	pgd_t *pgd;
#ifdef __PAGETABLE_P4D_FOLDED
	p4d_t *p4d;
#endif
	pmd_t *pmd;
	pte_t *pte;
	pud_t *pud;

	phys_addr_t page_addr;
	uintptr_t page_offset;

	pgd = pgd_offset(mm, va);
	if (pgd_none(*pgd) || pgd_bad(*pgd)) {
		return 0;
	}
#ifdef __PAGETABLE_P4D_FOLDED
	p4d = p4d_offset(pgd, va);
	if (p4d_none(*p4d) || p4d_bad(*p4d)) {
		return 0;
	}
	pud = pud_offset(p4d, va);
#else
	pud = pud_offset(pgd, va);
#endif
	if (pud_none(*pud) || pud_bad(*pud)) {
		return 0;
	}
	pmd = pmd_offset(pud, va);
	if (pmd_none(*pmd)) {
		return 0;
	}
	pte = pte_offset_kernel(pmd, va);
	if (pte_none(*pte)) {
		return 0;
	}
	/* C8：PTE present 预检——页不在物理内存就直接放弃，不触发缺页 */
	if (!pte_present(*pte)) {
		return 0;
	}
	page_addr = (phys_addr_t)(pte_pfn(*pte) << PAGE_SHIFT);
	page_offset = va & (PAGE_SIZE - 1);

	return page_addr + page_offset;
}

#if !defined(ARCH_HAS_VALID_PHYS_ADDR_RANGE) || defined(MODULE)
static inline int memk_valid_phys_addr_range(phys_addr_t addr, size_t size)
{
	return addr + size <= __pa(high_memory);
}
#define IS_VALID_PHYS_ADDR_RANGE(x,y) memk_valid_phys_addr_range(x,y)
#else
#define IS_VALID_PHYS_ADDR_RANGE(x,y) valid_phys_addr_range(x,y)
#endif

static size_t oplus_dfr_read_phys(phys_addr_t pa, void *buffer, size_t size)
{

	if (!pfn_valid(__phys_to_pfn(pa))) {
		return 0;
	}
	if (!IS_VALID_PHYS_ADDR_RANGE(pa, size)) {
		return 0;
	}
	if (copy_to_user(buffer, phys_to_virt(pa), size)) {
		return 0;
	}
	return size;
}

static size_t oplus_dfr_write_phys(phys_addr_t pa, void *buffer, size_t size)
{

	if (!pfn_valid(__phys_to_pfn(pa))) {
		return 0;
	}
	if (!IS_VALID_PHYS_ADDR_RANGE(pa, size)) {
		return 0;
	}
	if (copy_from_user(phys_to_virt(pa), buffer, size)) {
		return 0;
	}
	return size;
}

/*
 * 返回值：>0 实际读写字节数；-EINVAL 参数错；-ESRCH 进程不存在。
 * 跨页读取时遇到未驻留页即停（返回已成功部分），与 C8 一致。
 */
ssize_t oplus_dfr_rw_mem(
	pid_t pid,
	uintptr_t addr,
	void *buffer,
	size_t size,
	bool iswrite)
{

	struct task_struct *task;
	struct mm_struct *mm;
	struct pid *pid_struct;
	phys_addr_t pa;
	size_t max_chunk;
	size_t count = 0;

	if (size <= 0 || buffer == NULL) {
		return -EINVAL;
	}

	pid_struct = find_get_pid(pid);
	if (!pid_struct) {
		return -ESRCH;
	}
	task = get_pid_task(pid_struct, PIDTYPE_PID);
	put_pid(pid_struct);
	if (!task) {
		return -ESRCH;
	}
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm) {
		return -ESRCH;
	}

	MM_READ_LOCK(mm);
	while(size > 0)
	{
		pa = oplus_dfr_translate(mm, addr);
		if (!pa)
			break;

		max_chunk = min(PAGE_SIZE - (addr & (PAGE_SIZE - 1)), min(size, PAGE_SIZE));

		if (iswrite
			? !oplus_dfr_write_phys(pa, buffer, max_chunk)
			: !oplus_dfr_read_phys(pa, buffer, max_chunk))
		{
			break;
		}

		count += max_chunk;
		size -= max_chunk;
		buffer += max_chunk;
		addr += max_chunk;
	}
	MM_READ_UNLOCK(mm);
	mmput(mm);
	return (count > 0 ? count : -EFAULT);
}
