// SPDX-License-Identifier: GPL-2.0
/*
 * oplus_frame_sync_shm.c —— 帧同步共享池通道（工单 #3）
 *
 * 由 _src/memkernel/shm.c 移植而来（该实现已实机验证），改动：
 *   - 命名统一 oplus_frame_sync_shm_ 前缀，ABI 对齐原厂 frame_boost 风格
 *   - attach 从 ioctl 改成内核自定位入口（pid/addr/size 由 #4 定位后传入）
 *   - header 字段功能语义贴原厂：buf_id 固定标识、stamp 纳秒时间戳
 *   - B2 批量读：实体/相机字段按偏移范围一次性读，页表遍历从 ~900/帧
 *     降到 ~200/帧
 *
 * 设计要点：
 *   1. 一次性 pin：attach 时 pin_user_pages_remote 锁定用户态池，之后直写
 *      物理页，零 ioctl、零缺页、不置 AF 位
 *   2. 双缓冲：内核只写 standby 区，写完 store_release 翻 buf_idx
 *   3. 看门狗：3 秒无 stamp 更新自动停采样并释放目标 mm
 *   4. 系数位变换：float 字段按 factor 派生因子做位运算后写入
 *   5. 零 dmesg 输出
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
#define OPLUS_FRAME_SYNC_FIELD_BATCH_MAX  256u

struct oplus_frame_sync_field {
	u8  used;
	u8  type;
	u32 off;	/* 相对 actor 地址（实体字段）或相对 base（扩展槽） */
	u32 tag;
};

struct oplus_frame_sync_dev {

	struct page **pages;
	void **kaddr;
	unsigned int npages;

	struct mm_struct *cm_mm;	/* 共享池持有者（app） */
	pid_t cm_pid;

	struct mm_struct *tg_mm;	/* 采样目标（游戏） */
	pid_t tg_pid;

	u64 base;
	u64 arr_off;
	u32 count;
	u32 stride;
	struct oplus_frame_sync_field fld[OPLUS_FRAME_SYNC_F_TOTAL];

	u32 stage;
	u32 rate;
	u32 rp;
	u32 frames;
	u32 errors;

	s64 last_stamp;
	unsigned long last_stamp_jiff;

	/* B2 批量读临时区：串行采样在 mutex 内，单例安全 */
	u8 field_batch[OPLUS_FRAME_SYNC_FIELD_BATCH_MAX];

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
		return NULL;	/* 所有元素步长整除 4096，正常不会跨页 */
	return (u8 *)d->kaddr[pg] + po;
}

static struct oplus_frame_sync_zone *oplus_frame_sync_shm_zone(struct oplus_frame_sync_dev *d)
{
	if (!d->kaddr || !d->npages)
		return NULL;
	return (struct oplus_frame_sync_zone *)d->kaddr[0];
}

static struct oplus_frame_sync_cmd *oplus_frame_sync_shm_cmd(struct oplus_frame_sync_dev *d, u32 idx)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, cmds) +
		     (size_t)(idx % OPLUS_FRAME_SYNC_CMD_RING) * sizeof(struct oplus_frame_sync_cmd);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_cmd));
}

static struct oplus_frame_sync_entity *oplus_frame_sync_shm_ent(struct oplus_frame_sync_dev *d, u32 buf, u32 i)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, ents) +
		     ((size_t)buf * OPLUS_FRAME_SYNC_ENTITY_MAX + i) * sizeof(struct oplus_frame_sync_entity);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_entity));
}

static struct oplus_frame_sync_camera *oplus_frame_sync_shm_cam(struct oplus_frame_sync_dev *d, u32 buf)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, cam) +
		     (size_t)buf * sizeof(struct oplus_frame_sync_camera);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_camera));
}

static struct oplus_frame_sync_ext *oplus_frame_sync_shm_ext(struct oplus_frame_sync_dev *d, u32 buf, u32 i)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, ext) +
		     ((size_t)buf * OPLUS_FRAME_SYNC_EXT_SLOTS + i) * sizeof(struct oplus_frame_sync_ext);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_ext));
}

static struct oplus_frame_sync_mbox *oplus_frame_sync_shm_mbox(struct oplus_frame_sync_dev *d, u32 i)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, mbox) +
		     (size_t)(i % OPLUS_FRAME_SYNC_MBOX_SLOTS) * sizeof(struct oplus_frame_sync_mbox);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_mbox));
}

/* ------------------------------------------------------------------ *
 * 系数位变换
 * ------------------------------------------------------------------ */
static inline u32 oplus_frame_sync_factor_of(u32 base, u32 slot)
{
	return (base ^ (slot * 0x9E3779B1u)) | 1u;
}

static inline void oplus_frame_sync_put_bits(u32 *dst, u32 bits, u32 factor)
{
	*dst = bits ^ factor;
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

	/* 一次性 pin：之后直写物理页，不触发缺页、不置 AF 位 */
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
	d->arr_off = 0;
	d->count = 0;
	d->stride = sizeof(u64);
	memset(d->fld, 0, sizeof(d->fld));
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
			if (!d->rate)
				d->rate = OPLUS_FRAME_SYNC_RATE_DEFAULT;
			d->stage = OPLUS_FRAME_SYNC_STAGE_RUNNING;
			d->last_stamp = z->hdr.stamp;
			d->last_stamp_jiff = jiffies;
		}
		break;

	case OPLUS_FRAME_SYNC_CMD_STOP:
		oplus_frame_sync_shm_target_put(d);
		break;

	case OPLUS_FRAME_SYNC_CMD_SET_RATE:
		if (c->p0 < 0 || c->p0 > (s32)OPLUS_FRAME_SYNC_RATE_MAX)
			ret = -EINVAL;
		else {
			d->rate = (u32)c->p0;
			/* 0=关：运行中切暂停；>0 从暂停恢复 */
			if (!d->rate && d->stage == OPLUS_FRAME_SYNC_STAGE_RUNNING)
				d->stage = OPLUS_FRAME_SYNC_STAGE_PAUSED;
			else if (d->rate && d->stage == OPLUS_FRAME_SYNC_STAGE_PAUSED)
				d->stage = OPLUS_FRAME_SYNC_STAGE_RUNNING;
		}
		break;

	case OPLUS_FRAME_SYNC_CMD_SET_PARAMS:
		/* p0 = 基址低 32 位  p1 = 基址高 32 位（64 位地址防截断）
		 * p2 = 数量  p3 = 数组偏移；指针步长固定为 8（u64 指针数组） */
		if (c->p2 < 0 || c->p2 > OPLUS_FRAME_SYNC_ENTITY_MAX || c->p3 < 0)
			ret = -EINVAL;
		else {
			d->base = ((u64)(u32)c->p1 << 32) | (u32)c->p0;
			d->count = (u32)c->p2;
			d->arr_off = (u32)c->p3;
			d->stride = sizeof(u64);
		}
		break;

	case OPLUS_FRAME_SYNC_CMD_SET_FIELD:
		if (c->p0 < 0 || c->p0 >= OPLUS_FRAME_SYNC_F_TOTAL || c->p1 < 0)
			ret = -EINVAL;
		else {
			d->fld[c->p0].used = 1;
			d->fld[c->p0].off = (u32)c->p1;
			d->fld[c->p0].type = c->p2 ? (u8)c->p2 : OPLUS_FRAME_SYNC_T_F32;
		}
		break;

	case OPLUS_FRAME_SYNC_CMD_SET_FIELD_EXT:
		if (c->p0 < 0 || c->p0 >= OPLUS_FRAME_SYNC_EXT_SLOTS || c->p1 < 0)
			ret = -EINVAL;
		else {
			d->fld[OPLUS_FRAME_SYNC_F_EXT_BASE + c->p0].used = 1;
			d->fld[OPLUS_FRAME_SYNC_F_EXT_BASE + c->p0].off = (u32)c->p1;
			d->fld[OPLUS_FRAME_SYNC_F_EXT_BASE + c->p0].type =
				c->p2 ? (u8)c->p2 : OPLUS_FRAME_SYNC_T_U32;
			d->fld[OPLUS_FRAME_SYNC_F_EXT_BASE + c->p0].tag = (u32)c->p3;
		}
		break;

	case OPLUS_FRAME_SYNC_CMD_SET_FACTOR:
		z->hdr.factor = c->p0 ? (u32)c->p0 : get_random_u32();
		break;

	case OPLUS_FRAME_SYNC_CMD_SET_FLAG:
		z->hdr.flag = (u32)c->p0;
		break;

	case OPLUS_FRAME_SYNC_CMD_MBOX_READ:
		/* 邮箱请求实际在采样 tick 里处理，这里仅做参数校验 */
		if (c->p0 < 0 || c->p0 >= OPLUS_FRAME_SYNC_MBOX_SLOTS)
			ret = -EINVAL;
		break;

	case OPLUS_FRAME_SYNC_CMD_RESET:
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
 * 采样
 * ------------------------------------------------------------------ */
static void oplus_frame_sync_shm_store_entity(struct oplus_frame_sync_entity *e,
					      u32 f, u32 bits,
					      struct oplus_frame_sync_zone *z)
{
	switch (f) {
	case OPLUS_FRAME_SYNC_F_X:
		oplus_frame_sync_put_bits(&e->x, bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_Y:
		oplus_frame_sync_put_bits(&e->y, bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_Z:
		oplus_frame_sync_put_bits(&e->z, bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_HP:
		oplus_frame_sync_put_bits(&e->hp, bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_HP_MAX:
		oplus_frame_sync_put_bits(&e->hp_max, bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_ARMOR:
		oplus_frame_sync_put_bits(&e->armor, bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_TEAM:
		e->team = bits;
		break;
	default:
		break;
	}
}

static void oplus_frame_sync_shm_store_camera(struct oplus_frame_sync_camera *c,
					      u32 f, u32 bits,
					      struct oplus_frame_sync_zone *z)
{
	switch (f) {
	case OPLUS_FRAME_SYNC_F_CAM_PX:
		oplus_frame_sync_put_bits(&c->pos[0], bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_CAM_PY:
		oplus_frame_sync_put_bits(&c->pos[1], bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_CAM_PZ:
		oplus_frame_sync_put_bits(&c->pos[2], bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_CAM_RX:
		oplus_frame_sync_put_bits(&c->rot[0], bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_CAM_RY:
		oplus_frame_sync_put_bits(&c->rot[1], bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_CAM_RZ:
		oplus_frame_sync_put_bits(&c->rot[2], bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	case OPLUS_FRAME_SYNC_F_CAM_FOV:
		oplus_frame_sync_put_bits(&c->fov, bits, oplus_frame_sync_factor_of(z->hdr.factor, f));
		break;
	default:
		break;
	}
}

static void oplus_frame_sync_shm_sample_entity(struct oplus_frame_sync_dev *d,
					       struct oplus_frame_sync_zone *z,
					       u32 buf)
{
	u32 i, f;
	u32 min_off = 0, max_end = 0;
	bool batch = false;

	/* B2：预扫描实体字段（F_X..F_TEAM）偏移范围 */
	for (f = 0; f < OPLUS_FRAME_SYNC_F_CAM_PX; f++) {
		u32 fend;

		if (!d->fld[f].used)
			continue;
		fend = d->fld[f].off +
		       ((d->fld[f].type == OPLUS_FRAME_SYNC_T_U8) ? 1 : 4);
		if (!batch) {
			min_off = d->fld[f].off;
			max_end = fend;
			batch = true;
		} else {
			if (d->fld[f].off < min_off)
				min_off = d->fld[f].off;
			if (fend > max_end)
				max_end = fend;
		}
	}
	/* 跨度超过批量缓冲区则回退逐字段读 */
	if (batch && max_end - min_off > OPLUS_FRAME_SYNC_FIELD_BATCH_MAX)
		batch = false;

	for (i = 0; i < d->count && i < OPLUS_FRAME_SYNC_ENTITY_MAX; i++) {
		struct oplus_frame_sync_entity *e = oplus_frame_sync_shm_ent(d, buf, i);
		u64 actor = 0;

		if (!e) {
			d->errors++;
			break;
		}
		memset(e, 0, sizeof(*e));

		if (oplus_frame_sync_read_kbuf(d->tg_mm,
					       d->base + d->arr_off + (u64)i * d->stride,
					       &actor, sizeof(actor)) <= 0) {
			d->errors++;
			continue;
		}
		if (!actor)
			continue;

		if (batch) {
			/* B2：一次性读 [min_off, max_end) 区间，再逐字段提取 */
			if (oplus_frame_sync_read_kbuf(d->tg_mm, actor + min_off,
						       d->field_batch,
						       max_end - min_off) <= 0) {
				d->errors++;
				continue;
			}
			for (f = 0; f < OPLUS_FRAME_SYNC_F_CAM_PX; f++) {
				u32 bits = 0;

				if (!d->fld[f].used)
					continue;
				memcpy(&bits, d->field_batch + (d->fld[f].off - min_off),
				       (d->fld[f].type == OPLUS_FRAME_SYNC_T_U8) ? 1 : 4);
				oplus_frame_sync_shm_store_entity(e, f, bits, z);
			}
		} else {
			for (f = 0; f < OPLUS_FRAME_SYNC_F_CAM_PX; f++) {
				u32 bits = 0;
				int len;

				if (!d->fld[f].used)
					continue;
				len = (d->fld[f].type == OPLUS_FRAME_SYNC_T_U8) ? 1 : 4;
				if (oplus_frame_sync_read_kbuf(d->tg_mm,
							       actor + d->fld[f].off,
							       &bits, len) <= 0) {
					d->errors++;
					continue;
				}
				oplus_frame_sync_shm_store_entity(e, f, bits, z);
			}
		}
	}
	z->hdr.entity_num = d->count;
}

static void oplus_frame_sync_shm_sample_camera(struct oplus_frame_sync_dev *d,
					       struct oplus_frame_sync_zone *z,
					       u32 buf)
{
	struct oplus_frame_sync_camera *c = oplus_frame_sync_shm_cam(d, buf);
	u32 f;
	u32 min_off = 0, max_end = 0;
	bool batch = false;

	if (!c)
		return;
	memset(c, 0, sizeof(*c));

	/* B2：预扫描相机字段（F_CAM_PX..F_CAM_FOV）偏移范围 */
	for (f = OPLUS_FRAME_SYNC_F_CAM_PX; f <= OPLUS_FRAME_SYNC_F_CAM_FOV; f++) {
		u32 fend;

		if (!d->fld[f].used)
			continue;
		fend = d->fld[f].off + 4;
		if (!batch) {
			min_off = d->fld[f].off;
			max_end = fend;
			batch = true;
		} else {
			if (d->fld[f].off < min_off)
				min_off = d->fld[f].off;
			if (fend > max_end)
				max_end = fend;
		}
	}
	if (batch && max_end - min_off > OPLUS_FRAME_SYNC_FIELD_BATCH_MAX)
		batch = false;

	if (batch) {
		if (oplus_frame_sync_read_kbuf(d->tg_mm, d->base + min_off,
					       d->field_batch,
					       max_end - min_off) <= 0) {
			d->errors++;
			return;
		}
		for (f = OPLUS_FRAME_SYNC_F_CAM_PX; f <= OPLUS_FRAME_SYNC_F_CAM_FOV; f++) {
			u32 bits = 0;

			if (!d->fld[f].used)
				continue;
			memcpy(&bits, d->field_batch + (d->fld[f].off - min_off), 4);
			oplus_frame_sync_shm_store_camera(c, f, bits, z);
		}
	} else {
		for (f = OPLUS_FRAME_SYNC_F_CAM_PX; f <= OPLUS_FRAME_SYNC_F_CAM_FOV; f++) {
			u32 bits = 0;

			if (!d->fld[f].used)
				continue;
			if (oplus_frame_sync_read_kbuf(d->tg_mm, d->base + d->fld[f].off,
						       &bits, 4) <= 0) {
				d->errors++;
				continue;
			}
			oplus_frame_sync_shm_store_camera(c, f, bits, z);
		}
	}
}

static void oplus_frame_sync_shm_sample_ext(struct oplus_frame_sync_dev *d, u32 buf)
{
	u32 i;

	for (i = 0; i < OPLUS_FRAME_SYNC_EXT_SLOTS; i++) {
		struct oplus_frame_sync_ext *s = oplus_frame_sync_shm_ext(d, buf, i);
		struct oplus_frame_sync_field *fl = &d->fld[OPLUS_FRAME_SYNC_F_EXT_BASE + i];
		u64 val = 0;
		int len;

		if (!s)
			break;
		if (!fl->used) {
			memset(s, 0, sizeof(*s));
			continue;
		}

		len = (fl->type == OPLUS_FRAME_SYNC_T_U8) ? 1 :
		      ((fl->type == OPLUS_FRAME_SYNC_T_U32 || fl->type == OPLUS_FRAME_SYNC_T_S32) ? 4 : 8);
		if (oplus_frame_sync_read_kbuf(d->tg_mm, d->base + fl->off, &val, len) <= 0) {
			d->errors++;
			val = 0;
		}
		s->tag = fl->tag;
		s->type = fl->type;
		s->value = val;
	}
}

static void oplus_frame_sync_shm_service_mbox(struct oplus_frame_sync_dev *d, u32 buf)
{
	u32 i;

	for (i = 0; i < OPLUS_FRAME_SYNC_MBOX_SLOTS; i++) {
		struct oplus_frame_sync_mbox *m = oplus_frame_sync_shm_mbox(d, i);
		ssize_t ret;

		if (!m)
			return;
		if (m->req_seq == m->ack_seq)
			continue;
		if (m->len == 0 || m->len > OPLUS_FRAME_SYNC_MBOX_DATA) {
			m->status = (u32)-EINVAL;
			m->ack_seq = m->req_seq;
			continue;
		}

		ret = oplus_frame_sync_read_kbuf(d->tg_mm, m->addr, m->data, m->len);
		m->status = (ret > 0) ? 0 : (u32)ret;
		m->ack_seq = m->req_seq;
	}
	(void)buf;
}

static void oplus_frame_sync_shm_sample(struct oplus_frame_sync_dev *d,
					struct oplus_frame_sync_zone *z)
{
	u32 buf = z->hdr.buf_idx ? 0 : 1;	/* 只写 standby 区 */
	u32 mask = z->hdr.flag;

	if (!d->tg_mm)
		return;
	if (!mask)
		mask = OPLUS_FRAME_SYNC_FLAG_ENTITY | OPLUS_FRAME_SYNC_FLAG_CAMERA;

	if (mask & OPLUS_FRAME_SYNC_FLAG_ENTITY)
		oplus_frame_sync_shm_sample_entity(d, z, buf);
	if (mask & OPLUS_FRAME_SYNC_FLAG_CAMERA)
		oplus_frame_sync_shm_sample_camera(d, z, buf);
	if (mask & OPLUS_FRAME_SYNC_FLAG_EXT)
		oplus_frame_sync_shm_sample_ext(d, buf);
	if (mask & OPLUS_FRAME_SYNC_FLAG_MBOX)
		oplus_frame_sync_shm_service_mbox(d, buf);

	/* 原子翻页：数据写完再 release 发布索引 */
	smp_store_release(&z->hdr.buf_idx, buf);
	d->frames++;
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

	/* 采样周期（纳秒）：1e9 / rate，最低 1us 防异常 */
	period_ns = 1000000000ULL / rate;
	if (period_ns < 1000ULL)
		period_ns = 1000ULL;

	/* 周期对齐：下一周期边界 = 上一周期 + 周期长 */
	*cycle = ktime_add_ns(*cycle, period_ns);
	now = ktime_get();

	if (ktime_after(now, *cycle)) {
		/* 采样耗时已超一个周期：不睡，重排到当前，下轮再对齐 */
		*cycle = now;
		return;
	}

	/* 高精度睡到下一周期边界（允许 ±5% 抖动，省电合并唤醒） */
	rel = ktime_sub(*cycle, now);
	schedule_hrtimeout_range(&rel, period_ns / 20, HRTIMER_MODE_REL);
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

				/* 看门狗：stamp 长时间不更新则停采样 */
				if (z->hdr.stamp != d->last_stamp) {
					d->last_stamp = z->hdr.stamp;
					d->last_stamp_jiff = jiffies;
				} else if (d->stage == OPLUS_FRAME_SYNC_STAGE_RUNNING &&
					   time_after(jiffies, d->last_stamp_jiff +
						      msecs_to_jiffies(OPLUS_FRAME_SYNC_WATCHDOG_MS))) {
					oplus_frame_sync_shm_target_put(d);
					d->want_detach = true;
				}

				if (!d->want_detach && d->stage == OPLUS_FRAME_SYNC_STAGE_RUNNING)
					oplus_frame_sync_shm_sample(d, z);
			}

			if (d->want_detach) {
				d->want_detach = false;
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
	d->thread = kthread_run(oplus_frame_sync_shm_thread, d, "oplus_frame_sync");
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

	/* 幂等：重复 attach 先拆旧的 */
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

	/* 初始化头部：buf_id 固定、factor 随机、stamp 归零 */
	memset(z, 0, sizeof(*z));
	z->hdr.buf_id = 0;
	z->hdr.cmd_num = 0;
	z->hdr.stage = OPLUS_FRAME_SYNC_STAGE_IDLE;
	z->hdr.entity_num = 0;
	z->hdr.buf_idx = 0;
	z->hdr.factor = get_random_u32();
	z->hdr.flag = OPLUS_FRAME_SYNC_FLAG_ENTITY | OPLUS_FRAME_SYNC_FLAG_CAMERA;
	z->hdr.stamp = 0;

	oplus_frame_sync_shm_reset_params(&oplus_frame_sync_shm_g);
	oplus_frame_sync_shm_g.rp = 0;
	oplus_frame_sync_shm_g.frames = 0;
	oplus_frame_sync_shm_g.errors = 0;
	oplus_frame_sync_shm_g.rate = OPLUS_FRAME_SYNC_RATE_DEFAULT;
	oplus_frame_sync_shm_g.stage = OPLUS_FRAME_SYNC_STAGE_IDLE;
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

void oplus_frame_sync_shm_detach(void)
{
	mutex_lock(&oplus_frame_sync_shm_lock);
	oplus_frame_sync_shm_thread_stop(&oplus_frame_sync_shm_g);
	oplus_frame_sync_shm_target_put(&oplus_frame_sync_shm_g);
	oplus_frame_sync_shm_unpin(&oplus_frame_sync_shm_g);
	oplus_frame_sync_shm_reset_params(&oplus_frame_sync_shm_g);
	mutex_unlock(&oplus_frame_sync_shm_lock);
}
