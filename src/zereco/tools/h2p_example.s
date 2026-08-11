	.file	"h2p_example.c"
	.text
	.p2align 4
	.type	pbfs_inner.constprop.0, @function
pbfs_inner.constprop.0:
.LFB53:
	.cfi_startproc
	xorl	%r10d, %r10d
	cmpq	%rdi, %rsi
	je	.L1
	movsd	.LC0(%rip), %xmm3
	movsd	.LC1(%rip), %xmm2
	jmp	.L5
	.p2align 4,,10
	.p2align 3
.L3:
	cmpl	$1, %eax
	jne	.L4
	leaq	(%rcx,%r8,8), %rax
	addq	$2, %r10
	movsd	(%rax), %xmm1
	movapd	%xmm1, %xmm0
	mulsd	%xmm3, %xmm0
	addsd	%xmm2, %xmm0
	addsd	%xmm1, %xmm0
	movsd	%xmm0, (%rax)
.L4:
	addq	$4, %rdi
	cmpq	%rsi, %rdi
	je	.L1
.L5:
	movslq	(%rdi), %r8
	leaq	(%rdx,%r8,4), %r9
	movl	(%r9), %eax
	cmpl	$-1, %eax
	jne	.L3
	addq	$4, %rdi
	movl	$1, (%r9)
	addq	$1, %r10
	cmpq	%rsi, %rdi
	jne	.L5
.L1:
	movq	%r10, %rax
	ret
	.cfi_endproc
.LFE53:
	.size	pbfs_inner.constprop.0, .-pbfs_inner.constprop.0
	.section	.rodata.str1.1,"aMS",@progbits,1
.LC2:
	.string	"alloc failed\n"
.LC3:
	.string	"hits=%ld\n"
	.section	.text.startup,"ax",@progbits
	.p2align 4
	.globl	main
	.type	main, @function
main:
.LFB52:
	.cfi_startproc
	endbr64
	pushq	%r13
	.cfi_def_cfa_offset 16
	.cfi_offset 13, -16
	movl	$16777216, %edi
	pushq	%r12
	.cfi_def_cfa_offset 24
	.cfi_offset 12, -24
	pushq	%rbp
	.cfi_def_cfa_offset 32
	.cfi_offset 6, -32
	pushq	%rbx
	.cfi_def_cfa_offset 40
	.cfi_offset 3, -40
	subq	$8, %rsp
	.cfi_def_cfa_offset 48
	call	malloc@PLT
	movl	$4194304, %edi
	movq	%rax, depths(%rip)
	movq	%rax, %rbx
	call	malloc@PLT
	movq	%rax, neigh(%rip)
	testq	%rbx, %rbx
	je	.L22
	movq	%rax, %rbp
	testq	%rax, %rax
	je	.L22
	movq	rng_state(%rip), %rax
	movq	%rbx, %rcx
	leaq	16777216(%rbx), %rsi
	.p2align 4,,10
	.p2align 3
.L13:
	movq	%rax, %rdx
	addq	$4, %rcx
	salq	$13, %rdx
	xorq	%rdx, %rax
	movq	%rax, %rdx
	shrq	$7, %rdx
	xorq	%rax, %rdx
	movq	%rdx, %rax
	salq	$17, %rax
	xorq	%rdx, %rax
	andl	$3, %edx
	subl	$1, %edx
	movl	%edx, -4(%rcx)
	cmpq	%rcx, %rsi
	jne	.L13
	movq	%rbp, %rcx
	leaq	4194304(%rbp), %r12
	.p2align 4,,10
	.p2align 3
.L14:
	movq	%rax, %rdx
	addq	$4, %rcx
	salq	$13, %rdx
	xorq	%rax, %rdx
	movq	%rdx, %rax
	shrq	$7, %rax
	xorq	%rax, %rdx
	movq	%rdx, %rax
	salq	$17, %rax
	xorq	%rdx, %rax
	movl	%eax, %edx
	andl	$4194303, %edx
	movl	%edx, -4(%rcx)
	cmpq	%rcx, %r12
	jne	.L14
	movl	$1, %esi
	movl	$33554432, %edi
	movq	%rax, rng_state(%rip)
	call	calloc@PLT
	movq	%rax, scores(%rip)
	movq	%rax, %rcx
	testq	%rax, %rax
	je	.L22
	movl	$64, %r11d
	xorl	%r13d, %r13d
	.p2align 4,,10
	.p2align 3
.L16:
	movq	%rbx, %rdx
	movq	%r12, %rsi
	movq	%rbp, %rdi
	call	pbfs_inner.constprop.0
	addq	%rax, %r13
	subl	$1, %r11d
	jne	.L16
	movq	%r13, %rdx
	leaq	.LC3(%rip), %rsi
	movl	$1, %edi
	xorl	%eax, %eax
	call	__printf_chk@PLT
	movq	depths(%rip), %rdi
	call	free@PLT
	movq	neigh(%rip), %rdi
	call	free@PLT
	movq	scores(%rip), %rdi
	call	free@PLT
	xorl	%eax, %eax
.L9:
	addq	$8, %rsp
	.cfi_remember_state
	.cfi_def_cfa_offset 40
	popq	%rbx
	.cfi_def_cfa_offset 32
	popq	%rbp
	.cfi_def_cfa_offset 24
	popq	%r12
	.cfi_def_cfa_offset 16
	popq	%r13
	.cfi_def_cfa_offset 8
	ret
.L22:
	.cfi_restore_state
	movq	stderr(%rip), %rcx
	movl	$13, %edx
	movl	$1, %esi
	leaq	.LC2(%rip), %rdi
	call	fwrite@PLT
	movl	$1, %eax
	jmp	.L9
	.cfi_endproc
.LFE52:
	.size	main, .-main
	.data
	.align 8
	.type	rng_state, @object
	.size	rng_state, 8
rng_state:
	.quad	88172645463325252
	.local	scores
	.comm	scores,8,8
	.local	neigh
	.comm	neigh,8,8
	.local	depths
	.comm	depths,8,8
	.section	.rodata.cst8,"aM",@progbits,8
	.align 8
.LC0:
	.long	0
	.long	1071644672
	.align 8
.LC1:
	.long	0
	.long	1072693248
	.ident	"GCC: (Ubuntu 9.4.0-1ubuntu1~20.04.2) 9.4.0"
	.section	.note.GNU-stack,"",@progbits
	.section	.note.gnu.property,"a"
	.align 8
	.long	 1f - 0f
	.long	 4f - 1f
	.long	 5
0:
	.string	 "GNU"
1:
	.align 8
	.long	 0xc0000002
	.long	 3f - 2f
2:
	.long	 0x3
3:
	.align 8
4:
