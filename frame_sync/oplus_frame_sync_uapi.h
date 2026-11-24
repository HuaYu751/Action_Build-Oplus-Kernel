/* SPDX-License-Identifier: GPL-2.0 */
/*
 * oplus_frame_sync_uapi.h —— 帧同步共享池 内核/用户态共用 ABI v6
 *
 * v6（2026-11-24 扩容 + 双通道隔离）：
 *   - 总池 397312B(388KB) → 6291456B(6MB = 1536 页)
 *   - jobs 两个独立区，各 1024 条 × 1KB（共 2MB），物理隔离：
 *       jobs_main[1024] → 结果写 slots（正式 ESP 出框）
 *       jobs_test[1024] → 结果写 mbox 数据块（测试分析）
 *     hdr.job_num / hdr.test_num 分别计数；内核每帧先跑正式区再跑测试区
 *   - cmd 命令池恢复小尺寸：64 条 × 28B（它是控制命令不是数据通道，
 *     一条 = 命令码 + 4 参数 + 结果，1KB 纯属浪费），区凑整 4KB
 *   - slots 2 份 × 1024 格 × 1KB（每格 data 1008B）
 *   - mbox 重定义为"订阅刷新池"（不再是请求-应答邮箱）：
 *       控制头 256B（magic/sub_count/frame_no）
 *       订阅表 1024 项 × 16B（addr+len，前端填）
 *       数据块 1024 块 × 1KB（块号=订阅序号，内核每帧刷新，块头带 addr）
 *   - 外部工具（UMT 等）定位：前端进程 maps 找 6MB 匿名段
 *     → mbox 区偏移见下，先校验 mbox magic 再解析
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

#define OPLUS_FRAME_SYNC_ABI_VERSION  0x00060000u
#define OPLUS_FRAME_SYNC_BUF_ID        1u

/* 6291456 = 6 MB（1536 页）。必须页对齐：6291456 = 1536 × 4096。 */
#define OPLUS_FRAME_SYNC_MAP_SIZE     (6291456u)

/* 池归属魔数（双因子）：小端字节 = ASCII "OPLGFXS1"。
 * 位于 _rsv_future 起始（0x541000），布局变更时 offsetof 自动跟随。 */
#define OPLUS_FRAME_SYNC_POOL_MAGIC   0x31535846474C504FULL

/* mbox 区魔数：小端字节 = ASCII "HUYUMBOX"。外部工具先校验它再解析。 */
#define OPLUS_FRAME_SYNC_MBOX_MAGIC   0x584F424D59555448ULL

/* ---- 容量上限（v6） ---- */
#define OPLUS_FRAME_SYNC_CMD_RING     64      /* 控制命令条数（实际用不到几条） */
#define OPLUS_FRAME_SYNC_JOB_MAIN     1024    /* 正式段任务条数 → slots */
#define OPLUS_FRAME_SYNC_JOB_TEST     1024    /* 测试段任务条数 → mbox 块 */
#define OPLUS_FRAME_SYNC_JOB_CHAIN    64      /* 单指令链级数上限 */
#define OPLUS_FRAME_SYNC_JOB_SUB      16      /* 单指令子段数上限 */
#define OPLUS_FRAME_SYNC_SLOT_MAX     1024    /* 数据格上限（单份） */
#define OPLUS_FRAME_SYNC_SLOT_FLAG    1023    /* 每份最后一格 = 完成标志格（内核专用） */
#define OPLUS_FRAME_SYNC_SLOT_DATA    1008    /* 每格数据区字节数（1KB - 16B 头） */
#define OPLUS_FRAME_SYNC_MBOX_SUBS    1024    /* 订阅表项数 */
#define OPLUS_FRAME_SYNC_MBOX_BLOCKS  1024    /* 数据块块数 */
#define OPLUS_FRAME_SYNC_MBOX_DATA    1008    /* 单块数据区字节数 */
#define OPLUS_FRAME_SYNC_JOB_RETRY_MAX 2      /* 子段重试上限，总尝试 = 1 + retry */

/* 元素尺寸（静态断言锁死） */
#define OPLUS_FRAME_SYNC_JOB_SIZE     1024
#define OPLUS_FRAME_SYNC_SLOT_SIZE    1024
#define OPLUS_FRAME_SYNC_MBLOCK_SIZE  1024
#define OPLUS_FRAME_SYNC_MBOX_HDR_SIZE 256
#define OPLUS_FRAME_SYNC_SUB_SIZE     16

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
#define OPLUS_FRAME_SYNC_CMD_PAUSE         0x0B
#define OPLUS_FRAME_SYNC_CMD_DETACH        0x0C

/* flag 位（仅 MBOX 生效：任务区自己决定读什么） */
#define OPLUS_FRAME_SYNC_FLAG_MBOX    (1u << 3)

/* job.flags 位 */
#define JOB_FLAG_NO_OK   (1u << 0)   /* 只写 data 不写 ok/len/valid（多块共用一块的附属块） */

/*
 * 指令（1KB，前端写进任务区，内核每帧现取现执行）
 *
 * 一条指令 = 从模块基址出发走 chain_len 级指针链 → 从 read_off 连续读
 * read_len 字节 → 写进 slot 格（正式区）或 mbox 数据块（测试区）。
 *
 * 结果路由按区自动决定：
 *   jobs_main[i] → slot 格（slot 字段 = 格号 0..SLOT_FLAG-1）
 *   jobs_test[i] → mbox 数据块（slot 字段 = 块号 0..MBOX_BLOCKS-1）
 *
 * 失败降级（sub_count > 0）：整块读失败 → 按 sub_off/sub_len 逐子段补读，
 * 每子段最多 1+retry 次；成功子段真值保留、失败子段填 0。
 * 链走失败：数据区清零，状态标失败（不沿用旧值）。
 */
struct oplus_frame_sync_job {
	__u32 chain_len;       /* 指针链级数 0..JOB_CHAIN */
	__u32 chain[OPLUS_FRAME_SYNC_JOB_CHAIN];

	__u32 read_off;        /* 走完链后再偏移多少开始读（整块起点） */
	__u32 read_len;        /* 整块读多少字节（1..SLOT_DATA / MBOX_DATA） */
	__u32 slot;            /* 正式区=格号；测试区=mbox 块号 */
	__u32 slot_off;        /* 写入 data[] 的偏移（多块共用一块时用） */

	__u32 sub_count;       /* 0 = 不拆（整块成败）；1..JOB_SUB = 子段数 */
	__u32 sub_off[OPLUS_FRAME_SYNC_JOB_SUB];
	__u32 sub_len[OPLUS_FRAME_SYNC_JOB_SUB];

	__u32 retry;           /* 每子段重试 0..JOB_RETRY_MAX */
	__u32 flags;           /* JOB_FLAG_* */
	__u8  _rsv[608];       /* 补齐 1KB */
};

/*
 * 数据格（1KB，正式 ESP 结果）
 *   ok=1 → 整块成功；ok=0,valid≠0 → 部分子段有效；ok=0,valid=0 → 整块失败
 */
struct oplus_frame_sync_slot {
	__u32 ok;              /* 1 = 整块完全成功；0 = 部分或全部失败 */
	__u32 len;             /* data 有效长度（= read_len） */
	__u32 valid;           /* 子段有效性位图：bit s = 子段 s 有效 */
	__u32 _rsv;
	__u8  data[OPLUS_FRAME_SYNC_SLOT_DATA];
};

/* 控制命令（28B，一条 = 命令码 + 4 参数 + 结果；不是数据通道） */
struct oplus_frame_sync_cmd {
	__u32 cmd;
	__s32 p0, p1, p2, p3;
	__u32 result;       /* 内核回填：0 = OK，负值 = -errno */
	__u32 _rsv;
};

/* ---- mbox 订阅刷新池（v6 重定义，测试期专用） ---- */

/* 控制头 256B：内核写，外部工具先校验 magic 再解析 */
struct oplus_frame_sync_mbox_hdr {
	__u64 magic;        /* = OPLUS_FRAME_SYNC_MBOX_MAGIC */
	__u32 version;      /* 结构版本，当前 1 */
	__u32 sub_count;    /* 有效订阅数 0..MBOX_SUBS */
	__u32 frame_no;     /* 内核刷新到第几帧（UMT 用它判断数据新旧） */
	__u32 flags;        /* 保留 */
	__u8  _rsv[232];
};

/* 订阅项 16B：前端填（写哪些地址要每帧监控） */
struct oplus_frame_sync_sub {
	__u64 addr;         /* 游戏内存地址 */
	__u32 len;          /* 要读多少字节 ≤ MBOX_DATA */
	__u32 flags;        /* 保留 */
};

/* 数据块 1KB：内核每帧刷新；块号 = 订阅表序号 */
struct oplus_frame_sync_mblock {
	__u64 addr;         /* 来源地址（= 订阅表对应项的 addr，便于工具反查） */
	__u32 len;          /* 有效字节数 */
	__u32 status;       /* 0 = 空；1 = 成功；2 = 读失败 */
	__u8  data[OPLUS_FRAME_SYNC_MBOX_DATA];
};

/* ---- header 64B ---- */
struct oplus_frame_sync_hdr {
	__s32 buf_id;       /* 缓冲标识 */
	__s32 cmd_num;      /* 命令计数（app 写，内核 load_acquire 读） */
	__s32 stage;        /* 阶段 */
	__s32 test_num;     /* v6：测试区任务条数（jobs_test），内核每帧 acquire */
	__s32 buf_idx;      /* 当前可读缓冲索引 0/1（内核 store_release 翻转） */
	__s32 factor;       /* 保留字段（数据不混淆，恒 0） */
	__s32 flag;         /* 标志（仅 FLAG_MBOX 有效） */
	__s32 job_num;      /* 正式区任务条数（jobs_main），内核每帧 acquire */
	__s64 stamp;        /* 时间戳（纳秒，app 每轮写，内核看门狗用） */
	__s32 frame_num;    /* 采样帧计数 */
	__s64 k_stamp;      /* 内核存活心跳（纳秒） */
	__s32 zone_enable;  /* 双区开关：bit0=正式区(jobs_main→slots)；bit1=测试区(jobs_test→mbox)。默认 3=都开，0=全停 */
	__s32 _pad[1];      /* 补齐 64B */
};

/* zone_enable 位 */
#define OPLUS_FRAME_SYNC_ZONE_MAIN   (1u << 0)   /* 正式区使能 */
#define OPLUS_FRAME_SYNC_ZONE_TEST   (1u << 1)   /* 测试区使能 */

/*
 * 内存布局（总 6291456B = 6MB = 1536 页）：
 *   hdr        64B     @0x000000
 *   cmds 区    4KB     @0x000040   (cmds[64] 1792B + 填充 2304B，凑整页)
 *   jobs_main  1MB     @0x001000   (1024 × 1KB；结果写 slots)
 *   jobs_test  1MB     @0x101000   (1024 × 1KB；结果写 mbox 数据块)
 *   mbox 区    1.25MB  @0x201000
 *     mbox_hdr   256B   @0x201000
 *     subs       16KB   @0x201100  (1024 × 16B 订阅表)
 *     mblocks    1MB    @0x205100  (1024 × 1KB 数据块)
 *     _mbox_rsv  240KB  @0x305100  (填充到 1.25MB)
 *   slots      2MB     @0x341000   (2 份 × 1024 格 × 1KB；每份第 1023 格 = 标志)
 *   rsv_future 782336B @0x541000   (主池魔数在此；_rsv_future[97792])
 *   合计       6MB     @0x600000
 */
struct oplus_frame_sync_zone {
	/* ---- header 64B ---- */
	struct oplus_frame_sync_hdr hdr;

	/* ---- 命令池（4032B：cmds[64] 1792B + 填充 2240B，收到 0x1000 页边界） ---- */
	struct oplus_frame_sync_cmd cmds[OPLUS_FRAME_SYNC_CMD_RING];
	__u8 _cmds_pad[4032 - OPLUS_FRAME_SYNC_CMD_RING * 28];

	/* ---- 正式任务区 1MB（结果写 slots） ---- */
	struct oplus_frame_sync_job jobs_main[OPLUS_FRAME_SYNC_JOB_MAIN];

	/* ---- 测试任务区 1MB（结果写 mbox 数据块） ---- */
	struct oplus_frame_sync_job jobs_test[OPLUS_FRAME_SYNC_JOB_TEST];

	/* ---- mbox 订阅刷新池（测试期专用，1.25MB） ---- */
	struct oplus_frame_sync_mbox_hdr mbox_hdr;
	struct oplus_frame_sync_sub      subs[OPLUS_FRAME_SYNC_MBOX_SUBS];
	struct oplus_frame_sync_mblock   mblocks[OPLUS_FRAME_SYNC_MBOX_BLOCKS];
	__u64 _mbox_rsv[30688];   /* 245504B：mbox 区凑整 1.25MB，将来扩块数用 */

	/* ---- 统一数据格（2MB，A/B 双缓冲，每份最后一格 = 完成标志） ---- */
	struct oplus_frame_sync_slot slots[2][OPLUS_FRAME_SYNC_SLOT_MAX];

	__u64 _rsv_future[97792];   /* 782336B：归属魔数在起始（offsetof 自动跟随布局） */

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
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_job) == 1024,
		       "uapi: job must be 1KB");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_slot) == 1024,
		       "uapi: slot must be 1KB");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_mbox_hdr) == 256,
		       "uapi: mbox_hdr must be 256B");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_sub) == 16,
		       "uapi: sub must be 16B");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_mblock) == 1024,
		       "uapi: mblock must be 1KB");
OPLUS_FS_STATIC_ASSERT(sizeof(struct oplus_frame_sync_zone) == OPLUS_FRAME_SYNC_MAP_SIZE,
		       "uapi: zone size must equal MAP_SIZE");
OPLUS_FS_STATIC_ASSERT(OPLUS_FRAME_SYNC_MAGIC_OFF == 0x541000u,
		       "uapi: magic offset drifted");
OPLUS_FS_STATIC_ASSERT(offsetof(struct oplus_frame_sync_zone, jobs_main) == 0x1000u,
		       "uapi: jobs_main offset drifted");
OPLUS_FS_STATIC_ASSERT(offsetof(struct oplus_frame_sync_zone, jobs_test) == 0x101000u,
		       "uapi: jobs_test offset drifted");
OPLUS_FS_STATIC_ASSERT(offsetof(struct oplus_frame_sync_zone, mbox_hdr) == 0x201000u,
		       "uapi: mbox_hdr offset drifted");
OPLUS_FS_STATIC_ASSERT(offsetof(struct oplus_frame_sync_zone, subs) == 0x201100u,
		       "uapi: subs offset drifted");
OPLUS_FS_STATIC_ASSERT(offsetof(struct oplus_frame_sync_zone, mblocks) == 0x205100u,
		       "uapi: mblocks offset drifted");
OPLUS_FS_STATIC_ASSERT(offsetof(struct oplus_frame_sync_zone, slots) == 0x341000u,
		       "uapi: slots offset drifted");

#endif /* _OPLUS_FRAME_SYNC_UAPI_H */
