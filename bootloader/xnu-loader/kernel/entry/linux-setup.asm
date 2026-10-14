; evil hacky code to boot as if we're Linux (real-mode setup)

BITS 16
ORG 0

; 23 setup sectors: code, boot_params at 0x1000, stack below 0x3000.
%define SETUP_SECTS 23
%define BOOT_PARAMS 0x1000
%define STACK_TOP 0x3000
%define E820_MAX 128
%ifndef SYSSIZE
%define SYSSIZE 0
%endif
%ifndef INIT_SIZE
%define INIT_SIZE 0
%endif

    times 0x1f1-($-$$) db 0
setup_sects     db SETUP_SECTS
root_flags      dw 0
syssize         dd SYSSIZE
ram_size        dw 0
vid_mode        dw 0xffff
root_dev        dw 0
boot_flag       dw 0xaa55

; 0x200: the 16-bit protocol enters here with CS = setup segment + 0x20.
    jmp short realmode_entry
header          db 'HdrS'
version         dw 0x020f
realmode_swtch  dd 0
start_sys_seg   dw 0
kernel_version  dw version_string - 0x200
type_of_loader  db 0
loadflags       db 0x01            ; LOADED_HIGH
setup_move_size dw 0
code32_start    dd 0x100000
ramdisk_image   dd 0
ramdisk_size    dd 0
bootsect_kludge dd 0
heap_end_ptr    dw 0
ext_loader_ver  db 0
ext_loader_type db 0
cmd_line_ptr    dd 0
initrd_addr_max dd 0x7fffffff
kernel_alignment dd 0x1000
relocatable_kernel db 1
min_alignment   db 12
xloadflags      dw 0x0001          ; XLF_KERNEL_64: long-mode entry at +0x200
cmdline_size    dd 0x7ff
hardware_subarch dd 0
hardware_subarch_data dq 0
payload_offset  dd 0
payload_length  dd 0
setup_data      dq 0
pref_address    dq 0x10000000      ; the payload's link address
init_size       dd INIT_SIZE
handover_offset dd 0
kernel_info_offset dd 0
header_end:

realmode_entry:
    cli
    cld
    mov ax, cs
    sub ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STACK_TOP
    sti

    ; boot_params = zero page plus the (loader-patched) setup header.
    mov di, BOOT_PARAMS
    mov cx, 0x1000
    xor al, al
    rep stosb
    mov si, 0x1f1
    mov di, BOOT_PARAMS + 0x1f1
    mov cx, header_end - 0x1f1
    rep movsb

    call enable_a20
    call collect_e820
    cmp byte [BOOT_PARAMS + 0x1e8], 0
    jne .have_e820
    mov si, no_e820_message
    jmp fatal
.have_e820:
    call setup_vbe

    ; Linear addresses depend on where the loader put us.
    cli
    xor ebx, ebx
    mov bx, ds
    shl ebx, 4
    mov eax, ebx
    add eax, gdt
    mov [gdt_pointer + 2], eax
    mov eax, ebx
    add eax, protected_entry
    mov [protected_far + 0], eax

    o32 lgdt [gdt_pointer]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp dword far [protected_far]

BITS 32
protected_entry:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    lea esp, [ebx + STACK_TOP]
    lea esi, [ebx + BOOT_PARAMS]
    mov eax, [esi + 0x214]         ; code32_start
    xor ebp, ebp
    xor edi, edi
    xor ebx, ebx
    jmp eax

BITS 16
fatal:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    jmp fatal
.halt:
    cli
    hlt
    jmp .halt

enable_a20:
    mov ax, 0x2401
    int 0x15
    in al, 0x92
    or al, 2
    and al, 0xfe
    out 0x92, al
    ret

collect_e820:
    xor ebx, ebx
    mov di, BOOT_PARAMS + 0x2d0
.next:
    cmp byte [BOOT_PARAMS + 0x1e8], E820_MAX
    jae .done
    mov eax, 0xe820
    mov edx, 0x534d4150
    mov ecx, 20
    int 0x15
    jc .done
    cmp eax, 0x534d4150
    jne .done
    add di, 20
    inc byte [BOOT_PARAMS + 0x1e8]
    test ebx, ebx
    jnz .next
.done:
    ret

; Largest 32bpp linear mode, reported as a VESA LFB in screen_info.
setup_vbe:
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
    mov byte [BOOT_PARAMS + 0x0f], 0x23        ; VIDEO_TYPE_VLFB
    mov ax, [vbe_mode_info + 18]
    mov [BOOT_PARAMS + 0x12], ax
    mov ax, [vbe_mode_info + 20]
    mov [BOOT_PARAMS + 0x14], ax
    mov word [BOOT_PARAMS + 0x16], 32
    mov eax, [vbe_mode_info + 40]
    mov [BOOT_PARAMS + 0x18], eax
    mov ax, [vbe_mode_info + 50]
    test ax, ax
    jnz .have_stride
    mov ax, [vbe_mode_info + 16]
.have_stride:
    mov [BOOT_PARAMS + 0x24], ax
    mov eax, [vbe_mode_info + 31]              ; mask sizes and positions
    mov [BOOT_PARAMS + 0x26], eax
    mov eax, [vbe_mode_info + 35]
    mov [BOOT_PARAMS + 0x2a], eax
.done:
    ret

gdt:
    dq 0
    dq 0
    dq 0x00cf9a000000ffff                      ; 0x10: __BOOT_CS
    dq 0x00cf92000000ffff                      ; 0x18: __BOOT_DS
gdt_end:
gdt_pointer:
    dw gdt_end - gdt - 1
    dd 0
protected_far:
    dd 0
    dw 0x10

mode_list_offset dw 0
mode_list_segment dw 0
best_vbe_mode dw 0
current_vbe_mode dw 0
best_vbe_pixels dd 0
vbe_controller_info times 512 db 0
vbe_mode_info times 256 db 0

no_e820_message db 'xnu-loader: BIOS E820 memory map unavailable', 13, 10, 0
version_string db 'xnu-loader', 0

%if ($-$$) > BOOT_PARAMS
%error "setup code overlaps boot_params"
%endif
    times (SETUP_SECTS + 1) * 512 - ($-$$) db 0
