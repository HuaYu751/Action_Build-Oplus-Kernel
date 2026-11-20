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
#define OPLUS_FRAME_SYNC_SCRATCH_SIZE     (64u * 1024u)   /* ESP 全链批量读暂存 */
#define OPLUS_FRAME_SYNC_ACT_MAX          512             /* ESP 全链候选 actor 上限 */
#define OPLUS_FRAME_SYNC_EH_BATCH_MAX     512             /* eh 属性区间批量读上限 */

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
	pid_t want_pid;		/* 记忆的游戏进程：STOP 后保留，音量- 唤醒/前端重发 START 用；
				 * RESET/DETACH/看门狗死亡时清除 */

	u64 base;
	u64 arr_off;
	u32 count;
	u32 stride;
	struct oplus_frame_sync_field fld[OPLUS_FRAME_SYNC_F_TOTAL];

	/* 骨骼链配置（SET_BONE_CFG 时从 zone 拷入，采样用本地副本避免采样中被前端改） */
	struct oplus_frame_sync_bone_cfg bone_cfg;

	/* ESP 全链配置（SET_ESP_CFG 时从 zone 拷入；l0_off != 0 即进入全链模式） */
	struct oplus_frame_sync_esp_cfg esp_cfg;

	/* ESP 全链采样状态：候选 actor、与 ents 输出对齐的骨骼 actor、名字 ps 缓存 */
	u64 acts[OPLUS_FRAME_SYNC_ACT_MAX];
	u64 bone_acts[OPLUS_FRAME_SYNC_ENTITY_MAX];
	u32 bone_act_num;
	u64 name_ps[OPLUS_FRAME_SYNC_ENTITY_MAX];
	u32 name_rr;

	/* ESP 全链批量读暂存（lazy 申请，detach 释放；串行采样单例安全） */
	u8 *scratch;

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

static struct oplus_frame_sync_bone_cfg *oplus_frame_sync_shm_bonecfg(struct oplus_frame_sync_dev *d)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, bone_cfg);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_bone_cfg));
}

static struct oplus_frame_sync_esp_cfg *oplus_frame_sync_shm_espcfg(struct oplus_frame_sync_dev *d)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, esp_cfg);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_esp_cfg));
}

static struct oplus_frame_sync_name *oplus_frame_sync_shm_name(struct oplus_frame_sync_dev *d, u32 i)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, name) +
		     (size_t)(i % OPLUS_FRAME_SYNC_ENTITY_MAX) * sizeof(struct oplus_frame_sync_name);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_name));
}

static struct oplus_frame_sync_bone *oplus_frame_sync_shm_bone(struct oplus_frame_sync_dev *d, u32 buf, u32 i)
{
	size_t off = offsetof(struct oplus_frame_sync_zone, bone) +
		     ((size_t)buf * OPLUS_FRAME_SYNC_ENTITY_MAX + i) * sizeof(struct oplus_frame_sync_bone);
	return oplus_frame_sync_shm_ptr(d, off, sizeof(struct oplus_frame_sync_bone));
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

	if (d->scratch) {
		kvfree(d->scratch);
		d->scratch = NULL;
	}
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
	memset(&d->bone_cfg, 0, sizeof(d->bone_cfg));
	memset(&d->esp_cfg, 0, sizeof(d->esp_cfg));
	memset(d->name_ps, 0, sizeof(d->name_ps));
	d->name_rr = 0;
	d->bone_act_num = 0;
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
		/* 回待命：释放游戏 mm，保留共享池/配置/rate；
		 * want_pid 刻意保留（音量- 唤醒依据，见 工单_待命唤醒与UI状态_v1.md D1） */
		oplus_frame_sync_shm_target_put(d);
		d->stage = OPLUS_FRAME_SYNC_STAGE_STANDBY;
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

	case OPLUS_FRAME_SYNC_CMD_PAUSE:
		/* 暂停采样：保留目标 mm 与配置，SET_RATE>0 / START 秒恢复 */
		if (d->stage == OPLUS_FRAME_SYNC_STAGE_RUNNING)
			d->stage = OPLUS_FRAME_SYNC_STAGE_PAUSED;
		break;

	case OPLUS_FRAME_SYNC_CMD_DETACH:
		/* 前端主动断开共享池：kthread 下轮做 target_put + unpin */
		d->want_pid = 0;
		d->want_detach = true;
		break;

	case OPLUS_FRAME_SYNC_CMD_SET_BONE_CFG: {
		/* 前端已写好 zone.bone_cfg，这里拷入本地副本供采样使用 */
		struct oplus_frame_sync_bone_cfg *bc = oplus_frame_sync_shm_bonecfg(d);
		if (bc)
			memcpy(&d->bone_cfg, bc, sizeof(d->bone_cfg));
		break;
	}

	case OPLUS_FRAME_SYNC_CMD_SET_ESP_CFG: {
		/* 前端已写好 zone.esp_cfg，这里拷入本地副本供采样使用 */
		struct oplus_frame_sync_esp_cfg *ec = oplus_frame_sync_shm_espcfg(d);
		if (ec)
			memcpy(&d->esp_cfg, ec, sizeof(d->esp_cfg));
		break;
	}

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

/* 骨骼采样：对每个 actor 读 mesh→bd→关节数组 + ctw，写 bone 区。
 * 只读原始数据（局部坐标 + ctw），不做四元数浮点换算——换算放用户态。
 * joints/ctw 按 factor 位变换写入（slot 64..117），前端用同 slot 反变换。
 * 关节数组一次整读（B2），再按索引取 14 关节——替代逐关节 14 次小读。
 */
static void oplus_frame_sync_shm_sample_bone_one(struct oplus_frame_sync_dev *d,
						 struct oplus_frame_sync_zone *z,
						 u32 buf, u32 i, u64 actor)
{
	struct oplus_frame_sync_bone_cfg *cfg = &d->bone_cfg;
	struct oplus_frame_sync_bone *b = oplus_frame_sync_shm_bone(d, buf, i);
	u64 mesh = 0, bd = 0;
	u32 bn = 0;
	u32 ctw_raw[12];
	u32 rd, k, m;

	if (!b) {
		d->errors++;
		return;
	}
	memset(b, 0, sizeof(*b));

	if (oplus_frame_sync_read_kbuf(d->tg_mm, actor + cfg->mesh_off,
				       &mesh, sizeof(mesh)) <= 0)
		return;
	if (!mesh)
		return;

	if (oplus_frame_sync_read_kbuf(d->tg_mm, mesh + cfg->bd_off,
				       &bd, sizeof(bd)) <= 0)
		return;
	if (oplus_frame_sync_read_kbuf(d->tg_mm, mesh + cfg->bn_off,
				       &bn, sizeof(bn)) <= 0)
		return;
	if (!bd || bn < cfg->bn_min || bn > cfg->bn_max)
		return;
	if (!cfg->joint_stride || cfg->joint_stride > 64)
		return;
	/* u64 乘法防溢出；数组整读大小不得超暂存区 */
	if ((u64)bn * cfg->joint_stride > OPLUS_FRAME_SYNC_SCRATCH_SIZE)
		return;
	rd = bn * cfg->joint_stride;

	/* ctw 原始 48B（quat4 + trans3 + scale3 + 2pad） */
	memset(ctw_raw, 0, sizeof(ctw_raw));
	if (oplus_frame_sync_read_kbuf(d->tg_mm, mesh + cfg->ctw_off,
				       ctw_raw, sizeof(ctw_raw)) <= 0)
		return;

	/* B2：整读骨骼数组一次，再取 14 关节（需 scratch 已分配） */
	if (!d->scratch)
		return;
	if (oplus_frame_sync_read_kbuf(d->tg_mm, bd, d->scratch, rd) <= 0)
		return;

	for (m = 0; m < 12; m++)
		b->ctw[m] = ctw_raw[m] ^ oplus_frame_sync_factor_of(z->hdr.factor, 106 + m);

	for (k = 0; k < OPLUS_FRAME_SYNC_BONE_JOINTS; k++) {
		u8 ji = cfg->joint_idx[k];
		u32 jv[3] = { 0 };
		u32 j;
		u64 jo;

		if (ji >= bn)
			continue;
		/* u64 加法防配置异常导致暂存区越界读 */
		jo = (u64)ji * cfg->joint_stride + cfg->joint_data_off;
		if (jo + sizeof(jv) > rd)
			continue;
		memcpy(jv, d->scratch + jo, sizeof(jv));
		for (j = 0; j < 3; j++)
			b->joints[k][j] = jv[j] ^ oplus_frame_sync_factor_of(z->hdr.factor, 64 + k * 3 + j);
	}
	b->ok = 1;
}

static void oplus_frame_sync_shm_sample_bone(struct oplus_frame_sync_dev *d,
					     struct oplus_frame_sync_zone *z,
					     u32 buf)
{
	struct oplus_frame_sync_bone_cfg *cfg = &d->bone_cfg;
	u32 i;

	if (!cfg->mesh_off || !cfg->bd_off)
		return;	/* 未配置骨骼链 */
	if (!d->scratch) {
		d->scratch = kvzalloc(OPLUS_FRAME_SYNC_SCRATCH_SIZE, GFP_KERNEL);
		if (!d->scratch)
			return;
	}

	if (d->esp_cfg.l0_off) {
		/* ESP 全链模式：actor 与 ents 输出索引对齐，由 sample_esp 提供 */
		for (i = 0; i < d->bone_act_num && i < OPLUS_FRAME_SYNC_ENTITY_MAX; i++) {
			if (!d->bone_acts[i])
				continue;
			oplus_frame_sync_shm_sample_bone_one(d, z, buf, i,
							    d->bone_acts[i]);
		}
		return;
	}

	/* 旧模式：SET_PARAMS 下发的指针数组逐个取 actor */
	for (i = 0; i < d->count && i < OPLUS_FRAME_SYNC_ENTITY_MAX; i++) {
		u64 actor = 0;

		if (oplus_frame_sync_read_kbuf(d->tg_mm,
					       d->base + d->arr_off + (u64)i * d->stride,
					       &actor, sizeof(actor)) <= 0) {
			d->errors++;
			continue;
		}
		if (!actor)
			continue;
		oplus_frame_sync_shm_sample_bone_one(d, z, buf, i, actor);
	}
}

/* ------------------------------------------------------------------ *
 * ESP 全链采样（esp_cfg.l0_off != 0 启用）
 *
 * 内核每帧从 libUE4 基址重追整条链（世界会重建，不缓存中间指针）：
 *   base → L0 → pc/W → cam/pawn/lv → dec/cnt → layer → actor → 字段
 * 只读数据，不写目标内存；地址合法性由 read_kbuf 的页表预检兜底。
 * ------------------------------------------------------------------ */
static inline u64 oplus_frame_sync_shm_rd64(struct mm_struct *mm, u64 a)
{
	u64 v = 0;
	oplus_frame_sync_read_kbuf(mm, a, &v, sizeof(v));
	return v;
}

static inline s32 oplus_frame_sync_shm_rd32(struct mm_struct *mm, u64 a)
{
	s32 v = 0;
	oplus_frame_sync_read_kbuf(mm, a, &v, sizeof(v));
	return v;
}

/* float 位型判定（正浮点按位型整数序比较；不碰 FPU） */
static inline bool oplus_frame_sync_f32_isfinite(u32 bits)
{
	return (bits & 0x7F800000u) != 0x7F800000u;
}

static inline bool oplus_frame_sync_f32_pos_le(u32 bits, u32 le_bits)
{
	if (!bits || (bits & 0x80000000u))
		return false;
	if (!oplus_frame_sync_f32_isfinite(bits))
		return false;
	return bits <= le_bits;
}

/* 名字读取：ps → FString（主用，失败走兜底），原始 UTF-16LE 写 name 区（单缓冲） */
static void oplus_frame_sync_shm_name_fill(struct oplus_frame_sync_dev *d,
					   u32 idx, u64 ps)
{
	const struct oplus_frame_sync_esp_cfg *cfg = &d->esp_cfg;
	struct oplus_frame_sync_name *nm = oplus_frame_sync_shm_name(d, idx);
	u64 nptr = 0;
	s32 nnum = 0;
	u32 bytes;

	if (!nm)
		return;
	memset(nm, 0, sizeof(*nm));

	if (ps && cfg->name_ptr_off) {
		nptr = oplus_frame_sync_shm_rd64(d->tg_mm, ps + cfg->name_ptr_off);
		nnum = oplus_frame_sync_shm_rd32(d->tg_mm, ps + cfg->name_cnt_off);
		if (!nptr || nnum <= 0 ||
		    nnum > (s32)(OPLUS_FRAME_SYNC_NAME_DATA / 2)) {
			nptr = oplus_frame_sync_shm_rd64(d->tg_mm, ps + cfg->name_ptr2_off);
			nnum = oplus_frame_sync_shm_rd32(d->tg_mm, ps + cfg->name_cnt2_off);
		}
	}
	if (!nptr || nnum <= 0 || nnum > (s32)(OPLUS_FRAME_SYNC_NAME_DATA / 2)) {
		d->name_ps[idx] = ps;
		return;	/* len = 0，前端显示匿名 */
	}

	bytes = (u32)nnum * 2;
	if (bytes > OPLUS_FRAME_SYNC_NAME_DATA)
		bytes = OPLUS_FRAME_SYNC_NAME_DATA;
	if (oplus_frame_sync_read_kbuf(d->tg_mm, nptr, nm->data, bytes) > 0)
		nm->len = bytes;
	d->name_ps[idx] = ps;
}

static void oplus_frame_sync_shm_sample_esp(struct oplus_frame_sync_dev *d,
					    struct oplus_frame_sync_zone *z,
					    u32 buf, u32 mask)
{
	const struct oplus_frame_sync_esp_cfg *cfg = &d->esp_cfg;
	u64 l0, pc, w1, w, cam, pawn, lv, dec;
	s32 cnt;
	u32 i, out, arr_bytes;

	if (!d->scratch) {
		d->scratch = kvzalloc(OPLUS_FRAME_SYNC_SCRATCH_SIZE, GFP_KERNEL);
		if (!d->scratch)
			return;
	}

	/* 本帧数据无效时按 0 实体发布（断链清屏，不喂陈旧帧） */
	z->hdr.entity_num = 0;
	d->bone_act_num = 0;

	/* top 链（每帧重追，世界重建不缓存） */
	l0 = oplus_frame_sync_shm_rd64(d->tg_mm, d->base + cfg->l0_off);
	if (!l0)
		return;
	pc = oplus_frame_sync_shm_rd64(d->tg_mm, l0 + cfg->pc_off);
	w1 = oplus_frame_sync_shm_rd64(d->tg_mm, l0 + cfg->w1_off);
	w = w1 ? oplus_frame_sync_shm_rd64(d->tg_mm, w1 + cfg->w2_off) : 0;
	if (!pc || !w)
		return;
	cam = oplus_frame_sync_shm_rd64(d->tg_mm, pc + cfg->cam_off);
	pawn = oplus_frame_sync_shm_rd64(d->tg_mm, pc + cfg->pawn_off);
	lv = oplus_frame_sync_shm_rd64(d->tg_mm, w + cfg->lv_off);
	if (!cam || !lv)
		return;

	/* 相机（pos 12B 连读 + pitch/yaw/fov；fov 撕裂帧兜底 96°） */
	if (mask & OPLUS_FRAME_SYNC_FLAG_CAMERA) {
		struct oplus_frame_sync_camera *c = oplus_frame_sync_shm_cam(d, buf);
		u32 pv[3] = { 0 };
		u32 pitch = 0, yaw = 0, fov = 0;

		if (c) {
			memset(c, 0, sizeof(*c));
			oplus_frame_sync_read_kbuf(d->tg_mm, cam + cfg->cam_pos_off,
						   pv, sizeof(pv));
			oplus_frame_sync_read_kbuf(d->tg_mm, cam + cfg->cam_pitch_off,
						   &pitch, sizeof(pitch));
			oplus_frame_sync_read_kbuf(d->tg_mm, cam + cfg->cam_yaw_off,
						   &yaw, sizeof(yaw));
			oplus_frame_sync_read_kbuf(d->tg_mm, cam + cfg->cam_fov_off,
						   &fov, sizeof(fov));
			/* 30°..130° 之外视为撕裂帧，回退 96°（位型整数比较） */
			if ((fov & 0x80000000u) || !oplus_frame_sync_f32_isfinite(fov) ||
			    fov < 0x41F00000u || fov > 0x43020000u)
				fov = 0x42C00000u;
			oplus_frame_sync_shm_store_camera(c, OPLUS_FRAME_SYNC_F_CAM_PX, pv[0], z);
			oplus_frame_sync_shm_store_camera(c, OPLUS_FRAME_SYNC_F_CAM_PY, pv[1], z);
			oplus_frame_sync_shm_store_camera(c, OPLUS_FRAME_SYNC_F_CAM_PZ, pv[2], z);
			oplus_frame_sync_shm_store_camera(c, OPLUS_FRAME_SYNC_F_CAM_RX, pitch, z);
			oplus_frame_sync_shm_store_camera(c, OPLUS_FRAME_SYNC_F_CAM_RY, yaw, z);
			oplus_frame_sync_shm_store_camera(c, OPLUS_FRAME_SYNC_F_CAM_FOV, fov, z);
		}
	}

	/* 实体数组（整读进 scratch，再隔层取 actor） */
	dec = oplus_frame_sync_shm_rd64(d->tg_mm, lv + cfg->dec_off);
	cnt = oplus_frame_sync_shm_rd32(d->tg_mm, lv + cfg->cnt_off);
	if (!dec || cnt <= 0 || cnt > 1024)
		return;
	if (!cfg->arr_stride || cfg->arr_stride > 64)
		return;
	arr_bytes = (u32)cnt * cfg->arr_stride;
	if (arr_bytes > OPLUS_FRAME_SYNC_SCRATCH_SIZE) {
		cnt = OPLUS_FRAME_SYNC_SCRATCH_SIZE / cfg->arr_stride;
		arr_bytes = (u32)cnt * cfg->arr_stride;
	}
	if (oplus_frame_sync_read_kbuf(d->tg_mm, dec, d->scratch, arr_bytes) <= 0) {
		d->errors++;
		return;
	}

	if (cnt > OPLUS_FRAME_SYNC_ACT_MAX)
		cnt = OPLUS_FRAME_SYNC_ACT_MAX;
	for (i = 0; i < (u32)cnt; i++) {
		u64 layer;

		memcpy(&layer, d->scratch + (size_t)i * cfg->arr_stride, sizeof(layer));
		d->acts[i] = layer ?
			oplus_frame_sync_shm_rd64(d->tg_mm, layer + cfg->act_off) : 0;
	}

	out = 0;
	d->bone_act_num = 0;
	if (mask & OPLUS_FRAME_SYNC_FLAG_ENTITY) {
		for (i = 0; i < (u32)cnt && out < OPLUS_FRAME_SYNC_ENTITY_MAX; i++) {
			u64 act = d->acts[i];
			u64 e1, e2, e3, eh, root, ps;
			u32 min_off = 0, max_end = 0, nb;
			u32 hp = 0, hpmax = 0, armor = 0, armormax = 0;
			u32 pos[3] = { 0 };
			u32 team = 0;
			u8 weapon = 0;
			struct oplus_frame_sync_entity *e;

			if (!act || act == pawn)
				continue;
			e1 = oplus_frame_sync_shm_rd64(d->tg_mm, act + cfg->e1_off);
			if (!e1)
				continue;
			e2 = oplus_frame_sync_shm_rd64(d->tg_mm, e1 + cfg->e2_off);
			if (!e2)
				continue;
			e3 = oplus_frame_sync_shm_rd64(d->tg_mm, e2);
			eh = e3 ? e3 : e2;	/* 兜底链 */

			/* B2：eh 属性区间（hp/hpmax/armor/armormax）一次批量读 */
			{
				u32 offs[4] = { cfg->hp_off, cfg->hpmax_off,
						cfg->armor_off, cfg->armormax_off };
				u32 o;

				min_off = offs[0];
				max_end = offs[0] + 4;
				for (o = 1; o < 4; o++) {
					if (offs[o] < min_off)
						min_off = offs[o];
					if (offs[o] + 4 > max_end)
						max_end = offs[o] + 4;
				}
				nb = max_end - min_off;
				if (nb > OPLUS_FRAME_SYNC_EH_BATCH_MAX)
					continue;
				if (oplus_frame_sync_read_kbuf(d->tg_mm, eh + min_off,
							       d->scratch, nb) <= 0)
					continue;
				memcpy(&hp,       d->scratch + (cfg->hp_off - min_off), 4);
				memcpy(&hpmax,    d->scratch + (cfg->hpmax_off - min_off), 4);
				memcpy(&armor,    d->scratch + (cfg->armor_off - min_off), 4);
				memcpy(&armormax, d->scratch + (cfg->armormax_off - min_off), 4);
			}

			/* 过滤：0 < hp <= 300.0f（0x43960000，位型比较） */
			if (!oplus_frame_sync_f32_pos_le(hp, 0x43960000u))
				continue;

			root = oplus_frame_sync_shm_rd64(d->tg_mm, act + cfg->root_off);
			if (!root)
				continue;
			if (oplus_frame_sync_read_kbuf(d->tg_mm, root + cfg->pos_off,
						       pos, sizeof(pos)) <= 0)
				continue;
			if (!oplus_frame_sync_f32_isfinite(pos[0]) ||
			    !oplus_frame_sync_f32_isfinite(pos[1]))
				continue;

			ps = oplus_frame_sync_shm_rd64(d->tg_mm, act + cfg->ps_off);
			if (ps)
				team = (u32)oplus_frame_sync_shm_rd32(d->tg_mm,
								      ps + cfg->team_off);
			oplus_frame_sync_read_kbuf(d->tg_mm, act + cfg->weapon_off,
						   &weapon, sizeof(weapon));

			e = oplus_frame_sync_shm_ent(d, buf, out);
			if (!e) {
				d->errors++;
				break;
			}
			memset(e, 0, sizeof(*e));
			oplus_frame_sync_shm_store_entity(e, OPLUS_FRAME_SYNC_F_X, pos[0], z);
			oplus_frame_sync_shm_store_entity(e, OPLUS_FRAME_SYNC_F_Y, pos[1], z);
			oplus_frame_sync_shm_store_entity(e, OPLUS_FRAME_SYNC_F_Z, pos[2], z);
			oplus_frame_sync_shm_store_entity(e, OPLUS_FRAME_SYNC_F_HP, hp, z);
			oplus_frame_sync_shm_store_entity(e, OPLUS_FRAME_SYNC_F_HP_MAX,
							  hpmax ? hpmax : 0x42C80000u, z);
			oplus_frame_sync_shm_store_entity(e, OPLUS_FRAME_SYNC_F_ARMOR, armor, z);
			e->team = team;
			e->weapon = weapon;

			d->bone_acts[out] = act;
			d->bone_act_num = out + 1;

			/* 名字：ps 变了立即重读；不变则靠轮转慢刷（每帧 1 槽） */
			if (d->name_ps[out] != ps || out == (d->name_rr & 63u))
				oplus_frame_sync_shm_name_fill(d, out, ps);
			out++;
		}
	}
	d->name_rr++;

	/* 清掉本帧未用到槽位的旧名字（防串人） */
	for (i = out; i < OPLUS_FRAME_SYNC_ENTITY_MAX; i++) {
		if (d->name_ps[i]) {
			struct oplus_frame_sync_name *nm = oplus_frame_sync_shm_name(d, i);

			if (nm)
				memset(nm, 0, sizeof(*nm));
			d->name_ps[i] = 0;
		}
	}
	z->hdr.entity_num = out;

	/* 自身数据 → ext 保留槽 28..31（camp/子弹/手持） */
	{
		u32 self_idx[4] = { 28, 29, 30, 31 };
		u64 vals[4] = { 0, (u64)-1, (u64)-1, (u64)-1 };
		u32 tags[4] = { OPLUS_FRAME_SYNC_TAG_SELF_CAMP,
				OPLUS_FRAME_SYNC_TAG_AMMO_CUR,
				OPLUS_FRAME_SYNC_TAG_AMMO_MAX,
				OPLUS_FRAME_SYNC_TAG_SELF_WEAPON };
		u32 types[4] = { OPLUS_FRAME_SYNC_T_S32, OPLUS_FRAME_SYNC_T_S32,
				 OPLUS_FRAME_SYNC_T_S32, OPLUS_FRAME_SYNC_T_U32 };
		u32 k;

		if (pawn) {
			u64 ps_self = oplus_frame_sync_shm_rd64(d->tg_mm,
								pawn + cfg->self_ps_off);
			if (ps_self)
				vals[0] = (u64)(s64)oplus_frame_sync_shm_rd32(d->tg_mm,
						ps_self + cfg->self_camp_off);
			if (cfg->gunbase_off) {
				u64 gb = oplus_frame_sync_shm_rd64(d->tg_mm,
								   pawn + cfg->gunbase_off);
				if (gb) {
					s32 amax = oplus_frame_sync_shm_rd32(d->tg_mm,
									     gb + cfg->ammo_max_off);
					s32 used = oplus_frame_sync_shm_rd32(d->tg_mm,
									     gb + cfg->ammo_used_off);
					if (amax > 0 && amax < 1000 && used >= 0 && used <= amax) {
						vals[1] = (u64)(s64)(amax - used);
						vals[2] = (u64)(s64)amax;
					}
				}
			}
			{
				u8 sw = 0;

				oplus_frame_sync_read_kbuf(d->tg_mm,
							   pawn + cfg->self_weapon_off,
							   &sw, sizeof(sw));
				if (sw)
					vals[3] = sw;
			}
		}
		for (k = 0; k < 4; k++) {
			struct oplus_frame_sync_ext *s =
				oplus_frame_sync_shm_ext(d, buf, self_idx[k]);

			if (!s)
				break;
			s->tag = tags[k];
			s->type = types[k];
			s->value = vals[k];
		}
	}
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

	if (d->esp_cfg.l0_off) {
		/* ESP 全链模式：内核自追整条链，entity/camera/self 一次走完 */
		oplus_frame_sync_shm_sample_esp(d, z, buf, mask);
		if (mask & OPLUS_FRAME_SYNC_FLAG_MBOX)
			oplus_frame_sync_shm_service_mbox(d, buf);
		if (mask & OPLUS_FRAME_SYNC_FLAG_BONE)
			oplus_frame_sync_shm_sample_bone(d, z, buf);
	} else {
		if (mask & OPLUS_FRAME_SYNC_FLAG_ENTITY)
			oplus_frame_sync_shm_sample_entity(d, z, buf);
		if (mask & OPLUS_FRAME_SYNC_FLAG_CAMERA)
			oplus_frame_sync_shm_sample_camera(d, z, buf);
		if (mask & OPLUS_FRAME_SYNC_FLAG_EXT)
			oplus_frame_sync_shm_sample_ext(d, buf);
		if (mask & OPLUS_FRAME_SYNC_FLAG_MBOX)
			oplus_frame_sync_shm_service_mbox(d, buf);
		if (mask & OPLUS_FRAME_SYNC_FLAG_BONE)
			oplus_frame_sync_shm_sample_bone(d, z, buf);
	}

	/* 原子翻页：数据写完再 release 发布索引；frame_num 同步发布帧计数 */
	smp_store_release(&z->hdr.buf_idx, buf);
	d->frames++;
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

	/* 睡到下一周期边界（±5% 抖动窗口，合并唤醒省电）。
	 * 工单 #14b：schedule_hrtimeout_range 在该机 GKI 受保护符号表内
	 * （insmod 报 Protected symbol err -13），改用 usleep_range 实现同等
	 * 高精度睡眠（内部同为 hrtimer，周期 ≥1ms 时精度无差）。 */
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

				/* 状态发布：stage 每次变更后经 drain 落到 header；
				 * k_stamp 每 tick 写（待命/暂停也写）＝内核存活心跳，
				 * 前端 UI 据此显示 运行中/待命/暂停/失联 */
				smp_store_release(&z->hdr.stage, (s32)d->stage);
				smp_store_release(&z->hdr.k_stamp, (s64)ktime_get_ns());

				/* 看门狗：stamp 长时间不更新则停采样 */
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
	/* 工单 #14：线程名贴真机显示刷帧线程 fb_flush（15 字符内，规避内核线程名扫描） */
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

	/* 初始化共享池：factor 随机、flag 默认、stamp 归零；buf_id 最后置位作绑定标志。
	 * 池页由 pin_user_pages_remote 逐页取得，物理上不保证连续，不能从 kaddr[0]
	 * 一次 memset 整个 zone，否则会写入池外相邻物理页。 */
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
	z->hdr.entity_num = 0;
	z->hdr.buf_idx = 0;
	z->hdr.factor = get_random_u32();
	z->hdr.flag = OPLUS_FRAME_SYNC_FLAG_ENTITY | OPLUS_FRAME_SYNC_FLAG_CAMERA;
	z->hdr.stamp = 0;
	z->hdr.frame_num = 0;
	z->hdr.k_stamp = 0;
	z->hdr.buf_id = OPLUS_FRAME_SYNC_BUF_ID;

	/* attach 初始化清零会覆盖用户态预写的归属魔数，这里写回：
	 * 前端心跳每秒也会重写，双保险（见 工单_共享池误绑修复_v1.md D2） */
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

/* 是否已绑定共享池（音量键状态感知用：绑定→唤醒/忽略，未绑定→扫描） */
bool oplus_frame_sync_shm_bound(void)
{
	return oplus_frame_sync_shm_g.kaddr != NULL;
}

/*
 * 音量- 唤醒（工单_待命唤醒与UI状态_v1.md D4）：
 *   已绑定 + STANDBY + want_pid 存活 → 重新挂接游戏进程，恢复 RUNNING
 *   （rate/偏移配置原样保留）；进程已死 → 保持 STANDBY，等前端重发 START。
 * 运行/暂停态调用本函数直接忽略（用户决策：运行中长按 = 忽略）。
 */
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
