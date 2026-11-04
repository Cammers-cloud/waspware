BITS 32

SECTION .multiboot
ALIGN 8

mb2_header:
    dd 0xE85250D6                  ; Multiboot2 header magic
    dd 0                           ; Architecture: i386
    dd mb2_header_end - mb2_header
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header))

    ; Required end tag
    dw 0
    dw 0
    dd 8
mb2_header_end:

SECTION .bss
ALIGN 16
stack_bottom:
    resb 16384
stack_top:

ALIGN 4096
pml4:
    resb 4096
pdpt:
    resb 4096
page_directory:
    resb 4096

SECTION .text
GLOBAL _start
EXTERN kernel_main

_start:
    cli
    cld

    ; Multiboot2 entry: EAX = boot magic, EBX = boot information address.
    cmp eax, 0x36D76289
    jne boot_halt

    mov [boot_magic], eax
    mov [boot_info], ebx

    ; Set up a known stack before using any calls.
    mov esp, stack_top

    ; Clear and build a 1 GiB identity map using 2 MiB pages.
    ; This maps physical addresses 0 through 1 GiB.
    mov edi, pml4
    xor eax, eax
    mov ecx, (4096 * 3) / 4
    rep stosd

    mov dword [pml4], pdpt | 0x003
    mov dword [pdpt], page_directory | 0x003

    mov edi, page_directory
    mov eax, 0x00000083             ; Present | Writable | Page Size (2 MiB)
    mov ecx, 512
.map_pages:
    mov [edi], eax
    mov dword [edi + 4], 0
    add eax, 0x00200000
    add edi, 8
    loop .map_pages

    ; Enable PAE.
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Load PML4 physical address.
    mov eax, pml4
    mov cr3, eax

    ; Enable long mode in EFER.
    mov ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or eax, 1 << 8                  ; EFER.LME
    wrmsr

    ; Enable paging and protected-mode paging features.
    mov eax, cr0
    or eax, (1 << 31) | (1 << 0)   ; CR0.PG | CR0.PE
    mov cr0, eax

    ; Load the 64-bit GDT and far-jump into long mode.
    lgdt [gdt64.pointer]
    jmp 0x08:long_mode_entry

boot_halt:
    cli
.hang:
    hlt
    jmp .hang

BITS 64
long_mode_entry:
    ; Set data segments to the 64-bit data descriptor.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    ; Ensure a properly aligned stack. SysV x86-64 expects RSP aligned
    ; to 16 bytes immediately before a CALL instruction.
    mov rsp, stack_top
    and rsp, -16

    ; kernel_main(magic, boot_info)
    mov edi, [boot_magic]
    mov esi, [boot_info]
    call kernel_main

.returned:
    cli
    hlt
    jmp .returned

SECTION .rodata
ALIGN 8
gdt64:
    dq 0x0000000000000000            ; Null descriptor
    dq 0x00AF9A000000FFFF            ; 64-bit code descriptor
    dq 0x00CF92000000FFFF            ; Data descriptor
.pointer:
    dw gdt64.pointer - gdt64 - 1
    dq gdt64

SECTION .bss
ALIGN 4
boot_magic:
    resd 1
boot_info:
    resd 1
