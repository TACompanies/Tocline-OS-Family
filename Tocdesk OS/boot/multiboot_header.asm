; Minimaler Multiboot-Header für GRUB.
; Diese Datei wird in das Kernel-Binary gelinkt, damit GRUB den Kernel laden kann.
section .multiboot_header
    align 4
    dd 0x1BADB002
    dd 0x00000000
    dd -(0x1BADB002 + 0x00000000)
