// SPDX-License-Identifier: MIT
/*
 * shm.c — 共享内存采样通道（并入 memkernel，方案书 v2.2.1 §3/§4）
 *
 * 设计要点：
 *   1. 零新通道：attach 复用 fs/proc/version.c 的 ioctl（entry.c 分发到本文件）
 *   2. 一次性握手：attach 后稳态零 ioctl，控制全走共享页命令池
 *   3. 读取：复用 memory.c 的物理页直读（无 GUP / 无缺页 / 不置 AF 位）
 *   4. 双缓冲：内核只写 standby 区，写完 smp_store_release 翻 active_buf
 *   5. 心跳看门狗：3 秒无心跳自动停采样并释放目标 mm
 *   6. 混淆：float 字段按 obf_key 派生盐 XOR，抗物理内存特征扫描
 *   7. 零 dmesg 输出（任务书 C1）
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
#include <linux/mutex.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "comm.h"
#include "memory.h"
#include "shm.h"

#define SHM_HB_TIMEOUT_MS   3000u
#define SHM_RATE_DEFAULT    60u
#define SHM_RATE_MAX        240u

struct oplus_dfr_shm_field {
	u8  used;
	u8  type;
	u32 off;		/* 相对 actor 地址（实体字段）或相对 base（扩展槽） */
	u32 tag;
};

struct oplus_dfr_shm_dev {

	struct page **pages;
	void **kaddr;
	unsigned int npages;

	struct mm_struct *cm_mm;	/* 共享页持有者（app） */
	pid_t cm_pid;

	struct mm_struct *tg_mm;	/* 采样目标（游戏） */
	pid_t tg_pid;

	u64 base;
	u64 arr_off;
	u32 count;
	u32 stride;
	struct oplus_dfr_shm_field fld[SHM_F_TOTAL];

	u32 state;
	u32 rate;
	u32 rp;
	u32 frames;
	u32 errors;

	u32 last_hb;
	unsigned long last_hb_jiff;

	struct task_struct *thread;
	bool want_detach;
};

static struct oplus_dfr_shm_dev oplus_dfr_shm_g;
static DEFINE_MUTEX(oplus_dfr_shm_lock);

/* ------------------------------------------------------------------ *
 * 共享页访问器（按页翻译，避免假设物理连续）
 * ------------------------------------------------------------------ */
static void *oplus_dfr_shm_ptr(struct oplus_dfr_shm_dev *d, size_t off, size_t len)
{
	size_t pg = off >> PAGE_SHIFT;
	size_t po = off & ~PAGE_MASK;

	if (!d->kaddr || pg >= d->npages)
		return NULL;
	if (po + len > PAGE_SIZE)
		return NULL;	/* 所有元素步长整除 4096，正常不会跨页 */
	return (u8 *)d->kaddr[pg] + po;
}

static struct ShmZone *oplus_dfr_shm_zone(struct oplus_dfr_shm_dev *d)
{
	if (!d->kaddr || !d->npages)
		return NULL;
	return (struct ShmZone *)d->kaddr[0];
}

static struct ShmCmd *oplus_dfr_shm_cmd(struct oplus_dfr_shm_dev *d, u32 idx)
{
	size_t off = offsetof(struct ShmZone, cmds) +
		     (size_t)(idx % SHM_CMD_RING) * sizeof(struct ShmCmd);
	return oplus_dfr_shm_ptr(d, off, sizeof(struct ShmCmd));
}

static struct ShmEntity *oplus_dfr_shm_ent(struct oplus_dfr_shm_dev *d, u32 buf, u32 i)
{
	size_t off = offsetof(struct ShmZone, ents) +
		     ((size_t)buf * SHM_ENTITY_MAX + i) * sizeof(struct ShmEntity);
	return oplus_dfr_shm_ptr(d, off, sizeof(struct ShmEntity));
}

static struct ShmCamera *oplus_dfr_shm_cam(struct oplus_dfr_shm_dev *d, u32 buf)
{
	size_t off = offsetof(struct ShmZone, cam) +
		     (size_t)buf * sizeof(struct ShmCamera);
	return oplus_dfr_shm_ptr(d, off, sizeof(struct ShmCamera));
}

static struct ShmExtSlot *oplus_dfr_shm_ext(struct oplus_dfr_shm_dev *d, u32 buf, u32 i)
{
	size_t off = offsetof(struct ShmZone, ext) +
		     ((size_t)buf * SHM_EXT_SLOTS + i) * sizeof(struct ShmExtSlot);
	return oplus_dfr_shm_ptr(d, off, sizeof(struct ShmExtSlot));
}

static struct ShmMailbox *oplus_dfr_shm_mbox(struct oplus_dfr_shm_dev *d, u32 i)
{
	size_t off = offsetof(struct ShmZone, mbox) +
		     (size_t)(i % SHM_MBOX_SLOTS) * sizeof(struct ShmMailbox);
	return oplus_dfr_shm_ptr(d, off, sizeof(struct ShmMailbox));
}

/* ------------------------------------------------------------------ *
 * 混淆
 * ------------------------------------------------------------------ */
static inline u32 oplus_dfr_shm_salt(u32 key, u32 slot)
{
	return (key ^ (slot * 0x9E3779B1u)) | 1u;
}

static inline void oplus_dfr_shm_store_bits(u32 *dst, u32 bits, u32 salt)
{
	*dst = bits ^ salt;
}

/* ------------------------------------------------------------------ *
 * 目标进程
 * ------------------------------------------------------------------ */
static void oplus_dfr_shm_target_put(struct oplus_dfr_shm_dev *d)
{
	if (d->tg_mm) {
		mmput(d->tg_mm);
		d->tg_mm = NULL;
	}
	d->tg_pid = 0;
	d->state = SHM_ST_IDLE;
}

static int oplus_dfr_shm_target_get(struct oplus_dfr_shm_dev *d, pid_t pid)
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

	oplus_dfr_shm_target_put(d);
	d->tg_mm = mm;
	d->tg_pid = pid;
	return 0;
}

/* ------------------------------------------------------------------ *
 * pin / unpin
 * ------------------------------------------------------------------ */
static void oplus_dfr_shm_unpin(struct oplus_dfr_shm_dev *d)
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

static int oplus_dfr_shm_pin(struct oplus_dfr_shm_dev *d, pid_t customer_pid, u64 addr, u32 size)
{
	struct pid *p;
	struct task_struct *task;
	struct mm_struct *mm;
	unsigned int i;
	unsigned long npages;
	long ret;

	if (size < sizeof(struct ShmZone) || size > SHM_MAP_SIZE)
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
		oplus_dfr_shm_unpin(d);
		return -ENOMEM;
	}

	/* 一次性 pin：之后直写物理页，不触发缺页、不置 AF 位 */
	ret = pin_user_pages_remote(mm, (unsigned long)addr, npages,
				    FOLL_WRITE, d->pages, NULL);
	if (ret < 0) {
		mmput(mm);
		oplus_dfr_shm_unpin(d);
		return ret;
	}
	if ((unsigned long)ret != npages) {
		mmput(mm);
		d->npages = (unsigned int)ret;
		oplus_dfr_shm_unpin(d);
		return -ENOMEM;
	}

	for (i = 0; i < npages; i++) {
		d->kaddr[i] = page_address(d->pages[i]);
		if (!d->kaddr[i]) {
			mmput(mm);
			oplus_dfr_shm_unpin(d);
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
static void oplus_dfr_shm_reset_params(struct oplus_dfr_shm_dev *d)
{
	d->base = 0;
	d->arr_off = 0;
	d->count = 0;
	d->stride = sizeof(u64);
	memset(d->fld, 0, sizeof(d->fld));
}

static void oplus_dfr_shm_exec(struct oplus_dfr_shm_dev *d, struct ShmCmd *c, struct ShmZone *z)
{
	int ret = 0;

	switch (c->cmd) {
	case SHM_CMD_START:
		ret = oplus_dfr_shm_target_get(d, c->p0);
		if (!ret) {
			if (!d->rate)
				d->rate = SHM_RATE_DEFAULT;
			d->state = SHM_ST_RUNNING;
			d->last_hb = z->heartbeat;
			d->last_hb_jiff = jiffies;
		}
		break;

	case SHM_CMD_STOP:
		oplus_dfr_shm_target_put(d);
		break;

	case SHM_CMD_SET_RATE:
		if (c->p0 < 1 || c->p0 > (s32)SHM_RATE_MAX)
			ret = -EINVAL;
		else
			d->rate = (u32)c->p0;
		break;

	case SHM_CMD_SET_PARAMS:
		/* p0 = 基址低 32 位  p1 = 基址高 32 位（64 位地址防截断）
		 * p2 = 数量  p3 = 数组偏移；指针步长固定为 8（u64 指针数组） */
		if (c->p2 < 0 || c->p2 > SHM_ENTITY_MAX || c->p3 < 0)
			ret = -EINVAL;
		else {
			d->base = ((u64)(u32)c->p1 << 32) | (u32)c->p0;
			d->count = (u32)c->p2;
			d->arr_off = (u32)c->p3;
			d->stride = sizeof(u64);
		}
		break;

	case SHM_CMD_SET_FIELD:
		if (c->p0 < 0 || c->p0 >= SHM_F_TOTAL || c->p1 < 0)
			ret = -EINVAL;
		else {
			d->fld[c->p0].used = 1;
			d->fld[c->p0].off = (u32)c->p1;
			d->fld[c->p0].type = c->p2 ? (u8)c->p2 : SHM_T_F32;
		}
		break;

	case SHM_CMD_SET_FIELD_EXT:
		if (c->p0 < 0 || c->p0 >= SHM_EXT_SLOTS || c->p1 < 0)
			ret = -EINVAL;
		else {
			d->fld[SHM_F_EXT_BASE + c->p0].used = 1;
			d->fld[SHM_F_EXT_BASE + c->p0].off = (u32)c->p1;
			d->fld[SHM_F_EXT_BASE + c->p0].type =
				c->p2 ? (u8)c->p2 : SHM_T_U32;
			d->fld[SHM_F_EXT_BASE + c->p0].tag = (u32)c->p3;
		}
		break;

	case SHM_CMD_SET_OBF:
		z->obf_key = c->p0 ? (u32)c->p0 : get_random_u32();
		break;

	case SHM_CMD_SET_FEATURE:
		z->feature_mask = (u32)c->p0;
		break;

	case SHM_CMD_MBOX_READ:
		/* 邮箱请求实际在 sampling tick 里处理，这里仅做参数校验 */
		if (c->p0 < 0 || c->p0 >= SHM_MBOX_SLOTS)
			ret = -EINVAL;
		break;

	case SHM_CMD_DEAUTH:
		oplus_dfr_shm_target_put(d);
		oplus_dfr_shm_reset_params(d);
		d->want_detach = true;
		break;

	default:
		ret = -EINVAL;
		break;
	}

	c->result = (u32)ret;
}

static void oplus_dfr_shm_drain(struct oplus_dfr_shm_dev *d, struct ShmZone *z)
{
	u32 seq, guard = 0;

	seq = smp_load_acquire(&z->seq_cmd);
	if ((u32)(seq - d->rp) > SHM_CMD_RING)
		d->rp = seq - SHM_CMD_RING;

	while (d->rp != seq && guard++ < SHM_CMD_RING) {
		struct ShmCmd *c = oplus_dfr_shm_cmd(d, d->rp);

		if (!c) {
			d->rp = seq;
			break;
		}
		oplus_dfr_shm_exec(d, c, z);
		d->rp++;
	}
}

/* ------------------------------------------------------------------ *
 * 采样
 * ------------------------------------------------------------------ */
static void oplus_dfr_shm_sample_entity(struct oplus_dfr_shm_dev *d, struct ShmZone *z, u32 buf)
{
	u32 i, f;

	for (i = 0; i < d->count && i < SHM_ENTITY_MAX; i++) {
		struct ShmEntity *e = oplus_dfr_shm_ent(d, buf, i);
		u64 actor = 0;

		if (!e) {
			d->errors++;
			break;
		}
		memset(e, 0, sizeof(*e));

		if (oplus_dfr_read_kbuf(d->tg_mm,
					d->base + d->arr_off + (u64)i * d->stride,
					&actor, sizeof(actor)) <= 0) {
			d->errors++;
			continue;
		}
		if (!actor)
			continue;

		for (f = 0; f < SHM_F_BASE_MAX; f++) {
			u32 bits = 0;
			int len;

			if (!d->fld[f].used)
				continue;

			len = (d->fld[f].type == SHM_T_U8) ? 1 : 4;
			if (oplus_dfr_read_kbuf(d->tg_mm, actor + d->fld[f].off,
						&bits, len) <= 0) {
				d->errors++;
				continue;
			}

			switch (f) {
			case SHM_F_X:      oplus_dfr_shm_store_bits(&e->x, bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
			case SHM_F_Y:      oplus_dfr_shm_store_bits(&e->y, bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
			case SHM_F_Z:      oplus_dfr_shm_store_bits(&e->z, bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
			case SHM_F_HP:     oplus_dfr_shm_store_bits(&e->hp, bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
			case SHM_F_HP_MAX: oplus_dfr_shm_store_bits(&e->hp_max, bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
			case SHM_F_ARMOR:  oplus_dfr_shm_store_bits(&e->armor, bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
			case SHM_F_TEAM:   e->team = bits; break;
			default: break;
			}
		}
	}
	z->entity_count = d->count;
}

static void oplus_dfr_shm_sample_camera(struct oplus_dfr_shm_dev *d, struct ShmZone *z, u32 buf)
{
	struct ShmCamera *c = oplus_dfr_shm_cam(d, buf);
	u32 f;

	if (!c)
		return;
	memset(c, 0, sizeof(*c));

	for (f = SHM_F_CAM_PX; f <= SHM_F_CAM_FOV; f++) {
		u32 bits = 0;

		if (!d->fld[f].used)
			continue;
		if (oplus_dfr_read_kbuf(d->tg_mm, d->base + d->fld[f].off,
					&bits, 4) <= 0) {
			d->errors++;
			continue;
		}

		switch (f) {
		case SHM_F_CAM_PX: oplus_dfr_shm_store_bits(&c->pos[0], bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
		case SHM_F_CAM_PY: oplus_dfr_shm_store_bits(&c->pos[1], bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
		case SHM_F_CAM_PZ: oplus_dfr_shm_store_bits(&c->pos[2], bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
		case SHM_F_CAM_RX: oplus_dfr_shm_store_bits(&c->rot[0], bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
		case SHM_F_CAM_RY: oplus_dfr_shm_store_bits(&c->rot[1], bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
		case SHM_F_CAM_RZ: oplus_dfr_shm_store_bits(&c->rot[2], bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
		case SHM_F_CAM_FOV: oplus_dfr_shm_store_bits(&c->fov, bits, oplus_dfr_shm_salt(z->obf_key, f)); break;
		default: break;
		}
	}
}

static void oplus_dfr_shm_sample_ext(struct oplus_dfr_shm_dev *d, u32 buf)
{
	u32 i;

	for (i = 0; i < SHM_EXT_SLOTS; i++) {
		struct ShmExtSlot *s = oplus_dfr_shm_ext(d, buf, i);
		struct oplus_dfr_shm_field *fl = &d->fld[SHM_F_EXT_BASE + i];
		u64 val = 0;
		int len;

		if (!s)
			break;
		if (!fl->used) {
			memset(s, 0, sizeof(*s));
			continue;
		}

		len = (fl->type == SHM_T_U8) ? 1 : ((fl->type == SHM_T_U32 || fl->type == SHM_T_S32) ? 4 : 8);
		if (oplus_dfr_read_kbuf(d->tg_mm, d->base + fl->off, &val, len) <= 0) {
			d->errors++;
			val = 0;
		}
		s->tag = fl->tag;
		s->type = fl->type;
		s->value = val;
	}
}

static void oplus_dfr_shm_service_mbox(struct oplus_dfr_shm_dev *d, u32 buf)
{
	u32 i;

	for (i = 0; i < SHM_MBOX_SLOTS; i++) {
		struct ShmMailbox *m = oplus_dfr_shm_mbox(d, i);
		ssize_t ret;

		if (!m)
			return;
		if (m->req_seq == m->ack_seq)
			continue;
		if (m->len == 0 || m->len > SHM_MBOX_DATA) {
			m->status = (u32)-EINVAL;
			m->ack_seq = m->req_seq;
			continue;
		}

		ret = oplus_dfr_read_kbuf(d->tg_mm, m->addr, m->data, m->len);
		m->status = (ret > 0) ? 0 : (u32)ret;
		m->ack_seq = m->req_seq;
	}
	(void)buf;
}

static void oplus_dfr_shm_sample(struct oplus_dfr_shm_dev *d, struct ShmZone *z)
{
	u32 buf = z->active_buf ? 0 : 1;	/* 只写 standby 区 */
	u32 mask = z->feature_mask;

	if (!d->tg_mm)
		return;
	if (!mask)
		mask = SHM_FEAT_ENTITY | SHM_FEAT_CAMERA;

	if (mask & SHM_FEAT_ENTITY)
		oplus_dfr_shm_sample_entity(d, z, buf);
	if (mask & SHM_FEAT_CAMERA)
		oplus_dfr_shm_sample_camera(d, z, buf);
	if (mask & SHM_FEAT_EXT)
		oplus_dfr_shm_sample_ext(d, buf);
	if (mask & SHM_FEAT_MBOX)
		oplus_dfr_shm_service_mbox(d, buf);

	/* 原子翻页：数据写完再 release 发布索引 */
	smp_store_release(&z->active_buf, buf);
	d->frames++;
}

/* ------------------------------------------------------------------ *
 * kthread
 * ------------------------------------------------------------------ */
static int oplus_dfr_shm_thread(void *arg)
{
	struct oplus_dfr_shm_dev *d = arg;

	while (!kthread_should_stop()) {
		u32 interval;

		mutex_lock(&oplus_dfr_shm_lock);

		if (d->kaddr) {
			struct ShmZone *z = oplus_dfr_shm_zone(d);

			if (z) {
				oplus_dfr_shm_drain(d, z);

				/* 心跳看门狗 */
				if (z->heartbeat != d->last_hb) {
					d->last_hb = z->heartbeat;
					d->last_hb_jiff = jiffies;
				} else if (d->state == SHM_ST_RUNNING &&
					   time_after(jiffies, d->last_hb_jiff +
						      msecs_to_jiffies(SHM_HB_TIMEOUT_MS))) {
					oplus_dfr_shm_target_put(d);
					d->want_detach = true;
				}

				if (!d->want_detach && d->state == SHM_ST_RUNNING)
					oplus_dfr_shm_sample(d, z);
			}

			if (d->want_detach) {
				d->want_detach = false;
				oplus_dfr_shm_target_put(d);
				oplus_dfr_shm_unpin(d);
				oplus_dfr_shm_reset_params(d);
			}
		}

		interval = 1000 / (d->rate ? d->rate : SHM_RATE_DEFAULT);
		if (interval < 4)
			interval = 4;

		mutex_unlock(&oplus_dfr_shm_lock);
		msleep_interruptible(interval);
	}

	return 0;
}

static int oplus_dfr_shm_thread_start(struct oplus_dfr_shm_dev *d)
{
	if (d->thread)
		return 0;
	d->thread = kthread_run(oplus_dfr_shm_thread, d, "vs_wq");
	if (IS_ERR(d->thread)) {
		d->thread = NULL;
		return -ENOMEM;
	}
	return 0;
}

static void oplus_dfr_shm_thread_stop(struct oplus_dfr_shm_dev *d)
{
	if (d->thread) {
		kthread_stop(d->thread);
		d->thread = NULL;
	}
}

/* ------------------------------------------------------------------ *
 * 对外接口（entry.c 调用）
 * ------------------------------------------------------------------ */
int oplus_dfr_shm_attach(const struct AttachReq *req, struct AttachAck *ack)
{
	struct ShmZone *z;
	int ret;

	if (!req || !ack)
		return -EINVAL;
	if (req->key != OPLUS_DFR_AUTH_KEY)
		return -ENOTTY;

	mutex_lock(&oplus_dfr_shm_lock);

	/* 幂等：重复 attach 先拆旧的 */
	if (oplus_dfr_shm_g.kaddr) {
		oplus_dfr_shm_target_put(&oplus_dfr_shm_g);
		oplus_dfr_shm_unpin(&oplus_dfr_shm_g);
	}

	ret = oplus_dfr_shm_pin(&oplus_dfr_shm_g, (pid_t)req->customer_pid, req->addr, req->size);
	if (ret)
		goto out;

	z = oplus_dfr_shm_zone(&oplus_dfr_shm_g);
	if (!z) {
		oplus_dfr_shm_unpin(&oplus_dfr_shm_g);
		ret = -EFAULT;
		goto out;
	}

	/* 初始化头部：magic / obf_key 全随机 */
	memset(z, 0, sizeof(*z));
	z->magic = get_random_u32();
	z->version = SHM_ABI_VERSION;
	z->state = SHM_ST_IDLE;
	z->active_buf = 0;
	z->obf_key = get_random_u32();
	z->feature_mask = SHM_FEAT_ENTITY | SHM_FEAT_CAMERA;

	oplus_dfr_shm_reset_params(&oplus_dfr_shm_g);
	oplus_dfr_shm_g.rp = 0;
	oplus_dfr_shm_g.frames = 0;
	oplus_dfr_shm_g.errors = 0;
	oplus_dfr_shm_g.rate = SHM_RATE_DEFAULT;
	oplus_dfr_shm_g.state = SHM_ST_IDLE;
	oplus_dfr_shm_g.want_detach = false;
	oplus_dfr_shm_g.last_hb = 0;
	oplus_dfr_shm_g.last_hb_jiff = jiffies;

	ret = oplus_dfr_shm_thread_start(&oplus_dfr_shm_g);
	if (ret) {
		oplus_dfr_shm_unpin(&oplus_dfr_shm_g);
		goto out;
	}

	ack->magic = z->magic;
	ack->version = SHM_ABI_VERSION;
	ack->obf_key = z->obf_key;
	ack->status = 0;

out:
	mutex_unlock(&oplus_dfr_shm_lock);
	return ret;
}

int oplus_dfr_shm_status(struct ShmStatus *st)
{
	struct ShmZone *z;

	if (!st)
		return -EINVAL;

	mutex_lock(&oplus_dfr_shm_lock);
	z = oplus_dfr_shm_zone(&oplus_dfr_shm_g);
	st->state = oplus_dfr_shm_g.state;
	st->rate = oplus_dfr_shm_g.rate;
	st->entity_count = oplus_dfr_shm_g.count;
	st->target_pid = (u32)oplus_dfr_shm_g.tg_pid;
	st->frames = oplus_dfr_shm_g.frames;
	st->errors = oplus_dfr_shm_g.errors;
	st->heartbeat = z ? z->heartbeat : 0;
	st->_rsv = 0;
	mutex_unlock(&oplus_dfr_shm_lock);
	return 0;
}

void oplus_dfr_shm_detach(void)
{
	mutex_lock(&oplus_dfr_shm_lock);
	oplus_dfr_shm_target_put(&oplus_dfr_shm_g);
	oplus_dfr_shm_unpin(&oplus_dfr_shm_g);
	oplus_dfr_shm_reset_params(&oplus_dfr_shm_g);
	mutex_unlock(&oplus_dfr_shm_lock);
}
