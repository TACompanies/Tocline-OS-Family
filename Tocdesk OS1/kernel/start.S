# Multiboot Header
    .section .multiboot
    .align 4
    .long 0x1badb002        # Magic number (Multiboot 1)
    .long 0x00000001        # Flags
    .long -(0x1badb002 + 0x00000001)  # Checksum

    .globl _start
    .extern kmain

    .text
_start:
    mov $0x90000, %esp
    xor %ebp, %ebp
    call kmain
    cli
.halt:
    hlt
    jmp .halt
