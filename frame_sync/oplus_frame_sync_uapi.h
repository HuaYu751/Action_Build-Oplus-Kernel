/* SPDX-License-Identifier: GPL-2.0 */
/*
 * oplus_frame_sync_uapi.h —— 帧同步共享池 内核/用户态共用 ABI v4
 *
 * v4（2026-11-22 通用内核工单 v2）：游戏语义全部搬前端，内核退化为
 * 纯任务执行器——按前端下发的任务表追指针链、读 N 字节、写池子第 X 格。
 *   - 新结构：job（任务表）/ slot（统一数据格，ok + valid 位图 + 240B 原始字节）
 *   - 删全部游戏专用区：ents/cam/ext/bone/name/bone_cfg/esp_cfg/aim/trace
 *   - MAP_SIZE 258048 → 282624（276KB = 69 页，非 2 的幂避开分配撞车）
 *   - 前端 mmap 尺寸、内核扫描匹配、魔数偏移全部经本头文件宏自动跟随
 *
 * 命名与布局对齐原厂 frame_boost 风格：
 *   - 标识用 _id、计数用 _num、状态用 stage、时间戳用 stamp
 *   - ABI 版本走编译期宏 OPLUS_FRAME_SYNC_ABI_VERSION
 */

#ifndef _OPLUS_FRAME_SYNC_UAPI_H
#define _OPLUS_FRAME_SYNC_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stddef.h>
#if defined(__has_include)
#if __has_include(<linux/types.h>)
#include <linux/types.h>
#define __OPLUS_FRAME_SYNC_UTYPES
#endif
#endif
#ifndef __OPLUS_FRAME_SYNC_UTYPES
#define __OPLUS_FRAME_SYNC_UTYPES
typedef unsigned char       __u8;
typedef unsigned short      __u16;
typedef unsigned int        __u32;
typedef signed int          __s32;
typedef unsigned long long  __u64;
typedef signed long long    __s64;
#endif
#endif

#define OPLUS_FRAME_SYNC_ABI_VERSION  0x00040000u
#define OPLUS_FRAME_SYNC_BUF_ID        1u

/* 282624 = 276KB（69 页）：512 格 × 256B × A/B 双份 + 头/任务表/邮箱。
 * 非 2 的幂，避开 bionic/Scudo 恰好 256KB 的匿名分配撞车（沿用 D1 教训）。
 * 必须页对齐：282624 = 69 × 4096。 */
#define OPLUS_FRAME_SYNC_MAP_SIZE     (282624u)

/* 池归属魔数（D2 双因子）：小端字节 = ASCII "OPLGFXS1"。
 * 位于 _rsv_future 起始（280576），布局变更时 offsetof 自动跟随。 */
#define OPLUS_FRAME_SYNC_POOL_MAGIC   0x31535846474C504FULL

/* ---- 容量上限（工单 v2 第十节） ---- */
#define OPLUS_FRAME_SYNC_JOB_MAX      64      /* 任务条数上限 */
#define OPLUS_FRAME_SYNC_JOB_CHAIN    8       /* 单任务指针链级数上限 */
#define OPLUS_FRAME_SYNC_JOB_SUB      8       /* 单任务子段数上限 */
#define OPLUS_FRAME_SYNC_SLOT_MAX     512     /* 数据格上限（单份） */
#define OPLUS_FRAME_SYNC_SLOT_DATA    240     /* 每格数据区字节数 */
#define OPLUS_FRAME_SYNC_CMD_RING     16
#define OPLUS_FRAME_SYNC_MBOX_SLOTS   8
#define OPLUS_FRAME_SYNC_MBOX_DATA    232     /* 单槽 256B：24B 头 + 232B 数据 */
#define OPLUS_FRAME_SYNC_JOB_RETRY_MAX 2      /* 子段重试上限，总尝试 = 1 + retry */

/* 采样阶段 */
#define OPLUS_FRAME_SYNC_STAGE_IDLE    0
#define OPLUS_FRAME_SYNC_STAGE_RUNNING 1
#define OPLUS_FRAME_SYNC_STAGE_PAUSED  2
#define OPLUS_FRAME_SYNC_STAGE_STANDBY 3

/* 命令码 */
#define OPLUS_FRAME_SYNC_CMD_START         0x01   /* p0 = 目标 pid */
#define OPLUS_FRAME_SYNC_CMD_STOP          0x02
#define OPLUS_FRAME_SYNC_CMD_SET_RATE      0x03   /* p0 = Hz (0..165) */
#define OPLUS_FRAME_SYNC_CMD_SET_PARAMS    0x04   /* p0/p1 = 模块基址低/高 32 位（新路径仅取 base） */
#define OPLUS_FRAME_SYNC_CMD_RESET         0x06
#define OPLUS_FRAME_SYNC_CMD_SET_FACTOR    0x07   /* v4 保留：数据不再混淆，命令空转兼容 */
#define OPLUS_FRAME_SYNC_CMD_SET_FLAG      0x08   /* p0 = flag（仅 FLAG_MBOX 有效） */
#define OPLUS_FRAME_SYNC_CMD_MBOX_READ     0x0A   /* p0 = 邮箱槽（0..7） */
#define OPLUS_FRAME_SYNC_CMD_PAUSE         0x0B
#define OPLUS_FRAME_SYNC_CMD_DETACH        0x0C
#define OPLUS_FRAME_SYNC_CMD_SET_JOBS      0x0F   /* 前端已写 zone.jobs + hdr.job_num，内核拷入本地副本 */

/* flag 位（v4 仅 MBOX 生效：任务表自己决定读什么） */
#define OPLUS_FRAME_SYNC_FLAG_MBOX    (1u << 3)

/* ---- 任务起点来源 ---- */
#define JOB_SRC_BASE     0    /* d->base + base_off，再走 chain */
#define JOB_SRC_ARRAY    1    /* 数组第 i 项（指针），再走 chain（kind=1） */
#define JOB_SRC_PREV     2    /* 第 src_job 任务同轮格子的前 8 字节（取 8B 指针当起点） */

/* job.flags 位 */
#define JOB_FLAG_NO_OK   (1u << 0)   /* 只写 data 不写 ok/len/valid（多块共用一格的附属块） */

/*
 * 任务（192B，前端下发，内核逐条执行）
 *
 * 一条任务 = 解析起点 → 走 chain_len 级指针 → 从 read_off 连续读 read_len
 * 字节 → 写进 slot[slot_base + i*slot_stride] 的 data[slot_off]。
 *
 * 失败降级（sub_count > 0）：整块读失败 → 按 sub_off/sub_len 逐子段补读，
 * 每子段最多 1+retry 次（成功就停）；成功子段真值保留、失败子段填 0，
 * 拼成一整块写入，ok=0 且 valid 位图标明哪些子段有效。
 */
struct oplus_frame_sync_job {
	__u32 kind;            /* 0 = 单次；1 = 按数组循环（arr_job 提供数组） */
	__u32 src;             /* JOB_SRC_BASE / JOB_SRC_ARRAY / JOB_SRC_PREV */
	__u64 base_off;        /* src=BASE 时的起始偏移（相对模块基址） */
	__u32 src_job;         /* src=PREV 时引用的任务号 */

	__u32 chain_len;       /* 指针链级数 0..JOB_CHAIN */
	__u32 chain[OPLUS_FRAME_SYNC_JOB_CHAIN];

	__u32 read_off;        /* 走完链后再偏移多少开始读（整块起点） */
	__u32 read_len;        /* 整块读多少字节（1..SLOT_DATA） */
	__u32 slot_base;       /* 写到池子第几格；循环时是第一格 */
	__u32 slot_stride;     /* 循环时每项占几格（0 视为 1） */
	__u32 slot_off;        /* 写入格内 data[] 的偏移（多块共用一格时用） */

	__u32 arr_job;         /* kind=1：数组元信息任务（其格前 8B=数组地址 +4B=s32 数量） */
	__u32 arr_max;         /* 最多循环几项（硬上限保护） */
	__u32 arr_stride;      /* 数组元素步长（UE 指针数组 = 8） */

	__u32 sub_count;       /* 0 = 不拆（整块成败）；1..8 = 子段数 */
	__u32 sub_off[OPLUS_FRAME_SYNC_JOB_SUB];
	__u32 sub_len[OPLUS_FRAME_SYNC_JOB_SUB];

	__u32 retry;           /* 每子段重试 0..JOB_RETRY_MAX */
	__u32 flags;           /* JOB_FLAG_* */
	__u32 _rsv[7];         /* 补齐 192B（改动此结构必须同步静态断言） */
};

/*
 * 数据格（256B，不跨页）
 *
 * ok/valid 三种状态：
 *   ok=1         → 整块成功，全用新数据
 *   ok=0,valid≠0 → 部分子段成功：有效段用新值，无效段前端沿用上帧
 *   ok=0,valid=0 → 整块失败：前端整块沿用上帧
 */
struct oplus_frame_sync_slot {
	__u32 ok;              /* 1 = 整块完全成功；0 = 部分或全部失败 */
	__u32 len;             /* data 有效长度（= read_len） */
	__u32 valid;           /* 子段有效性位图：bit s = 子段 s 有效 */
	__u32 _rsv;
	__u8  data[OPLUS_FRAME_SYNC_SLOT_DATA];
};

/* ---- header 64B ---- */
struct oplus_frame_sync_hdr {
	__s32 buf_id;       /* 缓冲标识 */
	__s32 cmd_num;      /* 命令计数（app 写，内核 load_acquire 读） */
	__s32 stage;        /* 阶段 */
	__s32 entity_num;   /* 数组实际人数（锚点任务回填，诊断/UI 用） */
	__s32 buf_idx;      /* 当前可读缓冲索引 0/1（内核 store_release 翻转） */
	__s32 factor;       /* v4 保留字段（数据不再混淆，恒 0） */
	__s32 flag;         /* 标志（仅 FLAG_MBOX 有效） */
	__s32 job_num;      /* 任务表有效条数（app 写，内核 SET_JOBS 时读） */
	__s64 stamp;        /* 时间戳（纳秒，app 每轮写，内核看门狗用） */
	__s32 frame_num;    /* 采样帧计数 */
	__s64 k_stamp;      /* 内核存活心跳（纳秒） */
	__s32 _pad[2];      /* 补齐 64B */
};

struct oplus_frame_sync_cmd {
	__u32 cmd;
	__s32 p0, p1, p2, p3;
	__u32 result;       /* 内核回填：0 = OK，负值 = -errno */
	__u32 _rsv;
};

struct oplus_frame_sync_mbox {
	__u32 req_seq;
	__u32 ack_seq;
	__u64 addr;
	__u32 len;
	__u32 status;
	__u8  data[OPLUS_FRAME_SYNC_MBOX_DATA];
};

/*
 * 内存布局（总 282624B = 276KB = 69 页）：
 *   header 64B → cmds 448B → jobs[64] 12288B → pad → mbox 2KB(页对齐@16384)
 *   → slots[2][512] 256KB(@18432) → rsv_future 2KB(@280576，魔数在此)
 */
struct oplus_frame_sync_zone {
	/* ---- header 64B ---- */
	struct oplus_frame_sync_hdr hdr;

	/* ---- 命令池 ---- */
	struct oplus_frame_sync_cmd cmds[OPLUS_FRAME_SYNC_CMD_RING];

	/* ---- 任务表（前端下发，内核 SET_JOBS 拷入本地副本） ---- */
	struct oplus_frame_sync_job jobs[OPLUS_FRAME_SYNC_JOB_MAX];

	__u32 _pad2[896];   /* 3584B：将 mbox 对齐到页边界 16384 */

	/* ---- 一次性读邮箱（测试期探地址用） ---- */
	struct oplus_frame_sync_mbox mbox[OPLUS_FRAME_SYNC_MBOX_SLOTS];

	/* ---- 统一数据格（A/B 双缓冲） ---- */
	struct oplus_frame_sync_slot slots[2][OPLUS_FRAME_SYNC_SLOT_MAX];

	__u64 _rsv_future[256];   /* 2048B：归属魔数在起始（offsetof 自动跟随布局） */

	__u32 _tail[0];
};

#define OPLUS_FRAME_SYNC_MAGIC_OFF \
	((unsigned int)offsetof(struct oplus_frame_sync_zone, _rsv_future))

/* ---- 布局静态断言（内核/前端共用头文件，编译期锁死 ABI） ----
 * 任何结构字段改动导致尺寸变化都会在这里直接编译失败，防止两端布局漂移。 */
#ifdef __cplusplus
#define OPLUS_FS_STATIC_ASSERT static_assert
#else
#define OPLUS_FS_STATIC_ASSERT _Static_assert
#endif

OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_hdr) == 64,
		       "uapi: hdr must be 64B");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_cmd) == 28,
		       "uapi: cmd must be 28B");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_job) == 192,
		       "uapi: job must be 192B");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_slot) == 256,
		       "uapi: slot must be 256B");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_mbox) == 256,
		       "uapi: mbox must be 256B");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_zone) == OPLUS_FRAME_SYNC_MAP_SIZE,
		       "uapi: zone size must equal MAP_SIZE");
OPLUS_FS_STATIC_ASSERT(OPLUS_FRAME_SYNC_MAGIC_OFF == 280576u,
		       "uapi: magic offset drifted");

#endif /* _OPLUS_FRAME_SYNC_UAPI_H */
