/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Darwin Embedded contributors.
 *
 * Private-stack bridge for the native 64-bit XNU boot contract.
 *
 * SysV arguments: RDI=staging buffer, RSI=byte count, RDX=kernel entry,
 * RCX=boot_args, R8=top of the loader's reserved stack. The C installer
 * consumes the first four arguments without rearranging their registers.
 *
 * Firmware may have placed its current stack inside the kernel destination.
 * Move to our checked reservation before C copies the collection there.
 * Paging, segment selectors and the CPU execution mode stay unchanged; the
 * kernel installs its own machine state after the copy has finished.
 */
.text
.code64
.globl pc_enter_on_private_stack
.type pc_enter_on_private_stack,@function
pc_enter_on_private_stack:
    cli
    cld
    movq %r8, %rsp
    andq $-16, %rsp
    xorq %rbp, %rbp
    /* CALL supplies the SysV return-address slot and 16-byte alignment.
     * pc_install_collection cannot return; trap if that contract changes. */
    call pc_install_collection
    ud2
.size pc_enter_on_private_stack, .-pc_enter_on_private_stack
.section .note.GNU-stack,"",@progbits
