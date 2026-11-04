#include "vesa.h"

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289U
#define MULTIBOOT2_TAG_TYPE_END 0U
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER 8U
#define MULTIBOOT2_FRAMEBUFFER_TYPE_RGB 1U
#define MAX_IDENTITY_MAPPED_ADDRESS (1ULL << 30)

typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} multiboot_info_header_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} multiboot_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    uint8_t color_info[6];
} multiboot_tag_framebuffer_rgb_t;

static vesa_mode_t active_mode;
static uint32_t backbuffer[VESA_WIDTH * VESA_HEIGHT];
static int mode_ready;

static int range_inside(uint64_t base, uint64_t length, uint64_t limit)
{
    return base <= limit && length <= limit - base;
}

static int valid_color_field(uint8_t position, uint8_t size)
{
    return size > 0 && size <= 8 && position < 32 &&
           (uint16_t)position + size <= 32;
}

int vesa_init(uint32_t multiboot_magic, uint64_t multiboot_info_phys)
{
    mode_ready = 0;

    if (multiboot_magic != MULTIBOOT2_BOOTLOADER_MAGIC ||
        multiboot_info_phys == 0 ||
        multiboot_info_phys >= MAX_IDENTITY_MAPPED_ADDRESS) {
        return -1;
    }

    const multiboot_info_header_t *info =
        (const multiboot_info_header_t *)(uintptr_t)multiboot_info_phys;

    if (info->total_size < 16 ||
        !range_inside(multiboot_info_phys, info->total_size,
                      MAX_IDENTITY_MAPPED_ADDRESS)) {
        return -2;
    }

    uint64_t info_end = multiboot_info_phys + info->total_size;
    uint64_t cursor = multiboot_info_phys + 8;
    int found = 0;

    while (cursor + sizeof(multiboot_tag_t) <= info_end) {
        const multiboot_tag_t *tag =
            (const multiboot_tag_t *)(uintptr_t)cursor;

        if (tag->size < sizeof(multiboot_tag_t) ||
            !range_inside(cursor, tag->size, info_end)) {
            return -3;
        }

        if (tag->type == MULTIBOOT2_TAG_TYPE_END) {
            break;
        }

        if (tag->type == MULTIBOOT2_TAG_TYPE_FRAMEBUFFER) {
            if (tag->size < sizeof(multiboot_tag_framebuffer_rgb_t)) {
                return -4;
            }

            const multiboot_tag_framebuffer_rgb_t *fb =
                (const multiboot_tag_framebuffer_rgb_t *)tag;

            if (fb->framebuffer_type != MULTIBOOT2_FRAMEBUFFER_TYPE_RGB ||
                fb->framebuffer_width != VESA_WIDTH ||
                fb->framebuffer_height != VESA_HEIGHT ||
                fb->framebuffer_bpp != VESA_BPP) {
                return -5;
            }

            uint64_t minimum_pitch = (uint64_t)fb->framebuffer_width * 4U;
            uint64_t framebuffer_bytes =
                (uint64_t)fb->framebuffer_pitch * fb->framebuffer_height;

            if (fb->framebuffer_pitch < minimum_pitch ||
                framebuffer_bytes == 0 ||
                !range_inside(fb->framebuffer_addr, framebuffer_bytes,
                              MAX_IDENTITY_MAPPED_ADDRESS)) {
                return -6;
            }

            uint8_t red_position = fb->color_info[0];
            uint8_t red_mask_size = fb->color_info[1];
            uint8_t green_position = fb->color_info[2];
            uint8_t green_mask_size = fb->color_info[3];
            uint8_t blue_position = fb->color_info[4];
            uint8_t blue_mask_size = fb->color_info[5];

            if (!valid_color_field(red_position, red_mask_size) ||
                !valid_color_field(green_position, green_mask_size) ||
                !valid_color_field(blue_position, blue_mask_size)) {
                return -7;
            }

            active_mode.framebuffer =
                (volatile uint8_t *)(uintptr_t)fb->framebuffer_addr;
            active_mode.width = fb->framebuffer_width;
            active_mode.height = fb->framebuffer_height;
            active_mode.pitch = fb->framebuffer_pitch;
            active_mode.bits_per_pixel = fb->framebuffer_bpp;
            active_mode.framebuffer_type = fb->framebuffer_type;
            active_mode.red_position = red_position;
            active_mode.red_mask_size = red_mask_size;
            active_mode.green_position = green_position;
            active_mode.green_mask_size = green_mask_size;
            active_mode.blue_position = blue_position;
            active_mode.blue_mask_size = blue_mask_size;
            found = 1;
            break;
        }

        uint64_t next = cursor + tag->size;
        next = (next + 7ULL) & ~7ULL;
        if (next <= cursor || next > info_end) {
            return -8;
        }
        cursor = next;
    }

    if (!found) {
        return -9;
    }

    for (uint32_t i = 0; i < VESA_WIDTH * VESA_HEIGHT; ++i) {
        backbuffer[i] = 0;
    }

    mode_ready = 1;
    return 0;
}

const vesa_mode_t *vesa_mode(void)
{
    return mode_ready ? &active_mode : (const vesa_mode_t *)0;
}

volatile uint32_t *vesa_frontbuffer(void)
{
    if (!mode_ready) {
        return (volatile uint32_t *)0;
    }

    return (volatile uint32_t *)(uintptr_t)active_mode.framebuffer;
}

uint32_t *vesa_backbuffer(void)
{
    return mode_ready ? backbuffer : (uint32_t *)0;
}

uint32_t vesa_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!mode_ready) {
        return 0;
    }

    uint32_t r = ((uint32_t)red >> (8U - active_mode.red_mask_size))
                 << active_mode.red_position;
    uint32_t g = ((uint32_t)green >> (8U - active_mode.green_mask_size))
                 << active_mode.green_position;
    uint32_t b = ((uint32_t)blue >> (8U - active_mode.blue_mask_size))
                 << active_mode.blue_position;

    return r | g | b;
}

int vesa_present(void)
{
    if (!mode_ready) {
        return -1;
    }

    /*
     * Copy each visible row, honoring firmware-provided pitch. The
     * framebuffer is volatile because it is device memory.
     */
    for (uint32_t y = 0; y < active_mode.height; ++y) {
        volatile uint32_t *destination =
            (volatile uint32_t *)(uintptr_t)
            (active_mode.framebuffer + (uint64_t)y * active_mode.pitch);
        const uint32_t *source = &backbuffer[y * active_mode.width];

        for (uint32_t x = 0; x < active_mode.width; ++x) {
            destination[x] = source[x];
        }
    }

    __asm__ volatile("" ::: "memory");
    return 0;
}
