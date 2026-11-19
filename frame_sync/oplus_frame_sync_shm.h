/* SPDX-License-Identifier: GPL-2.0 */
/*
 * oplus_frame_sync_shm.h —— 帧同步共享池通道对外接口
 *
 * 由 oplus_frame_sync_shm.c 实现，供工单 #4（音量键触发 + 进程/池自定位）
 * 在定位成功后调用 attach 绑定用户态共享池。
 */
#ifndef _OPLUS_FRAME_SYNC_SHM_H
#define _OPLUS_FRAME_SYNC_SHM_H

#include <linux/types.h>

/*
 * 绑定用户态共享池（内核自定位完成后调用）。
 *   pid   持有共享池的进程（oplus_frame_sync）
 *   addr  池的虚拟地址（页对齐）
 *   size  池大小（通常 OPLUS_FRAME_SYNC_MAP_SIZE）
 * 返回：0 = OK；负值 = -errno。
 */
int oplus_frame_sync_shm_attach(pid_t pid, u64 addr, u32 size);

/*
 * 拆除共享池（释放 pin 页与目标 mm）。module_exit / 重新绑定时调用。
 */
void oplus_frame_sync_shm_detach(void);

#endif /* _OPLUS_FRAME_SYNC_SHM_H */
