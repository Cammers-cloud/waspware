#include <stdint.h>
#include "mem.h"

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289U
#define VGA_TEXT_BUFFER ((volatile uint16_t *)0xB8000)
#define VGA_COLUMNS 80
#define VGA_ROWS 25

static void vga_clear(void)
{
    for (uint32_t i = 0; i < VGA_COLUMNS * VGA_ROWS; ++i) {
        VGA_TEXT_BUFFER[i] = (uint16_t)0x0720;
    }
}

static void vga_write_at(uint32_t row, uint32_t column,
                         const char *text, uint8_t attribute)
{
    if (row >= VGA_ROWS || column >= VGA_COLUMNS || text == 0) {
        return;
    }

    uint32_t offset = row * VGA_COLUMNS + column;
    while (*text != '\0' && column < VGA_COLUMNS) {
        VGA_TEXT_BUFFER[offset++] =
            (uint16_t)(((uint16_t)attribute << 8) | (uint8_t)*text++);
        ++column;
    }
}

static void halt_forever(void)
{
    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info)
{
    vga_clear();
    vga_write_at(0, 0, "Pollen x86_64 kernel", 0x0F);
    vga_write_at(1, 0, "Booting core services...", 0x07);

    if (multiboot_magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        vga_write_at(3, 0, "ERROR: invalid Multiboot2 handoff", 0x4F);
        halt_forever();
    }

    if (mem_init(multiboot_magic, (uint64_t)multiboot_info) != 0) {
        vga_write_at(3, 0, "ERROR: physical memory initialization failed", 0x4F);
        halt_forever();
    }

    vga_write_at(3, 0, "Physical page-frame allocator initialized", 0x0A);
    vga_write_at(4, 0, "Identity map active for physical memory below 1 GiB", 0x07);

    halt_forever();
}
