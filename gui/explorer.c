#include <stdint.h>
#include "../drivers/video/vesa.h"

#define SCREEN_WIDTH  1024
#define SCREEN_HEIGHT 768

#define COLOR_DESKTOP  0x002B579A
#define COLOR_WINDOW   0x00F0F0F0
#define COLOR_TITLE    0x000078D7
#define COLOR_TITLE_TX 0x00FFFFFF
#define COLOR_TEXT     0x00101010
#define COLOR_BORDER   0x00808080
#define COLOR_TOOLBAR  0x00E4E4E4
#define COLOR_SELECTED 0x00CDE8FF

static uint32_t rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    return vesa_rgb(red, green, blue);
}

static void fill_rect(uint32_t *pixels, int x, int y, int width, int height,
                      uint32_t color)
{
    if (pixels == 0 || width <= 0 || height <= 0) {
        return;
    }

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width;
    int y1 = y + height;

    if (x1 > SCREEN_WIDTH) {
        x1 = SCREEN_WIDTH;
    }
    if (y1 > SCREEN_HEIGHT) {
        y1 = SCREEN_HEIGHT;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int row = y0; row < y1; ++row) {
        uint32_t *line = pixels + (uint32_t)row * SCREEN_WIDTH;
        for (int column = x0; column < x1; ++column) {
            line[column] = color;
        }
    }
}

static void draw_char(uint32_t *pixels, int x, int y, char c, uint32_t color)
{
    /*
     * Tiny readable 5x7 glyph subset for the labels used by this UI.
     * Unsupported characters are rendered as blank.
     */
    static const uint8_t glyphs[16][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
        {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
        {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}, /* 3 */
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
        {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}, /* 5 */
        {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
        {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
        {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}, /* 9 */
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
        {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
        {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
        {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}  /* F */
    };

    if (pixels == 0) {
        return;
    }

    int index = -1;
    if (c >= '0' && c <= '9') {
        index = c - '0';
    } else if (c >= 'A' && c <= 'F') {
        index = 10 + c - 'A';
    }

    if (index < 0) {
        return;
    }

    for (int row = 0; row < 7; ++row) {
        for (int column = 0; column < 5; ++column) {
            if ((glyphs[index][row] & (1U << (4 - column))) != 0) {
                int px = x + column;
                int py = y + row;
                if (px >= 0 && px < SCREEN_WIDTH &&
                    py >= 0 && py < SCREEN_HEIGHT) {
                    pixels[(uint32_t)py * SCREEN_WIDTH + (uint32_t)px] = color;
                }
            }
        }
    }
}

static void draw_text(uint32_t *pixels, int x, int y, const char *text,
                      uint32_t color)
{
    if (text == 0) {
        return;
    }

    while (*text != '\0') {
        draw_char(pixels, x, y, *text, color);
        x += 8;
        ++text;
    }
}

/*
 * Draws a static explorer window into the video backbuffer.
 * Call vesa_present() afterward to copy the backbuffer to the framebuffer.
 */
void explorer_draw(void)
{
    uint32_t *pixels = vesa_backbuffer();
    if (pixels == 0) {
        return;
    }

    uint32_t desktop = rgb(43, 87, 154);
    uint32_t window = rgb(240, 240, 240);
    uint32_t title = rgb(0, 120, 215);
    uint32_t title_text = rgb(255, 255, 255);
    uint32_t text = rgb(16, 16, 16);
    uint32_t border = rgb(128, 128, 128);
    uint32_t toolbar = rgb(228, 228, 228);
    uint32_t selected = rgb(205, 232, 255);

    fill_rect(pixels, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, desktop);

    int wx = 150;
    int wy = 100;
    int ww = 724;
    int wh = 520;

    fill_rect(pixels, wx - 1, wy - 1, ww + 2, wh + 2, border);
    fill_rect(pixels, wx, wy, ww, wh, window);
    fill_rect(pixels, wx, wy, ww, 30, title);

    draw_text(pixels, wx + 12, wy + 11, "POLLEN FILE EXPLORER", title_text);

    fill_rect(pixels, wx + 8, wy + 38, ww - 16, 32, toolbar);
    draw_text(pixels, wx + 20, wy + 50, "FILE", text);
    draw_text(pixels, wx + 76, wy + 50, "EDIT", text);
    draw_text(pixels, wx + 132, wy + 50, "VIEW", text);

    /* Address bar and current path. */
    fill_rect(pixels, wx + 12, wy + 82, ww - 24, 28, rgb(255, 255, 255));
    fill_rect(pixels, wx + 12, wy + 82, ww - 24, 1, border);
    fill_rect(pixels, wx + 12, wy + 109, ww - 24, 1, border);
    draw_text(pixels, wx + 24, wy + 93, "C", text);

    /* File-list headings and a selected placeholder row. */
    fill_rect(pixels, wx + 12, wy + 122, ww - 24, 24, toolbar);
    draw_text(pixels, wx + 24, wy + 130, "NAME", text);
    draw_text(pixels, wx + 284, wy + 130, "TYPE", text);
    draw_text(pixels, wx + 452, wy + 130, "SIZE", text);

    fill_rect(pixels, wx + 13, wy + 148, ww - 26, 26, selected);
    draw_text(pixels, wx + 24, wy + 158, "NO FILES", text);

    /* Status bar. */
    fill_rect(pixels, wx + 1, wy + wh - 24, ww - 2, 23, toolbar);
    draw_text(pixels, wx + 12, wy + wh - 16, "0 ITEMS", text);
}
