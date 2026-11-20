/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <linux/objtool.h>
#include <asm/ibt.h>
#include <asm/nospec-branch.h>
#include <asm/unwind_hints.h>

/*
 * LKL invokes f() before returning from jmp_buf_set. A later longjmp, on
 * the SAME host thread, discards f's frames and returns to set's caller.
 * Only the SysV callee-saved registers and the saved return frame survive.
 * The buffer is LKL's opaque unsigned long buf[128], not a libc jmp_buf.
 */
.text
SYM_FUNC_START(evl_lkl_jmp_set)
	ENDBR
	UNWIND_HINT_FUNC
	movq %rbx, 0(%rdi)
	movq %rbp, 8(%rdi)
	movq %r12, 16(%rdi)
	movq %r13, 24(%rdi)
	movq %r14, 32(%rdi)
	movq %r15, 40(%rdi)
	movq %rsp, 48(%rdi)
	subq $8, %rsp
	CALL_NOSPEC rsi
	addq $8, %rsp
	RET
SYM_FUNC_END(evl_lkl_jmp_set)

SYM_FUNC_START(evl_lkl_jmp_longjmp)
	ENDBR
	UNWIND_HINT_FUNC
	movq 0(%rdi), %rbx
	movq 8(%rdi), %rbp
	movq 16(%rdi), %r12
	movq 24(%rdi), %r13
	movq 32(%rdi), %r14
	movq 40(%rdi), %r15
	movq 48(%rdi), %rsp
	UNWIND_HINT_FUNC
	RET
SYM_FUNC_END(evl_lkl_jmp_longjmp)
STACK_FRAME_NON_STANDARD evl_lkl_jmp_longjmp
