/* arm32 Linux zImage entry (Documentation/arch/arm/booting.rst): r2 = FDT, MMU
 * off, SVC or HYP. Drops HYP to SVC, relocates the position-independent image
 * and calls boot32_main(fdt). */
	.syntax unified
	.arm
	.arch armv7-a
	.arch_extension virt
	/* On the 'go' path (r7 = argc 1..15), put one character on the board's UART (left set
	 * up by U-Boot) so a hang shows how far entry got; on the a20 on every path. Clobbers r0, r1 */
#if defined(XNU_LOADER_PLATFORM_A20)
#define MARK_UART	0x01c28000	// sun7i uart0, which only bootz reaches
#elif defined(XNU_LOADER_PLATFORM_RK3506)
#define MARK_UART	0xff0a0000	// lyra uart0
#else
#define MARK_UART	0xff4c0000	// pico uart2
#endif
	.macro	mark c
#if defined(XNU_LOADER_PLATFORM_A20)
	ldr	r0, =MARK_UART
	mov	r1, #\c
	str	r1, [r0]
#else
	sub	r0, r7, #1
	cmp	r0, #15
	ldrlo	r0, =MARK_UART
	movlo	r1, #\c
	strlo	r1, [r0]
#endif
	.endm
	.section .text.head, "ax"
	.globl _start
_start:
	.rept 8
	mov	r0, r0
	.endr
	b	1f
	.word	0x016f2818		/* zImage magic */
	.word	0			/* start: position independent */
	.word	__file_size		/* end of the loaded image */
	.word	0x04030201		/* little-endian */
	/* +0x34: entry for U-Boot 'go', which sets the Thumb bit on this vendor U-Boot:
	 * 'bx pc' in Thumb state lands in ARM state at +0x38 */
	.thumb
	bx	pc
	nop
	.arm
1:
	mov	r9, r2
	mov	r7, r0			/* U-Boot 'go' passes argc, argv instead of an FDT */
	mov	r8, r1
#if defined(XNU_LOADER_PLATFORM_A20)
	/* banana pi green led (ph24) on: entered, with no uart needed; off again at the kernel jump */
	ldr	r0, =0x01c20908		/* PH_CFG3 */
	ldr	r1, [r0]
	bic	r1, r1, #7
	orr	r1, r1, #1		/* output */
	str	r1, [r0]
	ldr	r0, =0x01c2090c		/* PH_DAT */
	ldr	r1, [r0]
	orr	r1, r1, #(1 << 24)
	str	r1, [r0]
#endif
	mark	'1'
	mrs	r0, cpsr
	and	r1, r0, #0x1f
	cmp	r1, #0x1a		/* HYP */
	bne	2f
	bic	r0, r0, #0x1f
	orr	r0, r0, #0xd3		/* SVC, IRQ and FIQ masked */
	msr	spsr_hyp, r0
	adr	r1, 2f
	msr	elr_hyp, r1
	eret
2:
	cpsid	aif
	mark	'a'
	/* 'go' leaves the D-cache on: clean and invalidate it by set/way before turning it off */
	mrc	p15, 0, r0, c1, c0, 0
	tst	r0, #0x4
	beq	8f
	mrc	p15, 1, r0, c0, c0, 1	/* CLIDR */
	ands	r3, r0, #0x7000000
	mov	r3, r3, lsr #23		/* LoC * 2 */
	beq	8f
	mov	r10, #0			/* cache level * 2 */
9:	add	r2, r10, r10, lsr #1
	mov	r1, r0, lsr r2
	and	r1, r1, #7		/* cache type at this level */
	cmp	r1, #2
	blt	12f			/* no data cache here */
	mcr	p15, 2, r10, c0, c0, 0	/* CSSELR */
	isb
	mrc	p15, 1, r1, c0, c0, 0	/* CCSIDR */
	and	r2, r1, #7
	add	r2, r2, #4		/* log2(line bytes) */
	ldr	r4, =0x3ff
	ands	r4, r4, r1, lsr #3	/* ways - 1 */
	clz	r5, r4			/* way shift */
	ldr	r6, =0x7fff
	ands	r6, r6, r1, lsr #13	/* sets - 1 */
10:	mov	r11, r4
11:	orr	r12, r10, r11, lsl r5
	orr	r12, r12, r6, lsl r2
	mcr	p15, 0, r12, c7, c14, 2	/* DCCISW */
	subs	r11, r11, #1
	bge	11b
	subs	r6, r6, #1
	bge	10b
12:	add	r10, r10, #2
	cmp	r3, r10
	bgt	9b
	dsb
	isb
8:
	mark	'b'
	/* Set/way does not promise the data reaches RAM: clean and invalidate this image
	 * (through the stack) by address to the point of coherency before the MMU goes off */
	adr	r2, _start
	ldr	r3, =__bss_end
	add	r3, r3, r2
	bic	r2, r2, #31
13:	mcr	p15, 0, r2, c7, c14, 1	/* DCCIMVAC */
	add	r2, r2, #32
	cmp	r2, r3
	blo	13b
	dsb
	isb
	mrc	p15, 0, r0, c1, c0, 0	/* SCTLR: MMU, D-cache off */
	bic	r0, r0, #0x1
	bic	r0, r0, #0x4
	mcr	p15, 0, r0, c1, c0, 0
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0	/* TLBIALL */
	dsb
	isb
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0	/* invalidate the I-cache */
	mark	'c'

	adr	r10, _start		/* load address = link offset delta */
	ldr	r0, =__stack_top
	add	sp, r0, r10
	ldr	r0, =__bss_start
	ldr	r1, =__bss_end
	add	r0, r0, r10
	add	r1, r1, r10
	mov	r2, #0
3:	cmp	r0, r1
	strlo	r2, [r0], #4
	blo	3b
	mark	'd'
	/* Linked at 0: every R_ARM_RELATIVE slot gets the load address added */
	ldr	r0, =__rel_start
	ldr	r1, =__rel_end
	add	r0, r0, r10
	add	r1, r1, r10
4:	cmp	r0, r1
	bhs	5f
	ldr	r2, [r0], #4		/* offset */
	ldr	r3, [r0], #4		/* info */
	and	r3, r3, #0xff
	cmp	r3, #23			/* R_ARM_RELATIVE */
	bne	4b
	ldr	r3, [r2, r10]
	add	r3, r3, r10
	str	r3, [r2, r10]
	b	4b
5:
	mark	'2'
	mov	r0, r9
	mov	r1, r7
	mov	r2, r8
	bl	boot32_main
6:	wfi
	b	6b
	.ltorg

/* void boot32_jump(uint32_t entry, uint32_t boot_args): MMU and caches are off */
	.text
	.globl boot32_jump
boot32_jump:
	mov	r4, r0
	mov	r0, r1
	mov	r1, #0
	dsb
	isb
	bx	r4
