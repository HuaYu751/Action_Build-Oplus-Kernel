#ifndef MEMKERNEL_COMM_H
#define MEMKERNEL_COMM_H

/*
 * comm.h — 内核/用户态共用协议头（方案三：/proc/version ioctl 通道）
 *
 * 通道：open("/proc/version") + ioctl
 *   - /proc/version 本无任何 ioctl 处理，正常节点对任意 cmd 返回 -ENOTTY
 *   - 我们的驱动只对 OPLUS_DFR_CMD_BASE~END 区间内的 cmd 有反应
 *   - 未认证进程、非绑定进程收到任何 cmd（含我们的）一律 -ENOTTY
 *     → 主动探测与正常内核节点零差异（任务书 C10/C11）
 *
 * 幻数说明：
 *   cmd 区间与认证密钥可按需整体更换（换游戏/换版本时换一套即可），
 *   内核侧与用户态必须同步修改。
 *
 * v3（2026-11-17）：新增共享内存通道（方案书 v2.2.1 §3）
 *   - 一次性 attach：helper 调 OP_ATTACH 下发 app 的共享页地址与 pid
 *   - 之后稳态零 ioctl：命令池/数据池全在共享页内
 *   - 旧 OP_READ_MEM / OP_WRITE_MEM 保留（kd 等工具兼容）
 */

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#endif

/* ---- ioctl cmd 幻数区间（不可与常见内核节点 cmd 撞车） ---- */
#define OPLUS_DFR_CMD_BASE   0xC0DE6000u
#define OPLUS_DFR_CMD_END    0xC0DE60FFu

enum Operations
{
	OP_AUTHENTICATE = OPLUS_DFR_CMD_BASE,	/* 认证握手：arg = OPLUS_DFR_AUTH_KEY */
	OP_READ_MEM,				/* 读目标进程内存 */
	OP_WRITE_MEM,				/* 写目标进程内存 */
	OP_MODULE_BASE,				/* 查模块基址（保留，暂不实现 → -ENOTTY） */
	OP_ATTACH,				/* 共享页附着：arg = struct AttachReq* */
	OP_SHM_STATUS,				/* 共享页状态：arg = struct ShmStatus* */
	OP_SHM_DETACH,				/* 主动拆除共享页 */
	OP_DEAUTH = OPLUS_DFR_CMD_END,		/* 解除认证绑定 */
};

/* ---- 认证密钥（64 位，直接作为 OP_AUTHENTICATE 的 arg） ---- */
#define OPLUS_DFR_AUTH_KEY   0x4B4D455532303236ULL	/* "KMEU2026" */

/* ---- 读写请求结构体（ioctl arg 指向它） ---- */
struct CopyMemory
{
	pid_t pid;
	uintptr_t addr;
	void *buffer;
	size_t size;
};

struct ModuleBase
{
	pid_t pid;
	char *name;
	uintptr_t base;
};

/* ================================================================== *
 * 共享内存通道 ABI（方案书 v2.2.1 §3.3）
 * 布局偏移：header 64B → cmds 448B → ents 8KB → cam 128B
 *           → ext 2KB → pad 1408B → mbox 2KB（起始 12288，页对齐）
 *           → tail 128B（总 14464B，4 页内）
 * 每个元素要么步长整除 4096，要么所在区起始页对齐，保证不跨页。
 * ================================================================== */
#define SHM_ABI_VERSION     0x00020001u	/* 2.1 */
#define SHM_MAP_SIZE        (256u * 1024u)
#define SHM_ENTITY_MAX      64
#define SHM_FIELD_MAX       16
#define SHM_CMD_RING        16
#define SHM_EXT_SLOTS       32
#define SHM_MBOX_SLOTS      8
#define SHM_MBOX_DATA       232		/* 单槽 256B：24B 头 + 232B 数据 */

/* 采样状态 */
#define SHM_ST_IDLE         0
#define SHM_ST_RUNNING      1
#define SHM_ST_PAUSED       2

/* 命令码（命令池 cmd 字段；稳态控制全走这里，零 ioctl） */
#define SHM_CMD_START        0x01	/* p0 = 目标 pid */
#define SHM_CMD_STOP         0x02
#define SHM_CMD_SET_RATE     0x03	/* p0 = Hz (1..240) */
#define SHM_CMD_SET_PARAMS   0x04	/* p0 = 基址低32 p1 = 基址高32 p2 = 数量 p3 = 数组偏移（步长固定 8） */
#define SHM_CMD_SET_FIELD    0x05	/* p0 = 字段槽 p1 = 偏移 p2 = 类型 */
#define SHM_CMD_DEAUTH       0x06
#define SHM_CMD_SET_OBF      0x07	/* p0 = 新混淆 key（0 = 随机） */
#define SHM_CMD_SET_FEATURE  0x08	/* p0 = feature_mask */
#define SHM_CMD_SET_FIELD_EXT 0x09	/* p0 = 扩展槽 p1 = 偏移 p2 = 类型 p3 = tag */
#define SHM_CMD_MBOX_READ    0x0A	/* p0 = 邮箱槽（0..7） */

/* 字段槽（SET_FIELD 的 p0） */
enum ShmFieldSlot
{
	SHM_F_X = 0,
	SHM_F_Y,
	SHM_F_Z,
	SHM_F_HP,
	SHM_F_HP_MAX,
	SHM_F_ARMOR,
	SHM_F_TEAM,
	SHM_F_CAM_PX,
	SHM_F_CAM_PY,
	SHM_F_CAM_PZ,
	SHM_F_CAM_RX,
	SHM_F_CAM_RY,
	SHM_F_CAM_RZ,
	SHM_F_CAM_FOV,
	SHM_F_BASE_MAX = 16,		/* 0..13 用于实体+相机 */
	SHM_F_EXT_BASE = 16,		/* 16..47 映射到 ext[0..31] */
	SHM_F_TOTAL = 48,
};

/* 字段类型（SET_FIELD 的 p2） */
#define SHM_T_F32   1
#define SHM_T_U32   2
#define SHM_T_S32   3
#define SHM_T_U8    4

/* feature_mask 位定义（app 声明启用哪些采样，内核据此省 CPU） */
#define SHM_FEAT_ENTITY   (1u << 0)
#define SHM_FEAT_CAMERA   (1u << 1)
#define SHM_FEAT_EXT      (1u << 2)
#define SHM_FEAT_MBOX     (1u << 3)

struct ShmCmd
{
	uint32_t cmd;
	int32_t  p0, p1, p2, p3;
	uint32_t result;		/* 内核回填：0 = OK，负值 = -errno */
	uint32_t _rsv;
};

struct ShmEntity
{
	uint32_t x, y, z;		/* float 位型，已 XOR 混淆 */
	uint32_t hp, hp_max;
	uint32_t armor;
	uint32_t team;			/* 不混淆 */
	uint32_t _rsv[9];		/* 补齐 64B */
};

struct ShmCamera
{
	uint32_t pos[3];		/* float 位型，已 XOR 混淆 */
	uint32_t rot[3];
	uint32_t fov;
	uint32_t _rsv[9];		/* 补齐 64B */
};

struct ShmExtSlot
{
	uint32_t tag;
	uint32_t type;
	uint64_t value;
	uint32_t _rsv[4];		/* 补齐 32B */
};

struct ShmMailbox
{
	uint32_t req_seq;
	uint32_t ack_seq;
	uint64_t addr;
	uint32_t len;
	uint32_t status;
	uint8_t  data[SHM_MBOX_DATA];	/* 24B 头 + 232B 数据 = 整 256B */
};

struct ShmZone
{
	/* ---- [0] header 64B ---- */
	uint32_t magic;			/* attach 时随机化 */
	uint32_t version;
	uint32_t seq_cmd;		/* app 写：命令序号 */
	uint32_t state;
	uint32_t entity_count;
	uint32_t active_buf;		/* 内核写：0/1 */
	uint32_t heartbeat;		/* app 每秒 +1 */
	uint32_t obf_key;
	uint32_t feature_mask;
	uint32_t _pad[7];

	/* ---- [1] 命令池 ---- */
	struct ShmCmd cmds[SHM_CMD_RING];

	/* ---- [2] 实体区（A/B 双缓冲） ---- */
	struct ShmEntity ents[2][SHM_ENTITY_MAX];

	/* ---- [3] 相机区（A/B 双缓冲） ---- */
	struct ShmCamera cam[2];

	/* ---- [4] 扩展槽（骨骼/武器等，A/B 双缓冲） ---- */
	struct ShmExtSlot ext[2][SHM_EXT_SLOTS];

	uint32_t _pad2[352];		/* 1408B：将 mbox 对齐到页边界（12288） */

	/* ---- [5] 一次性读邮箱 ---- */
	struct ShmMailbox mbox[SHM_MBOX_SLOTS];

	uint32_t _tail[32];
};

/* ---- attach 请求/回执 ---- */
struct AttachReq
{
	uint32_t customer_pid;		/* 持有共享页的 app pid */
	uint32_t size;			/* 通常 SHM_MAP_SIZE */
	uint64_t addr;			/* mmap 返回地址（页对齐） */
	uint64_t key;			/* = OPLUS_DFR_AUTH_KEY，兼作一道门槛 */
};

struct AttachAck
{
	uint32_t magic;			/* 供 app 自检是否绑定成功 */
	uint32_t version;		/* = SHM_ABI_VERSION */
	uint32_t obf_key;		/* 混淆主密钥 */
	uint32_t status;		/* 0 = OK */
};

/* OP_ATTACH 的 ioctl 包装：arg 指向它，req 入参 / ack 出参 */
struct AttachIo
{
	struct AttachReq req;
	struct AttachAck ack;
};

struct ShmStatus
{
	uint32_t state;
	uint32_t rate;
	uint32_t entity_count;
	uint32_t target_pid;
	uint32_t frames;
	uint32_t errors;
	uint32_t heartbeat;
	uint32_t _rsv;
};

#endif
