#ifndef JUMP_H
#define JUMP_H

#include "common.h"

VOID jump_to_xnu(VOID *entry, UINT64 boot_args_phys, VOID *stack_top) __attribute__((noreturn));

#if defined(__riscv)
// s-mode with satp = 0 and interrupts off, a0 = physical boot_args, a1 = boot hart id
VOID riscv64_jump_to_xnu(UINT64 entry_phys, UINT64 boot_args_phys, UINT64 hartid)
    __attribute__((noreturn));
#endif

#if defined(PD_ARCH_X86)
/*
 * Switch to `new_sp` and call fn(arg) there. Used to get off the
 * firmware-provided stack before copying the kernel image over low memory,
 * which on some boards contains that very stack.
 */
VOID pd_call_on_stack(VOID *new_sp, VOID (*fn)(VOID *), VOID *arg)
    __attribute__((noreturn));
#endif

#endif