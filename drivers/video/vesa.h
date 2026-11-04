#ifndef DRIVERS_VIDEO_VESA_H
#define DRIVERS_VIDEO_VESA_H

#include <stdint.h>
#include <stddef.h>

#define VESA_WIDTH  1024U
#define VESA_HEIGHT 768U
#define VESA_BPP    32U

typedef struct {
    volatile uint8_t *framebuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bits_per_pixel;
    uint8_t framebuffer_type;
    uint8_t red_position;
    uint8_t red_mask_size;
    uint8_t green_position;
    uint8_t green_mask_size;
    uint8_t blue_position;
    uint8_t blue_mask_size;
} vesa_mode_t;

/*
 * Parse the Multiboot2 information block and attach to its framebuffer.
 * Returns 0 if a supported 1024x768 32-bit RGB framebuffer is found.
 */
int vesa_init(uint32_t multiboot_magic, uint64_t multiboot_info_phys);

/* Access framebuffer properties and raw front/back pointers. */
const vesa_mode_t *vesa_mode(void);
volatile uint32_t *vesa_frontbuffer(void);
uint32_t *vesa_backbuffer(void);

/* Copy the software backbuffer to the raw framebuffer memory. */
int vesa_present(void);

/* Pack 8-bit RGB channels into the mode's pixel layout. */
uint32_t vesa_rgb(uint8_t red, uint8_t green, uint8_t blue);

#endif
