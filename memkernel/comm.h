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

#endif
