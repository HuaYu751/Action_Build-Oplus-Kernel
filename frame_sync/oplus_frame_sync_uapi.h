/* SPDX-License-Identifier: GPL-2.0 */
/*
 * oplus_frame_sync_uapi.h —— 帧同步共享池 内核/用户态共用 ABI
 *
 * 该头文件同时被内核驱动与用户态侧（JNI）包含，任何字段变更必须两边同步，
 * 并递增 OPLUS_FRAME_SYNC_ABI_VERSION。
 *
 * 命名与布局对齐原厂 frame_boost（ofb_ctrl_data / ofb_key_thread_info）风格：
 *   - 标识用 _id、计数用 _num、状态用 stage、时间戳用 stamp、系数用 factor
 *   - 结构体字段不出现 version / magic / seq / sig / salt 等安全协议黑话
 *   - ABI 版本号不落进内存池头部，走编译期宏 OPLUS_FRAME_SYNC_ABI_VERSION
 */
#ifndef _OPLUS_FRAME_SYNC_UAPI_H
#define _OPLUS_FRAME_SYNC_UAPI_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define OPLUS_FRAME_SYNC_ABI_VERSION  0x00030001u

/* 256KB 映射；实际使用不足 16KB，预留余量给后续字段扩展 */
#define OPLUS_FRAME_SYNC_MAP_SIZE     (256u * 1024u)

#define OPLUS_FRAME_SYNC_ENTITY_MAX   64
#define OPLUS_FRAME_SYNC_FIELD_MAX    16
#define OPLUS_FRAME_SYNC_CMD_RING     16
#define OPLUS_FRAME_SYNC_EXT_SLOTS    32
#define OPLUS_FRAME_SYNC_MBOX_SLOTS   8
#define OPLUS_FRAME_SYNC_MBOX_DATA    232   /* 单槽 256B：24B 头 + 232B 数据 */

/* 采样阶段 */
#define OPLUS_FRAME_SYNC_STAGE_IDLE    0
#define OPLUS_FRAME_SYNC_STAGE_RUNNING 1
#define OPLUS_FRAME_SYNC_STAGE_PAUSED  2

/* 命令码（命令池 cmd 字段；稳态控制全走这里，零 ioctl） */
#define OPLUS_FRAME_SYNC_CMD_START         0x01   /* p0 = 目标 pid */
#define OPLUS_FRAME_SYNC_CMD_STOP          0x02
#define OPLUS_FRAME_SYNC_CMD_SET_RATE      0x03   /* p0 = Hz (0..165) */
#define OPLUS_FRAME_SYNC_CMD_SET_PARAMS    0x04   /* p0 = 基址低32 p1 = 基址高32 p2 = 数量 p3 = 数组偏移（步长固定 8） */
#define OPLUS_FRAME_SYNC_CMD_SET_FIELD     0x05   /* p0 = 字段槽 p1 = 偏移 p2 = 类型 */
#define OPLUS_FRAME_SYNC_CMD_RESET         0x06   /* 清空全部配置（软复位） */
#define OPLUS_FRAME_SYNC_CMD_SET_FACTOR    0x07   /* p0 = 新系数（0 = 随机） */
#define OPLUS_FRAME_SYNC_CMD_SET_FLAG      0x08   /* p0 = flag */
#define OPLUS_FRAME_SYNC_CMD_SET_FIELD_EXT 0x09   /* p0 = 扩展槽 p1 = 偏移 p2 = 类型 p3 = tag */
#define OPLUS_FRAME_SYNC_CMD_MBOX_READ     0x0A   /* p0 = 邮箱槽（0..7） */

/* 字段槽（SET_FIELD 的 p0） */
enum oplus_frame_sync_field_slot {
	OPLUS_FRAME_SYNC_F_X = 0,
	OPLUS_FRAME_SYNC_F_Y,
	OPLUS_FRAME_SYNC_F_Z,
	OPLUS_FRAME_SYNC_F_HP,
	OPLUS_FRAME_SYNC_F_HP_MAX,
	OPLUS_FRAME_SYNC_F_ARMOR,
	OPLUS_FRAME_SYNC_F_TEAM,
	OPLUS_FRAME_SYNC_F_CAM_PX,
	OPLUS_FRAME_SYNC_F_CAM_PY,
	OPLUS_FRAME_SYNC_F_CAM_PZ,
	OPLUS_FRAME_SYNC_F_CAM_RX,
	OPLUS_FRAME_SYNC_F_CAM_RY,
	OPLUS_FRAME_SYNC_F_CAM_RZ,
	OPLUS_FRAME_SYNC_F_CAM_FOV,
	OPLUS_FRAME_SYNC_F_BASE_MAX = 16,   /* 0..13 用于实体+相机 */
	OPLUS_FRAME_SYNC_F_EXT_BASE = 16,   /* 16..47 映射到 ext[0..31] */
	OPLUS_FRAME_SYNC_F_TOTAL = 48,
};

/* 字段类型（SET_FIELD 的 p2） */
#define OPLUS_FRAME_SYNC_T_F32  1
#define OPLUS_FRAME_SYNC_T_U32  2
#define OPLUS_FRAME_SYNC_T_S32  3
#define OPLUS_FRAME_SYNC_T_U8   4

/* flag 位定义（app 声明启用哪些采样，内核据此省 CPU） */
#define OPLUS_FRAME_SYNC_FLAG_ENTITY  (1u << 0)
#define OPLUS_FRAME_SYNC_FLAG_CAMERA  (1u << 1)
#define OPLUS_FRAME_SYNC_FLAG_EXT     (1u << 2)
#define OPLUS_FRAME_SYNC_FLAG_MBOX    (1u << 3)

/*
 * 内存布局：header 64B → cmd ring → ents[2][64] → cam[2] → ext[2][32]
 *           → pad → mbox[8] → tail
 * 每个元素要么步长整除 4096，要么所在区起始页对齐，保证不跨页。
 */

/* ---- header 64B ---- */
struct oplus_frame_sync_hdr {
	__s32 buf_id;       /* 缓冲标识 */
	__s32 cmd_num;      /* 命令计数（app 写，内核 load_acquire 读） */
	__s32 stage;        /* 阶段 */
	__s32 entity_num;   /* 实体数量 */
	__s32 buf_idx;      /* 当前可读缓冲索引 0/1（内核 store_release 翻转） */
	__s32 factor;       /* 系数 */
	__s32 flag;         /* 标志 */
	__s32 _pad0;        /* 8 字节对齐 */
	__s64 stamp;        /* 时间戳（纳秒，app 每轮写，内核据此判断存活） */
	__s32 _pad[6];      /* 补齐 64B */
};

struct oplus_frame_sync_cmd {
	__u32 cmd;
	__s32 p0, p1, p2, p3;
	__u32 result;       /* 内核回填：0 = OK，负值 = -errno */
	__u32 _rsv;
};

struct oplus_frame_sync_entity {
	__u32 x, y, z;      /* float 位型 */
	__u32 hp, hp_max;
	__u32 armor;
	__u32 team;
	__u32 _rsv[9];      /* 补齐到 64B，保证数组元素不跨页 */
};

struct oplus_frame_sync_camera {
	__u32 pos[3];       /* float 位型 */
	__u32 rot[3];
	__u32 fov;
	__u32 _rsv[9];      /* 补齐到 64B */
};

struct oplus_frame_sync_ext {
	__u32 tag;
	__u32 type;
	__u64 value;
	__u32 _rsv[4];      /* 补齐到 32B */
};

struct oplus_frame_sync_mbox {
	__u32 req_seq;
	__u32 ack_seq;
	__u64 addr;
	__u32 len;
	__u32 status;
	__u8  data[OPLUS_FRAME_SYNC_MBOX_DATA];  /* 24B 头 + 232B 数据 = 整 256B */
};

struct oplus_frame_sync_zone {
	/* ---- header 64B ---- */
	struct oplus_frame_sync_hdr hdr;

	/* ---- 命令池 ---- */
	struct oplus_frame_sync_cmd cmds[OPLUS_FRAME_SYNC_CMD_RING];

	/* ---- 实体区（A/B 双缓冲） ---- */
	struct oplus_frame_sync_entity ents[2][OPLUS_FRAME_SYNC_ENTITY_MAX];

	/* ---- 相机区（A/B 双缓冲） ---- */
	struct oplus_frame_sync_camera cam[2];

	/* ---- 扩展槽（骨骼/武器等，A/B 双缓冲） ---- */
	struct oplus_frame_sync_ext ext[2][OPLUS_FRAME_SYNC_EXT_SLOTS];

	__u32 _pad2[352];   /* 1408B：将 mbox 对齐到页边界 */

	/* ---- 一次性读邮箱 ---- */
	struct oplus_frame_sync_mbox mbox[OPLUS_FRAME_SYNC_MBOX_SLOTS];

	__u32 _tail[32];
};

#endif /* _OPLUS_FRAME_SYNC_UAPI_H */
