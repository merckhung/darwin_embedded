BITS 16
ORG 0

%ifndef PAYLOAD_SECTORS
%define PAYLOAD_SECTORS 1
%endif
%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 4
%endif

%ifndef STAGE2_LBA
%define STAGE2_LBA 1
%endif

%define LOAD_SEGMENT       0x0800
%define PAYLOAD_SEGMENT    0x2000
%define PAYLOAD_PHYS       (PAYLOAD_SEGMENT << 4)
%define PAYLOAD_LBA        (STAGE2_LBA + STAGE2_SECTORS)
%define E820_MAX_ENTRIES   64
%define PML4_PHYS          0x1000
%define PDPT_PHYS          0x2000
%define PD_PHYS            0x3000
%define PD_COUNT           4
; Bounce buffer for BIOS disk reads from long mode: inside the reserved low
; area, 32KiB-aligned so a 64-sector read never crosses a 64KiB DMA boundary.
%define BOUNCE_SEGMENT     0x1800
%define BOUNCE_SECTORS     64
%define L(x)               ((LOAD_SEGMENT << 4) + (x))

    jmp short entry
header:
    db 'XNULEG2', 0
    dd stage2_end - $$

entry:
    cli
    mov ax, LOAD_SEGMENT
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xfff0
    sti
    mov [boot_drive], dl

    mov si, banner
    call print
    call enable_a20
    call collect_e820
    call setup_vbe
    call load_payload

    cli
    call build_page_tables
    lgdt [gdt_descriptor]
    mov eax, cr4
    or eax, 1 << 5                 ; PAE
    mov cr4, eax
    mov eax, PML4_PHYS
    mov cr3, eax
    mov ecx, 0xc0000080            ; IA32_EFER
    rdmsr
    or eax, 1 << 8                 ; LME
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | 1          ; paging + protected mode
    mov cr0, eax
    jmp 0x08:(LOAD_SEGMENT << 4) + long_mode_entry

print:
    lodsb
    test al, al
    jz .done
    out 0xe9, al
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    jmp print
.done:
    ret

enable_a20:
    in al, 0x92
    or al, 2
    and al, 0xfe
    out 0x92, al
    ret

collect_e820:
    xor ebx, ebx
    mov word [e820_count], 0
    mov di, e820_entries
.next:
    cmp word [e820_count], E820_MAX_ENTRIES
    jae .done
    mov eax, 0xe820
    mov edx, 0x534d4150
    mov ecx, 24
    mov dword [es:di + 20], 1
    int 0x15
    jc .done
    cmp eax, 0x534d4150
    jne .done
    add di, 24
    inc word [e820_count]
    test ebx, ebx
    jnz .next
.done:
    ret

setup_vbe:
    mov byte [framebuffer.valid], 0
    mov dword [vbe_controller_info], 'VBE2'
    mov ax, 0x4f00
    mov di, vbe_controller_info
    int 0x10
    cmp ax, 0x004f
    jne .done
    mov ax, [vbe_controller_info + 14]
    mov [mode_list_offset], ax
    mov ax, [vbe_controller_info + 16]
    mov [mode_list_segment], ax
    mov word [best_vbe_mode], 0xffff
    mov dword [best_vbe_pixels], 0
.next_mode:
    mov ax, [mode_list_segment]
    mov fs, ax
    mov bx, [mode_list_offset]
    mov cx, [fs:bx]
    add word [mode_list_offset], 2
    cmp cx, 0xffff
    je .set_best
    mov [current_vbe_mode], cx
    mov ax, LOAD_SEGMENT
    mov es, ax
    mov ax, 0x4f01
    mov di, vbe_mode_info
    int 0x10
    cmp ax, 0x004f
    jne .next_mode
    test word [vbe_mode_info], 1 << 7
    jz .next_mode
    cmp byte [vbe_mode_info + 25], 32
    jne .next_mode
    movzx eax, word [vbe_mode_info + 18]
    movzx edx, word [vbe_mode_info + 20]
    imul eax, edx
    cmp eax, [best_vbe_pixels]
    jbe .next_mode
    mov [best_vbe_pixels], eax
    mov cx, [current_vbe_mode]
    mov [best_vbe_mode], cx
    jmp .next_mode
.set_best:
    cmp word [best_vbe_mode], 0xffff
    je .done
    mov ax, LOAD_SEGMENT
    mov es, ax
    mov cx, [best_vbe_mode]
    mov ax, 0x4f01
    mov di, vbe_mode_info
    int 0x10
    cmp ax, 0x004f
    jne .done
    mov ax, 0x4f02
    mov bx, [best_vbe_mode]
    or bx, 0x4000
    int 0x10
    cmp ax, 0x004f
    jne .done
    mov eax, [vbe_mode_info + 40]
    mov [framebuffer.base], eax
    movzx eax, word [vbe_mode_info + 18]
    mov [framebuffer.width], eax
    movzx eax, word [vbe_mode_info + 20]
    mov [framebuffer.height], eax
    movzx eax, word [vbe_mode_info + 50]
    test eax, eax
    jnz .have_stride
    movzx eax, word [vbe_mode_info + 16]
.have_stride:
    shr eax, 2
    mov [framebuffer.pixels_per_scanline], eax
    mov al, [vbe_mode_info + 25]
    mov [framebuffer.bits_per_pixel], al
    mov al, [vbe_mode_info + 32]
    mov [framebuffer.red_position], al
    mov al, [vbe_mode_info + 36]
    mov [framebuffer.blue_position], al
    mov byte [framebuffer.valid], 1
.done:
    ret

; Keep every EDD request at 32KiB or less. Starting at a 32KiB-aligned
; destination then guarantees that even DMA-limited BIOS implementations do
; not cross a physical 64KiB transfer boundary.
load_payload:
    mov word [remaining], PAYLOAD_SECTORS
    mov word [dap.segment], PAYLOAD_SEGMENT
    mov dword [dap.lba], PAYLOAD_LBA
.next:
    mov ax, [remaining]
    test ax, ax
    jz .done
    cmp ax, 64
    jbe .count_ok
    mov ax, 64
.count_ok:
    mov [dap.count], ax
    mov dl, [boot_drive]
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc disk_error
    mov ax, [dap.count]
    sub [remaining], ax
    imul bx, ax, 0x20               ; one sector = 0x20 paragraphs
    add [dap.segment], bx
    add word [dap.lba], ax
    adc word [dap.lba + 2], 0
    jmp .next
.done:
    ret

build_page_tables:
    xor ax, ax
    mov es, ax
    mov di, PML4_PHYS
    mov cx, ((2 + PD_COUNT) * 4096) / 2
    rep stosw
    mov dword [es:PML4_PHYS], PDPT_PHYS | 3
    mov dword [es:PDPT_PHYS], PD_PHYS | 3
    mov dword [es:PDPT_PHYS + 8], (PD_PHYS + 0x1000) | 3
    mov dword [es:PDPT_PHYS + 16], (PD_PHYS + 0x2000) | 3
    mov dword [es:PDPT_PHYS + 24], (PD_PHYS + 0x3000) | 3
    mov di, PD_PHYS
    mov eax, 0x83                  ; present, writable, 2MiB page
    mov cx, PD_COUNT * 512         ; identity-map the first 4GiB
.pd:
    mov [es:di], eax
    mov dword [es:di + 4], 0
    add eax, 0x200000
    add di, 8
    loop .pd
    mov ax, LOAD_SEGMENT
    mov es, ax
    ret

disk_error:
    mov si, disk_error_text
    call print
.halt:
    cli
    hlt
    jmp .halt

align 8
gdt:
    dq 0
    dq 0x00af9a000000ffff          ; 0x08 64-bit code
    dq 0x00cf92000000ffff          ; 0x10 data
    dq 0x00cf9a000000ffff          ; 0x18 32-bit code (BIOS thunk)
    dq 0x00009a008000ffff          ; 0x20 16-bit code, base LOAD_SEGMENT
    dq 0x000092008000ffff          ; 0x28 16-bit data, base LOAD_SEGMENT
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt - 1
    dd (LOAD_SEGMENT << 4) + gdt
gdt_descriptor64:
    dw gdt_end - gdt - 1
    dq (LOAD_SEGMENT << 4) + gdt
real_idtr:
    dw 0x3ff
    dd 0

BITS 64
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x90000
    xor rbp, rbp
    mov rdi, (LOAD_SEGMENT << 4) + e820_entries
    movzx esi, word [(LOAD_SEGMENT << 4) + e820_count]
    movzx edx, byte [(LOAD_SEGMENT << 4) + boot_drive]
    mov rcx, (LOAD_SEGMENT << 4) + framebuffer
    mov r8, (LOAD_SEGMENT << 4) + bios_disk_read
    mov rax, PAYLOAD_PHYS
    call rax
.halt:
    cli
    hlt
    jmp .halt

; UINT32 bios_disk_read(UINT64 lba, UINT32 count), SysV ABI, called from long
; mode: drops to real mode, reads count (<= BOUNCE_SECTORS) sectors from the
; BIOS boot drive into the bounce buffer, returns 0 or the INT 13h status.
bios_disk_read:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    pushfq
    cli
    mov rax, cr3
    mov rdx, rax
    shr rdx, 32
    jnz .unreachable_cr3
    mov [abs L(saved_cr3)], eax
    mov [abs L(saved_rsp)], rsp
    sidt [abs L(saved_idtr)]
    sgdt [abs L(saved_gdtr)]
    mov ax, cs
    mov [abs L(saved_cs)], ax
    mov ax, ss
    mov [abs L(saved_ss)], ax
    mov [abs L(dap.lba)], rdi
    mov [abs L(dap.count)], si
    mov word [abs L(dap.offset)], 0
    mov word [abs L(dap.segment)], BOUNCE_SEGMENT
    lgdt [abs L(gdt_descriptor64)]
    push 0x18
    mov eax, L(.compat32)
    push rax
    retfq
.unreachable_cr3:
    mov eax, 0x100
    jmp .out

BITS 32
.compat32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr0
    and eax, 0x7fffffff             ; paging off: long mode deactivates
    mov cr0, eax
    mov ecx, 0xc0000080
    rdmsr
    and eax, ~(1 << 8)
    wrmsr
    jmp 0x20:.prot16

BITS 16
.prot16:
    mov ax, 0x28
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr0
    and al, 0xfe
    mov cr0, eax
    jmp LOAD_SEGMENT:.real16
.real16:
    mov ax, LOAD_SEGMENT
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000
    lidt [real_idtr]
    sti
    mov dl, [boot_drive]
    mov si, dap
    mov ah, 0x42
    int 0x13
    cli
    mov byte [disk_status], 0
    jnc .status_done
    test ah, ah
    jnz .status_ah
    mov ah, 0xff
.status_ah:
    mov [disk_status], ah
.status_done:
    lgdt [gdt_descriptor]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword 0x18:L(.prot32)

BITS 32
.prot32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, [L(saved_cr3)]
    mov cr3, eax
    mov ecx, 0xc0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    jmp 0x08:L(.long64)

BITS 64
.long64:
    lgdt [abs L(saved_gdtr)]
    lidt [abs L(saved_idtr)]
    movzx eax, word [abs L(saved_ss)]
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [abs L(saved_rsp)]
    movzx eax, word [abs L(saved_cs)]
    push rax
    lea rax, [rel .cs_reloaded]
    push rax
    retfq
.cs_reloaded:
    movzx eax, byte [abs L(disk_status)]
.out:
    popfq
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

BITS 16
saved_cr3 dd 0
saved_rsp dq 0
saved_idtr: times 10 db 0
saved_gdtr: times 10 db 0
saved_cs dw 0
saved_ss dw 0
disk_status db 0
boot_drive db 0
e820_count dw 0
remaining dw 0
banner db 'PureDarwin xnu-loader legacy stage2', 13, 10, 0
disk_error_text db 'xnu-loader: payload disk read failed', 13, 10, 0

align 8
framebuffer:
.base dq 0
.width dd 0
.height dd 0
.pixels_per_scanline dd 0
.bits_per_pixel db 0
.red_position db 0
.blue_position db 0
.valid db 0

align 16
vbe_mode_info:
    times 256 db 0

align 16
vbe_controller_info:
    times 512 db 0
mode_list_offset dw 0
mode_list_segment dw 0
best_vbe_mode dw 0
current_vbe_mode dw 0
best_vbe_pixels dd 0

align 4
dap:
    db 0x10, 0
.count   dw 0
.offset  dw 0
.segment dw 0
.lba     dq 0

align 16
e820_entries:
    times E820_MAX_ENTRIES * 24 db 0

stage2_end:
