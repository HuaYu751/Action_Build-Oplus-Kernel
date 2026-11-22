/* SPDX-License-Identifier: GPL-2.0 */
/*
 * oplus_frame_sync_uapi.h —— 帧同步共享池 内核/用户态共用 ABI v5
 *
 * v5（2026-11-22 通用内核工单 v3）：统一指令形式，内核退化为纯执行器——
 * 前端把指令直接写进共享池任务区，内核每帧现取现执行，全程零游戏语义。
 *   - 一条指令 = 一条链(≤12级) + 读偏移 + 读长度 + 写第几格
 *   - 删：BASE/ARRAY/PREV 三种起点、kind、arr_idx、内核私有任务表、pcache
 *   - 删：CMD_SET_JOBS（任务表不再拷贝入内核，内核从池里现取）
 *   - JOB_MAX 64 → 256（骨骼每人 15 条指令：ctw + 14 关节）
 *   - 每份 A/B 的最后一格（第 511 格）为完成标志格：内核写完全部指令后
 *     在该格写 ok=1 + 帧号，前端见标志才收，收完把自己读过的格清零
 *   - MAP_SIZE 282624 → 299008（292KB = 73 页，非 2 的幂避开分配撞车）
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

#define OPLUS_FRAME_SYNC_ABI_VERSION  0x00050000u
#define OPLUS_FRAME_SYNC_BUF_ID        1u

/* 299008 = 292KB（73 页）：512 格 × 256B × A/B 双份 + 头/任务区/邮箱。
 * 非 2 的幂，避开 bionic/Scudo 恰好 256KB 的匿名分配撞车（沿用 D1 教训）。
 * 必须页对齐：299008 = 73 × 4096。 */
#define OPLUS_FRAME_SYNC_MAP_SIZE     (299008u)

/* 池归属魔数（D2 双因子）：小端字节 = ASCII "OPLGFXS1"。
 * 位于 _rsv_future 起始（297472），布局变更时 offsetof 自动跟随。 */
#define OPLUS_FRAME_SYNC_POOL_MAGIC   0x31535846474C504FULL

/* ---- 容量上限（工单 v3） ---- */
#define OPLUS_FRAME_SYNC_JOB_MAX      256     /* 指令条数上限（骨骼每人 15 条） */
#define OPLUS_FRAME_SYNC_JOB_CHAIN    12      /* 单指令链级数上限（骨骼最深 9 级） */
#define OPLUS_FRAME_SYNC_JOB_SUB      6       /* 单指令子段数上限 */
#define OPLUS_FRAME_SYNC_SLOT_MAX     512     /* 数据格上限（单份） */
#define OPLUS_FRAME_SYNC_SLOT_FLAG    511     /* 每份最后一格 = 完成标志格（内核专用） */
#define OPLUS_FRAME_SYNC_SLOT_DATA    240     /* 每格数据区字节数 */
#define OPLUS_FRAME_SYNC_CMD_RING     16
#define OPLUS_FRAME_SYNC_MBOX_SLOTS   8
#define OPLUS_FRAME_SYNC_MBOX_DATA    232     /* 单槽 256B：24B 头 + 232B 数据 */
#define OPLUS_FRAME_SYNC_JOB_RETRY_MAX 2      /* 子段重试上限，总尝试 = 1 + retry */

/* 链每级翻页失败原地重试次数（内核固定能力，前端不可配） */
#define OPLUS_FRAME_SYNC_WALK_RETRY   3

/* 完成标志格 data 内容：data[0..4)=帧号 data[4..8)=FLAG_MAGIC */
#define OPLUS_FRAME_SYNC_FLAG_MAGIC   0x47414C46u   /* "FLAG" 小端 */

/* 采样阶段 */
#define OPLUS_FRAME_SYNC_STAGE_IDLE    0
#define OPLUS_FRAME_SYNC_STAGE_RUNNING 1
#define OPLUS_FRAME_SYNC_STAGE_PAUSED  2
#define OPLUS_FRAME_SYNC_STAGE_STANDBY 3

/* 命令码 */
#define OPLUS_FRAME_SYNC_CMD_START         0x01   /* p0 = 目标 pid */
#define OPLUS_FRAME_SYNC_CMD_STOP          0x02
#define OPLUS_FRAME_SYNC_CMD_SET_RATE      0x03   /* p0 = Hz (0..165) */
#define OPLUS_FRAME_SYNC_CMD_SET_PARAMS    0x04   /* p0/p1 = 模块基址低/高 32 位（仅取 base） */
#define OPLUS_FRAME_SYNC_CMD_RESET         0x06
#define OPLUS_FRAME_SYNC_CMD_SET_FACTOR    0x07   /* 保留：数据不混淆，命令空转兼容 */
#define OPLUS_FRAME_SYNC_CMD_SET_FLAG      0x08   /* p0 = flag（仅 FLAG_MBOX 有效） */
#define OPLUS_FRAME_SYNC_CMD_MBOX_READ     0x0A   /* p0 = 邮箱槽（0..7） */
#define OPLUS_FRAME_SYNC_CMD_PAUSE         0x0B
#define OPLUS_FRAME_SYNC_CMD_DETACH        0x0C

/* flag 位（仅 MBOX 生效：任务区自己决定读什么） */
#define OPLUS_FRAME_SYNC_FLAG_MBOX    (1u << 3)

/* job.flags 位 */
#define JOB_FLAG_NO_OK   (1u << 0)   /* 只写 data 不写 ok/len/valid（多块共用一格的附属块） */

/*
 * 指令（128B，前端写进池任务区，内核每帧现取现执行）
 *
 * 一条指令 = 从模块基址出发走 chain_len 级指针链 → 从 read_off 连续读
 * read_len 字节 → 写进 slot 格的 data[slot_off]。
 *
 * 链上每一级 = 一次「读指针」：ptr = *(u64*)(ptr + chain[k])。
 * 动态下标（名单第 i 号）由前端算成普通偏移写进链（如 i*8），内核无感知。
 *
 * 失败降级（sub_count > 0）：整块读失败 → 按 sub_off/sub_len 逐子段补读，
 * 每子段最多 1+retry 次（成功就停）；成功子段真值保留、失败子段填 0，
 * 拼成一整块写入，ok=0 且 valid 位图标明哪些子段有效。
 * 链走失败（任何一级 3 次重试仍拿不到非零指针）：
 * 数据区清零，ok=0/len=0/valid=0（没有就没有，不沿用旧值）。
 */
struct oplus_frame_sync_job {
	__u32 chain_len;       /* 指针链级数 0..JOB_CHAIN */
	__u32 chain[OPLUS_FRAME_SYNC_JOB_CHAIN];

	__u32 read_off;        /* 走完链后再偏移多少开始读（整块起点） */
	__u32 read_len;        /* 整块读多少字节（1..SLOT_DATA） */
	__u32 slot;            /* 写到池子第几格（0..SLOT_FLAG-1，标志格内核专用） */
	__u32 slot_off;        /* 写入格内 data[] 的偏移（多块共用一格时用） */

	__u32 sub_count;       /* 0 = 不拆（整块成败）；1..JOB_SUB = 子段数 */
	__u32 sub_off[OPLUS_FRAME_SYNC_JOB_SUB];
	__u32 sub_len[OPLUS_FRAME_SYNC_JOB_SUB];

	__u32 retry;           /* 每子段重试 0..JOB_RETRY_MAX */
	__u32 flags;           /* JOB_FLAG_* */
};

/*
 * 数据格（256B，不跨页）
 *
 * ok/valid 三种状态（v5 前端不沿用上一帧）：
 *   ok=1         → 整块成功，全用新数据
 *   ok=0,valid≠0 → 部分子段成功：只有位图为 1 的段是本帧真值，其余段不用
 *   ok=0,valid=0 → 整块失败：本帧该格作废（数据区已被内核清零）
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
	__s32 _rsv0;        /* v4 entity_num 已废（人数是前端指令自己的事） */
	__s32 buf_idx;      /* 当前可读缓冲索引 0/1（内核 store_release 翻转） */
	__s32 factor;       /* 保留字段（数据不混淆，恒 0） */
	__s32 flag;         /* 标志（仅 FLAG_MBOX 有效） */
	__s32 job_num;      /* 任务区有效条数（app 写 release，内核每帧 acquire 现读） */
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
 * 内存布局（总 299008B = 292KB = 73 页）：
 *   header 64B → cmds 448B → jobs[256] 32KB → mbox 2KB(@33280)
 *   → slots[2][512] 256KB(@35328) → rsv_future 1536B(@297472，魔数在此)
 */
struct oplus_frame_sync_zone {
	/* ---- header 64B ---- */
	struct oplus_frame_sync_hdr hdr;

	/* ---- 命令池 ---- */
	struct oplus_frame_sync_cmd cmds[OPLUS_FRAME_SYNC_CMD_RING];

	/* ---- 任务区（前端写，内核每帧现取现执行，无本地副本） ---- */
	struct oplus_frame_sync_job jobs[OPLUS_FRAME_SYNC_JOB_MAX];

	/* ---- 一次性读邮箱（测试期探地址用） ---- */
	struct oplus_frame_sync_mbox mbox[OPLUS_FRAME_SYNC_MBOX_SLOTS];

	/* ---- 统一数据格（A/B 双缓冲，每份最后一格 = 完成标志） ---- */
	struct oplus_frame_sync_slot slots[2][OPLUS_FRAME_SYNC_SLOT_MAX];

	__u64 _rsv_future[192];   /* 1536B：归属魔数在起始（offsetof 自动跟随布局） */

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
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_job) == 128,
		       "uapi: job must be 128B");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_slot) == 256,
		       "uapi: slot must be 256B");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_mbox) == 256,
		       "uapi: mbox must be 256B");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_zone) == OPLUS_FRAME_SYNC_MAP_SIZE,
		       "uapi: zone size must equal MAP_SIZE");
OPLUS_FS_STATIC_ASSERT(OPLUS_FRAME_SYNC_MAGIC_OFF == 297472u,
		       "uapi: magic offset drifted");
OPLUS_FS_STATIC_ASSERT(offsetof(struct oplus_frame_sync_zone, mbox) == 33280u,
		       "uapi: mbox offset drifted");
OPLUS_FS_STATIC_ASSERT(offsetof(struct oplus_frame_sync_zone, slots) == 35328u,
		       "uapi: slots offset drifted");

#endif /* _OPLUS_FRAME_SYNC_UAPI_H */
