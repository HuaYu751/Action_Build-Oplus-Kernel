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
#include <stddef.h>   /* offsetof：OPLUS_FRAME_SYNC_MAGIC_OFF 两端共用 */
/* 用户态优先用系统 linux/types.h（NDK sysroot 自带，__u64=unsigned long long）；
 * 无该头的环境再兜底补齐，类型写法与内核保持一致避免重定义冲突 */
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

#define OPLUS_FRAME_SYNC_ABI_VERSION  0x00030003u
#define OPLUS_FRAME_SYNC_BUF_ID        1u      /* 缓冲标识：attach 后内核写入，用户态据此判绑定 */

/* 258048 = 252KB（63 页）：奇尺寸避开 bionic/Scudo 恰好 256KB 的匿名分配撞车
 *（2026-11-21 实机误绑根因，见 工单_共享池误绑修复_v1.md D1）。
 * 注意必须是页倍数：263168 非页对齐，mmap 后 VMA 被取整导致尺寸匹配失效。 */
#define OPLUS_FRAME_SYNC_MAP_SIZE     (258048u)

/* 池归属魔数（D2 双因子）：用户态 mmap 后即写入 _rsv_future 起始（8 字节对齐），
 * 内核扫描尺寸匹配后再校验魔数，防止再次误绑同尺寸匿名分配。
 * 常量 0x31535846474C504F 的小端内存字节 = ASCII "OPLGFXS1"。 */
#define OPLUS_FRAME_SYNC_POOL_MAGIC   0x31535846474C504FULL

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
#define OPLUS_FRAME_SYNC_STAGE_STANDBY 3   /* 待命：已绑定、已释放游戏 mm，等音量- 唤醒或前端重发 START */

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
#define OPLUS_FRAME_SYNC_CMD_PAUSE         0x0B   /* 暂停采样：保留目标 mm 与配置，可 SET_RATE>0/START 秒恢复 */
#define OPLUS_FRAME_SYNC_CMD_DETACH        0x0C   /* 前端主动断开共享池：unpin + 释放一切，进程退出前调用 */
#define OPLUS_FRAME_SYNC_CMD_SET_BONE_CFG  0x0D   /* 下发骨骼链配置（写 zone.bone_cfg） */
#define OPLUS_FRAME_SYNC_CMD_SET_ESP_CFG   0x0E   /* 下发 ESP 全链偏移配置（写 zone.esp_cfg） */

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
#define OPLUS_FRAME_SYNC_FLAG_BONE    (1u << 4)   /* 骨骼采样（每实体关节原始数据 + ctw，前端换算世界坐标） */

/*
 * 内存布局（总 80512B ≈ 78.6KB，远小于 256KB 映射）：
 *   header 64B → cmds 448B → ents[2][64] 8KB → cam[2] 128B → ext[2][32] 2KB
 *   → pad → mbox[8] 2KB(页对齐@12288) → bone_cfg 64B → esp_cfg 156B → pad
 *   → bone[2][64] 32KB(页对齐@16384) → aim 512B(占位) → trace 2KB(占位)
 *   → name[64] 4KB(单缓冲) → rsv_future 24KB(备用) → tail 128B
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
	__s32 frame_num;    /* 采样帧计数（内核每采样一次 +1，用户态据此测算实际采样率） */
	__s64 k_stamp;      /* 内核存活心跳（纳秒，内核每 tick 写，用户态据此判断内核在线） */
	__s32 _pad[3];      /* 补齐 64B */
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
	__u32 weapon;       /* 手持武器 ID（原始 u8 零扩展） */
	__u32 _rsv[8];      /* 补齐到 64B，保证数组元素不跨页 */
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

/* ---- 骨骼：每实体 14 关节原始数据 + ctw（前端用户态换算世界坐标，内核不碰浮点） ---- */
#define OPLUS_FRAME_SYNC_BONE_JOINTS  14

/* 骨骼链配置（CMD_SET_BONE_CFG 下发；稳定偏移，换版本只改这里） */
struct oplus_frame_sync_bone_cfg {
	__u32 mesh_off;        /* act → mesh 偏移 */
	__u32 bd_off;          /* mesh → 骨骼数组首址 偏移 */
	__u32 bn_off;          /* mesh → 骨骼数 偏移 */
	__u32 ctw_off;         /* mesh → ctw（ComponentToWorld）偏移 */
	__u32 joint_data_off;  /* 关节局部坐标在元素内偏移 */
	__u32 joint_stride;    /* 关节元素步长 */
	__u32 bn_min;          /* 骨骼数下限（合法性校验） */
	__u32 bn_max;          /* 骨骼数上限 */
	__u8  joint_idx[OPLUS_FRAME_SYNC_BONE_JOINTS]; /* 14 关节在骨骼数组里的索引 */
	__u8  _pad[2];
	__u32 _rsv[4];         /* 补齐 64B */
};

/* 单实体骨骼数据（A/B 双缓冲；用户态取 joints + ctw 做四元数旋转得世界坐标） */
struct oplus_frame_sync_bone {
	__u32 joints[OPLUS_FRAME_SYNC_BONE_JOINTS][3]; /* 168B：14 关节局部坐标（float 位型，已按 factor 变换） */
	__u32 ctw[12];         /* 48B：ctw 原始（quat4 + trans3 + scale3 + 2pad） */
	__u32 ok;              /* 4B：本次骨骼读取有效 */
	__u32 _rsv[9];         /* 36B：补齐 256B，保证元素不跨页 */
};

/* ---- ESP 全链偏移配置（CMD_SET_ESP_CFG 下发；内核据此从 libUE4 基址追整条链） ----
 * 只存偏移与索引，不含地址；地址只有 base（libUE4 基址）由 SET_PARAMS 下发。
 * 世界重建时内核每帧重追 top 链（L0/pc/dec...），无需前端缓存重解。
 */
struct oplus_frame_sync_esp_cfg {
	/* top 链（base → L0 → pc/W → cam/pawn/lv → dec/cnt） */
	__u32 l0_off;          /* base → L0 */
	__u32 pc_off;          /* L0 → PlayerController */
	__u32 cam_off;         /* pc → Camera */
	__u32 pawn_off;        /* pc → 自身 pawn */
	__u32 w1_off;          /* L0 → w1 */
	__u32 w2_off;          /* w1 → World */
	__u32 lv_off;          /* World → Level */
	__u32 dec_off;         /* Level → 实体数组首址 */
	__u32 cnt_off;         /* Level → 实体数量 */
	/* 实体数组遍历 */
	__u32 arr_stride;      /* 数组元素步长 */
	__u32 act_off;         /* 元素(layer) → actor */
	/* 坐标链 */
	__u32 root_off;        /* actor → root component */
	__u32 pos_off;         /* root → x（y=+4 z=+8） */
	/* 血量链（act→e1→e2→(e3?e3:e2)=eh） */
	__u32 e1_off;          /* actor → e1 */
	__u32 e2_off;          /* e1 → e2 */
	__u32 hp_off;          /* eh → hp */
	__u32 hpmax_off;       /* eh → hpmax */
	__u32 armor_off;       /* eh → armor */
	__u32 armormax_off;    /* eh → armormax */
	/* 队伍 */
	__u32 ps_off;          /* actor → PlayerState */
	__u32 team_off;        /* ps → team */
	/* 名字链（ps → FString 指针/字符数，两组：主用 + 兜底） */
	__u32 name_ptr_off;    /* ps → 名字字符串指针（主用） */
	__u32 name_cnt_off;    /* ps → 名字字符数（主用） */
	__u32 name_ptr2_off;   /* ps → 名字字符串指针（兜底） */
	__u32 name_cnt2_off;   /* ps → 名字字符数（兜底） */
	/* 武器（实体手持 ID，1 字节） */
	__u32 weapon_off;      /* actor → weapon */
	/* 相机字段（相对 cam） */
	__u32 cam_pos_off;     /* cam → pos（x,y,z 连续） */
	__u32 cam_pitch_off;
	__u32 cam_yaw_off;
	__u32 cam_fov_off;
	/* 自身（相对 pawn） */
	__u32 self_ps_off;     /* pawn → PlayerState */
	__u32 self_camp_off;   /* ps → camp */
	__u32 gunbase_off;     /* pawn → 枪基址 */
	__u32 ammo_max_off;    /* 枪 → 最大子弹 */
	__u32 ammo_used_off;   /* 枪 → 已消耗 */
	__u32 self_weapon_off; /* pawn → 自身手持 */
	__u32 _rsv[3];
};

/* ---- 名字区（单缓冲，慢变数据；内核写原始 UTF-16LE，前端转换） ---- */
#define OPLUS_FRAME_SYNC_NAME_DATA  56   /* 原始 UTF-16LE 字节（≤28 字符） */

struct oplus_frame_sync_name {
	__u8  data[OPLUS_FRAME_SYNC_NAME_DATA];
	__u32 len;           /* data 有效字节数；0 = 无名字 */
	__u32 _rsv[1];       /* 56+4+4 = 整 64B，元素不跨页 */
};

/* ext 槽保留段：ESP 全链模式下内核写自身数据（28..31 槽） */
#define OPLUS_FRAME_SYNC_EXT_SELF_BASE   28
#define OPLUS_FRAME_SYNC_TAG_SELF_CAMP   0x01   /* 自身阵营（区分敌友） */
#define OPLUS_FRAME_SYNC_TAG_AMMO_CUR    0x02   /* 当前子弹 */
#define OPLUS_FRAME_SYNC_TAG_AMMO_MAX    0x03   /* 最大子弹 */
#define OPLUS_FRAME_SYNC_TAG_SELF_WEAPON 0x04   /* 自身手持武器 ID */

/* ---- 占位区：本期只占位定义，后续工单逐个填实现 ---- */
struct oplus_frame_sync_aim {        /* 自瞄输入/输出（占位） */
	__u32 enable;
	__u32 target;          /* 目标实体索引 */
	__u32 joint;           /* 瞄准关节 */
	__u32 mode;
	__s32 out_yaw;         /* 输出 yaw（float 位型） */
	__s32 out_pitch;
	__u32 _rsv[122];       /* 补齐 512B */
};

struct oplus_frame_sync_trace {      /* 断点追踪配置 + 缓冲（占位） */
	__u32 cfg[64];         /* 256B */
	__u32 buf[448];        /* 1792B，共 2048B */
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

	/* ---- 骨骼链配置（前端下发） ---- */
	struct oplus_frame_sync_bone_cfg bone_cfg;

	/* ---- ESP 全链偏移配置（前端下发） ---- */
	struct oplus_frame_sync_esp_cfg esp_cfg;

	__u32 _pad3[457];   /* 1828B：将 bone 对齐到页边界（16384） */

	/* ---- 骨骼区（A/B 双缓冲，每实体 14 关节原始数据 + ctw） ---- */
	struct oplus_frame_sync_bone bone[2][OPLUS_FRAME_SYNC_ENTITY_MAX];

	/* ---- 自瞄区（占位） ---- */
	struct oplus_frame_sync_aim aim;

	/* ---- 断点追踪区（占位） ---- */
	struct oplus_frame_sync_trace trace;

	/* ---- 名字区（单缓冲，与实体输出索引一一对应） ---- */
	struct oplus_frame_sync_name name[OPLUS_FRAME_SYNC_ENTITY_MAX];

	__u32 _rsv_future[6144];   /* 24KB 备用：后续自瞄/断点/其他绘制扩展拆分 */

	__u32 _tail[32];
};

/* 魔数在池内的偏移（struct 定义之后才能 offsetof，两端同一头文件自动一致） */
#define OPLUS_FRAME_SYNC_MAGIC_OFF \
	((unsigned int)offsetof(struct oplus_frame_sync_zone, _rsv_future))

#endif /* _OPLUS_FRAME_SYNC_UAPI_H */
