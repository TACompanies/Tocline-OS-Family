# Tocdesk OS - ISO Build System
# Build-Prozess: Kernel kompilieren → Multiboot-ELF erzeugen → ISO mit GRUB erstellen

# --- Toolchain ---
CC := gcc
LD := ld
QEMU := qemu-system-i386
GRUBMKRESCUE := grub-mkrescue

# --- Compiler/Linker Flags ---
CFLAGS := -m32 -ffreestanding -O2 -Wall -Wextra -Iinclude
LDFLAGS := -m elf_i386 -T build/linker.ld

# --- Directories ---
OBJDIR := obj
BINDIR := bin
ISODIR := iso

# --- Kernel Build ---
KERNEL_C := $(wildcard kernel/*.c)
KERNEL_S := $(wildcard kernel/*.S)
KERNEL_OBJS := $(patsubst kernel/%.c,$(OBJDIR)/kernel/%.o,$(KERNEL_C)) \
               $(patsubst kernel/%.S,$(OBJDIR)/kernel/%.o,$(KERNEL_S))

KERNEL_ELF := $(BINDIR)/kernel.elf
ISO := $(BINDIR)/tocdesk.iso

# --- Targets ---
.PHONY: all run clean

all: $(ISO)

$(OBJDIR)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/kernel/%.o: kernel/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^

$(ISO): $(KERNEL_ELF)
	@mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL_ELF) $(ISODIR)/boot/tocdesk.elf
	@echo 'set timeout=1' > $(ISODIR)/boot/grub/grub.cfg
	@echo 'set default=0' >> $(ISODIR)/boot/grub/grub.cfg
	@echo 'menuentry "Tocdesk OS" { multiboot /boot/tocdesk.elf; boot; }' >> $(ISODIR)/boot/grub/grub.cfg
	$(GRUBMKRESCUE) -o $@ $(ISODIR)

run: $(ISO)
	$(QEMU) -cdrom $(ISO)

clean:
	rm -rf $(OBJDIR) $(BINDIR) $(ISODIR)
	@echo "Clean complete."
