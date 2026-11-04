CROSS    := x86_64-linux-gnu-
CC       := $(CROSS)gcc
LD       := $(CROSS)ld

CFLAGS = -Wall -Wextra -ffreestanding -O3 -march=native -mno-red-zone \
            -fno-exceptions -fno-stack-protector -fno-pic -fno-pie \
            -m64 -mcmodel=kernel -I.
LDFLAGS  := -nostdlib -z max-page-size=0x1000 -T linker.ld

KERNEL   := my_os.bin
ISO      := my_os.iso
STAGING  := iso

OBJECTS := \
    boot/boot.o \
    kernel/main.o \
    kernel/mem.o \
    drivers/video/vesa.o \
    gui/wm.o \
    drivers/net/e1000.o \
    net/stack.o \
    compat/pe_parser.o \
    compat/syscalls.o \
    loader/pollen.o \
    fs/vfs.o \
    fs/ramdisk.o \
    gui/explorer.o

.PHONY: all clean check-multiboot iso-image

all: $(ISO)

$(KERNEL): $(OBJECTS) linker.ld
    $(LD) $(LDFLAGS) -o $@ $(OBJECTS)

check-multiboot: $(KERNEL)
    grub-file --is-x86-multiboot2 $(KERNEL)

iso-image: check-multiboot
    mkdir -p $(STAGING)/boot/grub
    cp $(KERNEL) $(STAGING)/boot/my_os.bin
    cp boot/grub.cfg $(STAGING)/boot/grub/grub.cfg
    grub-mkrescue -o $(ISO) $(STAGING)

$(ISO): iso-image

boot/boot.o: boot/boot.asm
    nasm -f elf64 $< -o $@

%.o: %.c
    $(CC) $(CFLAGS) -c $< -o $@

clean:
    rm -f $(OBJECTS) $(KERNEL) $(ISO)
    rm -rf $(STAGING)
