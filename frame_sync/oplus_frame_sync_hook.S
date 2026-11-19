/* SPDX-License-Identifier: GPL-2.0 */
/*
 * oplus_frame_sync_hook.S —— input_event 的 inline hook 入口（工单 #4）
 *
 * 由 input_event 入口 patch 的绝对跳转跳到此处。
 * 流程：保存现场(x0-x18,x30) → 调 C 处理函数 → 恢复现场
 *       → 执行原始 4 条指令槽 → 绝对跳回 input_event + 16。
 *
 * 符号：
 *   oplus_frame_sync_ihook       hook 入口（patch 目标）
 *   oplus_frame_sync_orig_slot   16 字节原始指令槽（运行时填充）
 *   oplus_frame_sync_ret_addr    8 字节跳回地址（运行时填 input_event+16）
 */

.section .text

.global oplus_frame_sync_ihook
oplus_frame_sync_ihook:
	/* 保存调用者保存寄存器 + lr（共 160 字节） */
	stp	x0, x1, [sp, #-160]!
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x30, [sp, #144]

	/* 调 C：oplus_frame_sync_input_event(type, code, value) */
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	bl	oplus_frame_sync_input_event

	/* 恢复现场 */
	ldp	x18, x30, [sp, #144]
	ldp	x16, x17, [sp, #128]
	ldp	x14, x15, [sp, #112]
	ldp	x12, x13, [sp, #96]
	ldp	x10, x11, [sp, #80]
	ldp	x8, x9, [sp, #64]
	ldp	x6, x7, [sp, #48]
	ldp	x4, x5, [sp, #32]
	ldp	x2, x3, [sp, #16]
	ldp	x0, x1, [sp], #160

	/* 原始 4 条指令槽（运行时填充） */
	.global oplus_frame_sync_orig_slot
oplus_frame_sync_orig_slot:
	nop
	nop
	nop
	nop

	/* 绝对跳回 input_event + 16 */
	adrp	x16, oplus_frame_sync_ret_addr
	ldr	x16, [x16, #:lo12:oplus_frame_sync_ret_addr]
	br	x16

	.global oplus_frame_sync_ret_addr
	.align 3
oplus_frame_sync_ret_addr:
	.quad	0
