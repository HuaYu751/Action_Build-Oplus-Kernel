#ifndef MEMKERNEL_SHM_H
#define MEMKERNEL_SHM_H

#include <linux/types.h>
#include "comm.h"

/*
 * 共享内存通道（方案书 v2.2.1 §3/§4）
 *
 *   attach  一次性：pin 调用方指定 app 的共享页，初始化头部，返回 ack
 *   运行态  零 ioctl：命令池/邮箱/数据池全在共享页内
 *   detach  命令池 DEAUTH 或心跳超时，或 ioctl OP_SHM_DETACH
 */
int oplus_dfr_shm_attach(const struct AttachReq *req, struct AttachAck *ack);
int oplus_dfr_shm_status(struct ShmStatus *st);
void oplus_dfr_shm_detach(void);

#endif
