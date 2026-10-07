#pragma once

#include <stdint.h>
#include "esp_err.h"

/* OLED geometry (0.91" SSD1306, I2C). Override before include to change. */
#ifndef OLED_WIDTH
#define OLED_WIDTH  128
#endif
#ifndef OLED_HEIGHT
#define OLED_HEIGHT 32
#endif
#ifndef OLED_ADDR
#define OLED_ADDR   0x3C
#endif
#ifndef OLED_SDA
#define OLED_SDA    9
#endif
#ifndef OLED_SCL
#define OLED_SCL    8
#endif

/* Returns ESP_OK on success. Initializes the I2C bus + the panel. */
esp_err_t oled_init(void);

/* Clear the internal framebuffer (does NOT push to screen). */
void oled_clear(void);

/* Push the framebuffer to the panel (call after drawing). */
void oled_update(void);

/* Draw a string at (x,y). y is a pixel row; use 0,8,16,24 for a 32px panel. */
void oled_draw_text(uint8_t x, uint8_t y, const char *s);

/* Draw a filled / highlighted bar across a line (inverts that region), for
 * marking the currently-selected playlist row. Call oled_update() after. */
void oled_invert_region(uint8_t x, uint8_t y, uint8_t w, uint8_t h);

/* Basic graphics primitives. All draw into the framebuffer (no push); call
 * oled_update() after a group. Coordinates are clipped to the panel. */
void oled_set_pixel(uint8_t x, uint8_t y);
void oled_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
void oled_draw_hline(uint8_t x, uint8_t y, uint8_t w);
void oled_draw_vline(uint8_t x, uint8_t y, uint8_t h);
void oled_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2);
void oled_fill_circle(uint8_t cx, uint8_t cy, uint8_t r);
void oled_draw_roundrect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t r);
