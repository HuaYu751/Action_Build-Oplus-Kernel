// SPDX-License-Identifier: GPL-2.0
/*
 * oplus_frame_sync_shm.c —— 帧同步共享池通道（通用内核 v5，2026-11-22）
 *
 * 架构（工单_通用内核_v3.md）：内核 = 纯指令执行器，零游戏语义。
 *   前端把指令直接写进共享池任务区（zone.jobs）→ 内核每帧现取现执行：
 *   从模块基址走指针链（每级翻页失败原地重试 3 次）→ 连续读 N 字节 →
 *   写数据格。整块失败按子段补救（每子段最多 1+retry 次，成功即停），
 *   成功子段真值保留、失败子段填 0，ok/valid 位图标明有效性。
 *   全部指令跑完后在该份 A/B 的标志格（第 511 格）写 ok=1 + 帧号，
 *   前端见标志才收，收完把自己读过的格清零。分类、过滤、绘制全在前端。
 *
 * 删除件（v4 → v5）：
 *   - pcache 链解析缓存（128KB）——没有"上次的地址"，每次按指令现走
 *   - kind=1 内核按人数组循环 ——几个人就是几条指令，内核不循环人
 *   - BASE/ARRAY/PREV 三种起点 ——只剩一种指令：一条链 + 读 + 写格
 *   - 内核私有任务表（d->jobs）——指令在池里，现取现用
 *   - CMD_SET_JOBS ——前端写完任务区 release hdr.job_num 即生效
 *
 * 保留件：pin/unpin、双缓冲翻页、命令环、看门狗、mbox 探址、
 *   音量键触发绑定（trigger.c）、read_kbuf 读原语（core.c，不动）、
 *   共享池按页拷贝（页不连续铁律）。
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/pid.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/hrtimer.h>
#include <linux/timekeeping.h>
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "oplus_frame_sync.h"
#include "oplus_frame_sync_shm.h"
#include "oplus_frame_sync_uapi.h"

#define OPLUS_FRAME_SYNC_RATE_DEFAULT     60u
#define OPLUS_FRAME_SYNC_RATE_MAX         165u
#define OPLUS_FRAME_SYNC_WATCHDOG_MS      3000u

struct oplus_frame_sync_dev {
	struct page **pages;
	void **kaddr;
	unsigned int npages;

	struct mm_struct *cm_mm;	/* 共享池持有者（app） */
	pid_t cm_pid;

	struct mm_struct *tg_mm;	/* 采样目标（游戏） */
	pid_t tg_pid;
	pid_t want_pid;

	u64 base;			/* 模块基址（SET_PARAMS 下发） */

	u32 stage;
	u32 rate;
	u32 rp;
	u32 frames;
	u32 errors;

	s64 last_stamp;
	unsigned long last_stamp_jiff;

	struct task_struct *thread;
	bool want_detach;
};

static struct oplus_frame_sync_dev oplus_frame_sync_shm_g;
static DEFINE_MUTEX(oplus_frame_sync_shm_lock);

/* ------------------------------------------------------------------ *
 * 共享池访问器（按页翻译，避免假设物理连续）
 * ------------------------------------------------------------------ */
static void *oplus_frame_sync_shm_ptr(struct oplus_frame_sync_dev *d, size_t off, size_t len)
{
	size_t pg = off >> PAGE_SHIFT;
	size_t po = off & ~PAGE_MASK;

	if (!d->kaddr || pg >= d->npages)
		return NULL;
	if (po + len > PAGE_SIZE)
		return NULL;
	return (u8 *)d->kaddr[pg] + po;
}

static struct oplus_frame_sync_zone *oplus_frame_sync_shm_zone(struct oplus_frame_sync_dev *d)
{
	if (!d->kaddr || !d->npages)
		return NULL;
	return (struct oplus_frame_sync_zone *)d->kaddr[0];
}

/* 共享池页在内核侧不一定物理连续：跨页字段必须按页拷贝，不能直接当一整块 struct 读。
 * 返回 0 成功，-EFAULT 表示页映射/长度异常。 */
static int oplus_frame_sync_shm_copy_out(struct oplus_frame_sync_dev *d,
					 size_t off, void *dst, size_t len)
{
	u8 *out = (u8 *)dst;

	while (len) {
		size_t po = off & ~PAGE_MASK;
		size_t n = min_t(size_t, len, PAGE_SIZE - po);
		void *src = oplus_frame_sync_shm_ptr(d, off, n);

		if (!src)
			return -EFAULT;
		memcpy(out, src, n);
		off += n;
		out += n;
		len -= n;
	}
	return 0;
}

static struct oplus_frame_sync_cmd *oplus_frame_sync_shm_cmd(struct oplus_frame_sync_dev *d, u32 idx)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, cmds) +
		     (size_t)(idx % OPLUS_FRAME_SYNC_CMD_RING) * sizeof(struct oplus_frame_sync_cmd);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_cmd));
}

static struct oplus_frame_sync_mbox_hdr *oplus_frame_sync_shm_mbox_hdr(struct oplus_frame_sync_dev *d)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, mbox_hdr);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_mbox_hdr));
}

static struct oplus_frame_sync_mblock *oplus_frame_sync_shm_mblock(struct oplus_frame_sync_dev *d, u32 i)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, mblocks) +
		     (size_t)(i % OPLUS_FRAME_SYNC_MBOX_BLOCKS) * sizeof(struct oplus_frame_sync_mblock);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_mblock));
}

static struct oplus_frame_sync_slot *oplus_frame_sync_shm_slot(struct oplus_frame_sync_dev *d,
							       u32 buf, u32 idx)
{
	size_t off;

	if (idx >= OPLUS_FRAME_SYNC_SLOT_MAX)
		return NULL;
	off = offsetof(struct oplus_frame_sync_zone, slots) +
	      ((size_t)(buf & 1) * OPLUS_FRAME_SYNC_SLOT_MAX + idx) *
	      sizeof(struct oplus_frame_sync_slot);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_slot));
}

/* ------------------------------------------------------------------ *
 * 目标进程
 * ------------------------------------------------------------------ */
static void oplus_frame_sync_shm_target_put(struct oplus_frame_sync_dev *d)
{
	if (d->tg_mm) {
		mmput(d->tg_mm);
		d->tg_mm = NULL;
	}
	d->tg_pid = 0;
	d->stage = OPLUS_FRAME_SYNC_STAGE_IDLE;
}

static int oplus_frame_sync_shm_target_get(struct oplus_frame_sync_dev *d, pid_t pid)
{
	struct pid *p;
	struct task_struct *task;
	struct mm_struct *mm;

	if (pid <= 0)
		return -EINVAL;

	p = find_get_pid(pid);
	if (!p)
		return -ESRCH;
	task = get_pid_task(p, PIDTYPE_PID);
	put_pid(p);
	if (!task)
		return -ESRCH;
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -ESRCH;

	oplus_frame_sync_shm_target_put(d);
	d->tg_mm = mm;
	d->tg_pid = pid;
	return 0;
}

/* ------------------------------------------------------------------ *
 * pin / unpin
 * ------------------------------------------------------------------ */
static void oplus_frame_sync_shm_unpin(struct oplus_frame_sync_dev *d)
{
	unsigned int i, pinned = 0;

	if (d->pages) {
		for (i = 0; i < d->npages; i++) {
			if (d->pages[i])
				pinned++;
		}
		unpin_user_pages(d->pages, pinned);
		kvfree(d->pages);
		d->pages = NULL;
	}
	kfree(d->kaddr);
	d->kaddr = NULL;
	d->npages = 0;

	if (d->cm_mm) {
		mmput(d->cm_mm);
		d->cm_mm = NULL;
	}
	d->cm_pid = 0;
}

static int oplus_frame_sync_shm_pin(struct oplus_frame_sync_dev *d, pid_t customer_pid, u64 addr, u32 size)
{
	struct pid *p;
	struct task_struct *task;
	struct mm_struct *mm;
	unsigned int i;
	unsigned long npages;
	long ret;

	if (size < sizeof(struct oplus_frame_sync_zone) || size > OPLUS_FRAME_SYNC_MAP_SIZE)
		return -EINVAL;
	if (addr & ~PAGE_MASK)
		return -EINVAL;

	p = find_get_pid(customer_pid);
	if (!p)
		return -ESRCH;
	task = get_pid_task(p, PIDTYPE_PID);
	put_pid(p);
	if (!task)
		return -ESRCH;
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return -ESRCH;

	npages = size >> PAGE_SHIFT;
	if (!npages) {
		mmput(mm);
		return -EINVAL;
	}

	d->pages = kvcalloc(npages, sizeof(struct page *), GFP_KERNEL);
	d->kaddr = kcalloc(npages, sizeof(void *), GFP_KERNEL);
	d->npages = npages;
	if (!d->pages || !d->kaddr) {
		mmput(mm);
		oplus_frame_sync_shm_unpin(d);
		return -ENOMEM;
	}

	ret = pin_user_pages_remote(mm, (unsigned long)addr, npages,
				    FOLL_WRITE, d->pages, NULL);
	if (ret < 0) {
		mmput(mm);
		oplus_frame_sync_shm_unpin(d);
		return ret;
	}
	if ((unsigned long)ret != npages) {
		mmput(mm);
		d->npages = (unsigned int)ret;
		oplus_frame_sync_shm_unpin(d);
		return -ENOMEM;
	}

	for (i = 0; i < npages; i++) {
		d->kaddr[i] = page_address(d->pages[i]);
		if (!d->kaddr[i]) {
			mmput(mm);
			oplus_frame_sync_shm_unpin(d);
			return -EFAULT;
		}
	}

	d->cm_mm = mm;
	d->cm_pid = customer_pid;
	return 0;
}

/* ------------------------------------------------------------------ *
 * 命令执行
 * ------------------------------------------------------------------ */
static void oplus_frame_sync_shm_reset_params(struct oplus_frame_sync_dev *d)
{
	d->base = 0;
}

static void oplus_frame_sync_shm_exec(struct oplus_frame_sync_dev *d,
				      struct oplus_frame_sync_cmd *c,
				      struct oplus_frame_sync_zone *z)
{
	int ret = 0;

	switch (c->cmd) {
	case OPLUS_FRAME_SYNC_CMD_START:
		ret = oplus_frame_sync_shm_target_get(d, c->p0);
		if (!ret) {
			d->want_pid = c->p0;
			if (!d->rate)
				d->rate = OPLUS_FRAME_SYNC_RATE_DEFAULT;
			d->stage = OPLUS_FRAME_SYNC_STAGE_RUNNING;
			d->last_stamp = z->hdr.stamp;
			d->last_stamp_jiff = jiffies;
		}
		break;

	case OPLUS_FRAME_SYNC_CMD_STOP:
		oplus_frame_sync_shm_target_put(d);
		d->stage = OPLUS_FRAME_SYNC_STAGE_STANDBY;
		break;

	case OPLUS_FRAME_SYNC_CMD_SET_RATE:
		if (c->p0 < 0 || c->p0 > (s32)OPLUS_FRAME_SYNC_RATE_MAX)
			ret = -EINVAL;
		else {
			d->rate = (u32)c->p0;
			if (!d->rate && d->stage == OPLUS_FRAME_SYNC_STAGE_RUNNING)
				d->stage = OPLUS_FRAME_SYNC_STAGE_PAUSED;
			else if (d->rate && d->stage == OPLUS_FRAME_SYNC_STAGE_PAUSED)
				d->stage = OPLUS_FRAME_SYNC_STAGE_RUNNING;
		}
		break;

	case OPLUS_FRAME_SYNC_CMD_SET_PARAMS:
		/* 仅取模块基址（p0/p1 拼 64 位），其余全部由任务区指令决定 */
		d->base = ((u64)(u32)c->p1 << 32) | (u32)c->p0;
		break;

	case OPLUS_FRAME_SYNC_CMD_SET_FACTOR:
		/* 数据不混淆，保留命令空转兼容旧前端 */
		z->hdr.factor = c->p0 ? (u32)c->p0 : (u32)get_random_u32();
		break;

	case OPLUS_FRAME_SYNC_CMD_SET_FLAG:
		z->hdr.flag = (u32)c->p0;
		break;

	case OPLUS_FRAME_SYNC_CMD_PAUSE:
		if (d->stage == OPLUS_FRAME_SYNC_STAGE_RUNNING)
			d->stage = OPLUS_FRAME_SYNC_STAGE_PAUSED;
		break;

	case OPLUS_FRAME_SYNC_CMD_DETACH:
		d->want_pid = 0;
		d->want_detach = true;
		break;

	case OPLUS_FRAME_SYNC_CMD_RESET:
		d->want_pid = 0;
		oplus_frame_sync_shm_target_put(d);
		oplus_frame_sync_shm_reset_params(d);
		d->want_detach = true;
		break;

	default:
		ret = -EINVAL;
		break;
	}

	c->result = (u32)ret;
}

static void oplus_frame_sync_shm_drain(struct oplus_frame_sync_dev *d,
				       struct oplus_frame_sync_zone *z)
{
	u32 seq, guard = 0;

	seq = smp_load_acquire(&z->hdr.cmd_num);
	if ((u32)(seq - d->rp) > OPLUS_FRAME_SYNC_CMD_RING)
		d->rp = seq - OPLUS_FRAME_SYNC_CMD_RING;

	while (d->rp != seq && guard++ < OPLUS_FRAME_SYNC_CMD_RING) {
		struct oplus_frame_sync_cmd *c = oplus_frame_sync_shm_cmd(d, d->rp);

		if (!c) {
			d->rp = seq;
			break;
		}
		oplus_frame_sync_shm_exec(d, c, z);
		d->rp++;
	}
}

/* ------------------------------------------------------------------ *
 * 指令执行器（通用读取引擎，零游戏语义）
 * ------------------------------------------------------------------ */
static inline u64 oplus_frame_sync_shm_rd64(struct mm_struct *mm, u64 a)
{
	u64 v = 0;

	oplus_frame_sync_read_kbuf(mm, a, &v, sizeof(v));
	return v;
}

/* 指令合法性校验（配置错误整条拒绝，宁缺毋滥）。
 * is_test=false → 写 slots 格（slot 字段=格号）；true → 写 mbox 块（slot 字段=块号） */
static bool oplus_frame_sync_job_valid(const struct oplus_frame_sync_job *j, bool is_test)
{
	u32 slot_max = is_test ? OPLUS_FRAME_SYNC_MBOX_BLOCKS
			       : OPLUS_FRAME_SYNC_SLOT_FLAG;
	u32 s;

	if (j->chain_len > OPLUS_FRAME_SYNC_JOB_CHAIN)
		return false;
	if (!j->read_len || j->read_len > OPLUS_FRAME_SYNC_SLOT_DATA)
		return false;
	if (j->slot >= slot_max)
		return false;	/* 正式段：标志格内核专用，指令不许写 */
	if (j->slot_off + j->read_len > OPLUS_FRAME_SYNC_SLOT_DATA)
		return false;
	if (j->sub_count > OPLUS_FRAME_SYNC_JOB_SUB)
		return false;
	for (s = 0; s < j->sub_count; s++) {
		if (!j->sub_len[s])
			return false;
		if (j->sub_off[s] + j->sub_len[s] > j->read_len)
			return false;
	}
	if (j->retry > OPLUS_FRAME_SYNC_JOB_RETRY_MAX)
		return false;
	return true;
}

/* 走链失败：本格数据区清零 + ok=0（没有就没有，不沿用旧值） */
static void oplus_frame_sync_job_fail(const struct oplus_frame_sync_job *j,
				      struct oplus_frame_sync_slot *s)
{
	memset(s->data + j->slot_off, 0, j->read_len);
	if (!(j->flags & JOB_FLAG_NO_OK)) {
		smp_wmb();	/* 先清数据，再发布失败头 */
		s->ok = 0;
		s->len = 0;
		s->valid = 0;
	}
}

/* 走指针链：ptr = *(u64*)(ptr + chain[k])，每级失败原地重试 3 次 */
static u64 oplus_frame_sync_job_walk(struct oplus_frame_sync_dev *d,
				     const struct oplus_frame_sync_job *j)
{
	u64 ptr = d->base;
	u32 k, t;

	for (k = 0; k < j->chain_len; k++) {
		u64 addr = ptr + j->chain[k];
		u64 v = 0;

		for (t = 0; t < OPLUS_FRAME_SYNC_WALK_RETRY; t++) {
			v = oplus_frame_sync_shm_rd64(d->tg_mm, addr);
			if (v)
				break;
		}
		if (!v)
			return 0;
		ptr = v;
	}
	return ptr;
}

/* 整块读，失败按子段补救（成功子段真值保留、失败子段填 0）。
 * dst 指向目标 data 区（容量 SLOT_DATA）；返回子段有效位图，*ok_out=1 整块成功。
 * 头（slot 的 ok/len/valid 或 mblock 的 addr/len/status）由调用方按目标类型填。 */
static u32 oplus_frame_sync_job_read_data(struct oplus_frame_sync_dev *d,
					  const struct oplus_frame_sync_job *j,
					  u64 addr, u8 *dst, int *ok_out)
{
	u32 valid = 0, k, t;

	*ok_out = 0;
	if (oplus_frame_sync_read_kbuf(d->tg_mm, addr, dst, j->read_len) ==
	    (ssize_t)j->read_len) {
		*ok_out = 1;
		return j->sub_count ? ((1u << j->sub_count) - 1u) : 0;
	}

	if (!j->sub_count) {
		memset(dst, 0, j->read_len);
		return 0;
	}

	for (k = 0; k < j->sub_count; k++) {
		u8 *sub = dst + j->sub_off[k];

		for (t = 0; t <= j->retry; t++) {
			if (oplus_frame_sync_read_kbuf(d->tg_mm, addr + j->sub_off[k],
						       sub, j->sub_len[k]) ==
			    (ssize_t)j->sub_len[k]) {
				valid |= 1u << k;
				break;
			}
		}
		if (!(valid & (1u << k)))
			memset(sub, 0, j->sub_len[k]);
	}
	return valid;
}

/* 测试区走链失败：数据区清零 + status=2（mblock 无 ok/valid 概念） */
static void oplus_frame_sync_mblock_fail(const struct oplus_frame_sync_job *j,
					 struct oplus_frame_sync_mblock *b)
{
	memset(b->data + j->slot_off, 0, j->read_len);
	smp_wmb();
	b->len = 0;
	b->status = 2;
}

/* 执行一条指令。is_test=false → 结果写 slots 格（双缓冲）；true → 写 mbox 数据块。 */
static void oplus_frame_sync_job_run(struct oplus_frame_sync_dev *d,
				     const struct oplus_frame_sync_job *j,
				     u32 buf, bool is_test)
{
	u64 ptr;
	int ok;
	u32 valid;

	if (is_test) {
		struct oplus_frame_sync_mblock *b =
			oplus_frame_sync_shm_mblock(d, j->slot);

		if (!b)
			return;
		if (!d->base) {
			oplus_frame_sync_mblock_fail(j, b);
			return;
		}
		ptr = oplus_frame_sync_job_walk(d, j);
		if (!ptr) {
			oplus_frame_sync_mblock_fail(j, b);
			return;
		}
		valid = oplus_frame_sync_job_read_data(d, j, ptr + j->read_off,
						       b->data, &ok);
		if (!(j->flags & JOB_FLAG_NO_OK)) {
			smp_wmb();	/* 先写完数据，再发布块头 */
			b->addr = ptr + j->read_off;
			b->len = ok ? j->read_len : 0;
			b->status = ok ? 1 : 2;
			(void)valid;	/* mblock 头无 valid 位图，子段成败体现在数据里 */
		}
		return;
	}

	struct oplus_frame_sync_slot *s =
		oplus_frame_sync_shm_slot(d, buf, j->slot);

	if (!s)
		return;
	if (!d->base) {
		oplus_frame_sync_job_fail(j, s);
		return;
	}

	ptr = oplus_frame_sync_job_walk(d, j);
	if (!ptr) {
		oplus_frame_sync_job_fail(j, s);
		return;
	}
	valid = oplus_frame_sync_job_read_data(d, j, ptr + j->read_off,
					       s->data + j->slot_off, &ok);
	if (!(j->flags & JOB_FLAG_NO_OK)) {
		smp_wmb();	/* 先写完数据，再发布头 */
		s->ok = ok;
		s->len = (ok || valid) ? j->read_len : 0;
		s->valid = valid;
	}
}

/* ------------------------------------------------------------------ *
 * mbox 订阅刷新（v6：按订阅表每帧把游戏内存最新值刷进数据块）
 * ------------------------------------------------------------------ */
static void oplus_frame_sync_shm_refresh_subs(struct oplus_frame_sync_dev *d,
					    struct oplus_frame_sync_zone *z)
{
	struct oplus_frame_sync_mbox_hdr mh;
	u32 i, n;

	/* 控制头 256B 可能跨页：按页拷出 */
	if (oplus_frame_sync_shm_copy_out(d,
			offsetof(struct oplus_frame_sync_zone, mbox_hdr),
			&mh, sizeof(mh)) != 0)
		return;
	if (mh.magic != OPLUS_FRAME_SYNC_MBOX_MAGIC)
		return;	/* 前端还没初始化订阅池 */

	n = mh.sub_count;
	if (n > OPLUS_FRAME_SYNC_MBOX_SUBS)
		n = OPLUS_FRAME_SYNC_MBOX_SUBS;

	for (i = 0; i < n; i++) {
		struct oplus_frame_sync_sub sub;
		struct oplus_frame_sync_mblock *b;

		if (oplus_frame_sync_shm_copy_out(d,
				offsetof(struct oplus_frame_sync_zone, subs) +
				(size_t)i * sizeof(sub),
				&sub, sizeof(sub)) != 0)
			break;
		if (!sub.len || sub.len > OPLUS_FRAME_SYNC_MBOX_DATA)
			continue;

		b = oplus_frame_sync_shm_mblock(d, i);
		if (!b)
			break;

		if (oplus_frame_sync_read_kbuf(d->tg_mm, sub.addr, b->data,
					       sub.len) == (ssize_t)sub.len) {
			smp_wmb();
			b->addr = sub.addr;
			b->len = sub.len;
			b->status = 1;
		} else {
			memset(b->data, 0, sub.len);
			smp_wmb();
			b->addr = sub.addr;
			b->len = 0;
			b->status = 2;
		}
	}

	/* 刷新帧号（UMT 用它判断数据新旧）。控制头里 magic/version/sub_count 是前端写的，
	 * 内核只动 frame_no——按页读改写，避免整头写回冲掉前端的订阅数。 */
	{
		struct oplus_frame_sync_mbox_hdr *ph =
			oplus_frame_sync_shm_mbox_hdr(d);

		if (ph) {
			smp_wmb();
			ph->frame_no = d->frames;
		}
	}
}

/* ------------------------------------------------------------------ *
 * 采样主流程
 * ------------------------------------------------------------------ */
static void oplus_frame_sync_shm_sample(struct oplus_frame_sync_dev *d,
					struct oplus_frame_sync_zone *z)
{
	u32 buf = z->hdr.buf_idx ? 0 : 1;	/* 只写 standby 份 */
	u32 zone_en, num, id;
	bool ran_main = false;

	if (!d->tg_mm)
		return;

	/* 双区开关：前端寻址页勾选，内核每帧 acquire。默认 3=都开。
	 * zone_enable==0 时两个区都不跑，但 frame_num/k_stamp 仍更新（心跳不中断）。 */
	zone_en = smp_load_acquire(&z->hdr.zone_enable);

	/* ---- 正式区：jobs_main → slots（双缓冲，写标志格并翻页） ---- */
	if (zone_en & OPLUS_FRAME_SYNC_ZONE_MAIN) {
		num = smp_load_acquire(&z->hdr.job_num);
		if (num > OPLUS_FRAME_SYNC_JOB_MAIN)
			num = OPLUS_FRAME_SYNC_JOB_MAIN;

		for (id = 0; id < num; id++) {
			struct oplus_frame_sync_job j;

			if (oplus_frame_sync_shm_copy_out(d,
					offsetof(struct oplus_frame_sync_zone, jobs_main) +
					(size_t)id * sizeof(j),
					&j, sizeof(j)) != 0) {
				d->errors++;
				break;
			}
			if (!oplus_frame_sync_job_valid(&j, false)) {
				d->errors++;
				continue;
			}
			oplus_frame_sync_job_run(d, &j, buf, false);
		}
		if (num)
			ran_main = true;
	}

	/* ---- 测试区：jobs_test → mbox 数据块 + 订阅刷新 ---- */
	if (zone_en & OPLUS_FRAME_SYNC_ZONE_TEST) {
		num = smp_load_acquire(&z->hdr.test_num);
		if (num > OPLUS_FRAME_SYNC_JOB_TEST)
			num = OPLUS_FRAME_SYNC_JOB_TEST;

		for (id = 0; id < num; id++) {
			struct oplus_frame_sync_job j;

			if (oplus_frame_sync_shm_copy_out(d,
					offsetof(struct oplus_frame_sync_zone, jobs_test) +
					(size_t)id * sizeof(j),
					&j, sizeof(j)) != 0) {
				d->errors++;
				break;
			}
			if (!oplus_frame_sync_job_valid(&j, true)) {
				d->errors++;
				continue;
			}
			oplus_frame_sync_job_run(d, &j, buf, true);
		}

		oplus_frame_sync_shm_refresh_subs(d, z);
	}

	d->frames++;

	/* 收尾：只有跑过正式区才写标志格 + 翻 buf_idx（slots 这份才有意义）。
	 * 只跑测试区时不翻页，避免前端读到空 slots。 */
	if (ran_main) {
		struct oplus_frame_sync_slot *f =
			oplus_frame_sync_shm_slot(d, buf, OPLUS_FRAME_SYNC_SLOT_FLAG);

		if (f) {
			u32 *fd = (u32 *)f->data;

			fd[0] = d->frames;
			fd[1] = OPLUS_FRAME_SYNC_FLAG_MAGIC;
			smp_wmb();
			f->len = 8;
			f->valid = 0;
			f->ok = 1;
		}
		smp_store_release(&z->hdr.buf_idx, buf);
	}
	smp_store_release(&z->hdr.frame_num, d->frames);
}

/* ------------------------------------------------------------------ *
 * kthread
 * ------------------------------------------------------------------ */
static void oplus_frame_sync_shm_sleep_cycle(ktime_t *cycle, u32 rate)
{
	u64 period_ns;
	ktime_t now, rel;

	if (!rate)
		rate = OPLUS_FRAME_SYNC_RATE_DEFAULT;

	period_ns = 1000000000ULL / rate;
	if (period_ns < 1000ULL)
		period_ns = 1000ULL;

	*cycle = ktime_add_ns(*cycle, period_ns);
	now = ktime_get();

	if (ktime_after(now, *cycle)) {
		*cycle = now;
		return;
	}

	rel = ktime_sub(*cycle, now);
	{
		s64 sleep_ns = ktime_to_ns(rel);
		if (sleep_ns > 0) {
			u64 slack = period_ns / 20;
			u64 lo = (u64)sleep_ns > slack ? ((u64)sleep_ns - slack) / 1000 : 0;
			u64 hi = ((u64)sleep_ns + slack) / 1000;

			if (lo < 200)
				lo = 200;
			if (hi < lo)
				hi = lo;
			usleep_range((unsigned long)lo, (unsigned long)hi);
		}
	}
}

static int oplus_frame_sync_shm_thread(void *arg)
{
	struct oplus_frame_sync_dev *d = arg;
	ktime_t cycle = ktime_get();

	while (!kthread_should_stop()) {
		u32 rate;

		mutex_lock(&oplus_frame_sync_shm_lock);

		if (d->kaddr) {
			struct oplus_frame_sync_zone *z = oplus_frame_sync_shm_zone(d);

			if (z) {
				oplus_frame_sync_shm_drain(d, z);

				smp_store_release(&z->hdr.stage, (s32)d->stage);
				smp_store_release(&z->hdr.k_stamp, (s64)ktime_get_ns());

				if (z->hdr.stamp != d->last_stamp) {
					d->last_stamp = z->hdr.stamp;
					d->last_stamp_jiff = jiffies;
				} else if (d->stage == OPLUS_FRAME_SYNC_STAGE_RUNNING &&
					   time_after(jiffies, d->last_stamp_jiff +
						      msecs_to_jiffies(OPLUS_FRAME_SYNC_WATCHDOG_MS))) {
					oplus_frame_sync_shm_target_put(d);
					d->want_pid = 0;
					d->want_detach = true;
				}

				if (!d->want_detach && d->stage == OPLUS_FRAME_SYNC_STAGE_RUNNING)
					oplus_frame_sync_shm_sample(d, z);
			}

			if (d->want_detach) {
				d->want_detach = false;
				d->want_pid = 0;
				oplus_frame_sync_shm_target_put(d);
				oplus_frame_sync_shm_unpin(d);
				oplus_frame_sync_shm_reset_params(d);
			}
		}

		rate = d->rate;
		mutex_unlock(&oplus_frame_sync_shm_lock);

		oplus_frame_sync_shm_sleep_cycle(&cycle, rate);
	}

	return 0;
}

static int oplus_frame_sync_shm_thread_start(struct oplus_frame_sync_dev *d)
{
	if (d->thread)
		return 0;
	d->thread = kthread_run(oplus_frame_sync_shm_thread, d, "fb_flush");
	if (IS_ERR(d->thread)) {
		d->thread = NULL;
		return -ENOMEM;
	}
	return 0;
}

static void oplus_frame_sync_shm_thread_stop(struct oplus_frame_sync_dev *d)
{
	if (d->thread) {
		kthread_stop(d->thread);
		d->thread = NULL;
	}
}

/* ------------------------------------------------------------------ *
 * 对外接口（#4 音量键回调调用）
 * ------------------------------------------------------------------ */
int oplus_frame_sync_shm_attach(pid_t pid, u64 addr, u32 size)
{
	struct oplus_frame_sync_zone *z;
	int ret;

	mutex_lock(&oplus_frame_sync_shm_lock);

	if (oplus_frame_sync_shm_g.kaddr) {
		oplus_frame_sync_shm_target_put(&oplus_frame_sync_shm_g);
		oplus_frame_sync_shm_unpin(&oplus_frame_sync_shm_g);
	}

	ret = oplus_frame_sync_shm_pin(&oplus_frame_sync_shm_g, pid, addr, size);
	if (ret)
		goto out;

	z = oplus_frame_sync_shm_zone(&oplus_frame_sync_shm_g);
	if (!z) {
		oplus_frame_sync_shm_unpin(&oplus_frame_sync_shm_g);
		ret = -EFAULT;
		goto out;
	}

	/* 初始化共享池：逐页清零（池页物理不连续，不能一次 memset 整个 zone）。
	 * factor 恒 0（数据不混淆），flag 默认只开 mbox 服务。 */
	{
		size_t left = sizeof(*z);
		size_t done = 0;

		while (left) {
			size_t n = min_t(size_t, left, PAGE_SIZE);

			memset((u8 *)oplus_frame_sync_shm_g.kaddr[done >> PAGE_SHIFT], 0, n);
			left -= n;
			done += n;
		}
	}
	z->hdr.cmd_num = 0;
	z->hdr.stage = OPLUS_FRAME_SYNC_STAGE_IDLE;
	z->hdr.buf_idx = 0;
	z->hdr.factor = 0;
	z->hdr.flag = OPLUS_FRAME_SYNC_FLAG_MBOX;
	z->hdr.job_num = 0;
	z->hdr.test_num = 0;
	z->hdr.zone_enable = (OPLUS_FRAME_SYNC_ZONE_MAIN | OPLUS_FRAME_SYNC_ZONE_TEST);
	z->hdr.stamp = 0;
	z->hdr.frame_num = 0;
	z->hdr.k_stamp = 0;
	z->hdr.buf_id = OPLUS_FRAME_SYNC_BUF_ID;

	/* attach 清零会覆盖前端预写的归属魔数，这里写回（前端心跳也每秒重写） */
	{
		u8 *magic_ptr = oplus_frame_sync_shm_ptr(&oplus_frame_sync_shm_g,
							 OPLUS_FRAME_SYNC_MAGIC_OFF,
							 sizeof(u64));
		if (magic_ptr)
			*(u64 *)magic_ptr = OPLUS_FRAME_SYNC_POOL_MAGIC;
	}

	oplus_frame_sync_shm_reset_params(&oplus_frame_sync_shm_g);
	oplus_frame_sync_shm_g.rp = 0;
	oplus_frame_sync_shm_g.frames = 0;
	oplus_frame_sync_shm_g.errors = 0;
	oplus_frame_sync_shm_g.rate = OPLUS_FRAME_SYNC_RATE_DEFAULT;
	oplus_frame_sync_shm_g.stage = OPLUS_FRAME_SYNC_STAGE_IDLE;
	oplus_frame_sync_shm_g.want_pid = 0;
	oplus_frame_sync_shm_g.want_detach = false;
	oplus_frame_sync_shm_g.last_stamp = 0;
	oplus_frame_sync_shm_g.last_stamp_jiff = jiffies;

	ret = oplus_frame_sync_shm_thread_start(&oplus_frame_sync_shm_g);
	if (ret) {
		oplus_frame_sync_shm_unpin(&oplus_frame_sync_shm_g);
		goto out;
	}

out:
	mutex_unlock(&oplus_frame_sync_shm_lock);
	return ret;
}

bool oplus_frame_sync_shm_bound(void)
{
	return oplus_frame_sync_shm_g.kaddr != NULL;
}

int oplus_frame_sync_shm_wake(void)
{
	struct oplus_frame_sync_dev *d = &oplus_frame_sync_shm_g;
	struct oplus_frame_sync_zone *z;
	int ret = -EINVAL;

	mutex_lock(&oplus_frame_sync_shm_lock);

	if (!d->kaddr || d->stage != OPLUS_FRAME_SYNC_STAGE_STANDBY || d->want_pid <= 0)
		goto out;

	ret = oplus_frame_sync_shm_target_get(d, d->want_pid);
	if (ret)
		goto out;

	if (!d->rate)
		d->rate = OPLUS_FRAME_SYNC_RATE_DEFAULT;
	d->stage = OPLUS_FRAME_SYNC_STAGE_RUNNING;

	z = oplus_frame_sync_shm_zone(d);
	if (z) {
		d->last_stamp = z->hdr.stamp;
		d->last_stamp_jiff = jiffies;
		smp_store_release(&z->hdr.stage, (s32)d->stage);
	}

out:
	mutex_unlock(&oplus_frame_sync_shm_lock);
	return ret;
}

void oplus_frame_sync_shm_detach(void)
{
	mutex_lock(&oplus_frame_sync_shm_lock);
	oplus_frame_sync_shm_thread_stop(&oplus_frame_sync_shm_g);
	oplus_frame_sync_shm_target_put(&oplus_frame_sync_shm_g);
	oplus_frame_sync_shm_unpin(&oplus_frame_sync_shm_g);
	oplus_frame_sync_shm_reset_params(&oplus_frame_sync_shm_g);
	mutex_unlock(&oplus_frame_sync_shm_lock);
}
