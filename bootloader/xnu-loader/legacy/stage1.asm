BITS 16
ORG 0x7c00

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 1
%endif

%ifndef STAGE2_LBA
%define STAGE2_LBA 1
%endif

%define STAGE2_SEGMENT 0x0800

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    mov [boot_drive], dl

    mov ah, 0x41
    mov bx, 0x55aa
    int 0x13
    jc disk_error
    cmp bx, 0xaa55
    jne disk_error
    test cx, 1
    jz disk_error

    mov word [dap.count], STAGE2_SECTORS
    mov word [dap.offset], 0
    mov word [dap.segment], STAGE2_SEGMENT
    mov dl, [boot_drive]
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc disk_error

    mov dl, [boot_drive]
    jmp STAGE2_SEGMENT:0

disk_error:
    mov si, disk_error_text
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

boot_drive db 0
disk_error_text db 'xnu-loader: BIOS disk read failed', 13, 10, 0

align 4
dap:
    db 0x10, 0
.count   dw 0
.offset  dw 0
.segment dw 0
    dq STAGE2_LBA

times 510-($-$$) db 0
dw 0xaa55
