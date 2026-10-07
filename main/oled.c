/* Minimal SSD1306 (0.91", I2C) driver for the ESP-IDF 5.x I2C master driver.
 * Custom-wired OLED: SDA / SCL below. Only text drawing — enough for the
 * music library UI. */

#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "oled.h"

static const char *TAG = "oled";

#define FB_PAGES (OLED_HEIGHT / 8)             /* 4 pages for 32px */
#define FB_SIZE  (OLED_WIDTH * FB_PAGES)       /* bytes */

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_fb[FB_SIZE];
static bool s_inited = false;            /* true only after init sequence OK */

/* ------------------------------------------------------------------ */
/* Font: 5x7, ASCII 0x20..0x7E. LSB of each byte is the top pixel,      */
/* exactly like Adafruit_GFX's built-in glcdfont.                       */
/* ------------------------------------------------------------------ */
static const uint8_t FONT5X7[][5] = {
    {0x00,0x00,0x00,0x00,0x00},/*SP*/ {0x00,0x00,0x5f,0x00,0x00},/*!*/
    {0x00,0x07,0x00,0x07,0x00},/*"*/ {0x14,0x7f,0x14,0x7f,0x14},/*#*/
    {0x24,0x2a,0x7f,0x2a,0x12},/*$*/ {0x23,0x13,0x08,0x64,0x62},/*%*/
    {0x36,0x49,0x55,0x22,0x50},/*&*/ {0x00,0x05,0x03,0x00,0x00},/*'*/
    {0x00,0x1c,0x22,0x41,0x00},/*(*/ {0x00,0x41,0x22,0x1c,0x00},/*)*/
    {0x14,0x08,0x3e,0x08,0x14},/***/ {0x08,0x08,0x3e,0x08,0x08},/*+*/
    {0x00,0x50,0x30,0x00,0x00},/*,*/ {0x08,0x08,0x08,0x08,0x08},/*-*/
    {0x00,0x60,0x60,0x00,0x00},/*.*/ {0x20,0x10,0x08,0x04,0x02},/*/*/
    {0x3e,0x51,0x49,0x45,0x3e},/*0*/ {0x00,0x42,0x7f,0x40,0x00},/*1*/
    {0x42,0x61,0x51,0x49,0x46},/*2*/ {0x21,0x41,0x45,0x4b,0x31},/*3*/
    {0x18,0x14,0x12,0x7f,0x10},/*4*/ {0x27,0x45,0x45,0x45,0x39},/*5*/
    {0x3c,0x4a,0x49,0x49,0x30},/*6*/ {0x01,0x71,0x09,0x05,0x03},/*7*/
    {0x36,0x49,0x49,0x49,0x36},/*8*/ {0x06,0x49,0x49,0x29,0x1e},/*9*/
    {0x00,0x36,0x36,0x00,0x00},/*:*/ {0x00,0x56,0x36,0x00,0x00},/*;*/
    {0x08,0x14,0x22,0x41,0x00},/*<*/ {0x14,0x14,0x14,0x14,0x14},/*=*/
    {0x00,0x41,0x22,0x14,0x08},/*>*/ {0x02,0x01,0x51,0x09,0x06},/*?*/
    {0x32,0x49,0x79,0x41,0x3e},/*@*/ {0x7e,0x11,0x11,0x11,0x7e},/*A*/
    {0x7f,0x49,0x49,0x49,0x36},/*B*/ {0x3e,0x41,0x41,0x41,0x22},/*C*/
    {0x7f,0x41,0x41,0x22,0x1c},/*D*/ {0x7f,0x49,0x49,0x49,0x41},/*E*/
    {0x7f,0x09,0x09,0x09,0x01},/*F*/ {0x3e,0x41,0x49,0x49,0x7a},/*G*/
    {0x7f,0x08,0x08,0x08,0x7f},/*H*/ {0x00,0x41,0x7f,0x41,0x00},/*I*/
    {0x20,0x40,0x41,0x3f,0x01},/*J*/ {0x7f,0x08,0x14,0x22,0x41},/*K*/
    {0x7f,0x40,0x40,0x40,0x40},/*L*/ {0x7f,0x02,0x0c,0x02,0x7f},/*M*/
    {0x7f,0x04,0x08,0x10,0x7f},/*N*/ {0x3e,0x41,0x41,0x41,0x3e},/*O*/
    {0x7f,0x09,0x09,0x09,0x06},/*P*/ {0x3e,0x41,0x51,0x21,0x5e},/*Q*/
    {0x7f,0x09,0x19,0x29,0x46},/*R*/ {0x46,0x49,0x49,0x49,0x31},/*S*/
    {0x01,0x01,0x7f,0x01,0x01},/*T*/ {0x3f,0x40,0x40,0x40,0x3f},/*U*/
    {0x1f,0x20,0x40,0x20,0x1f},/*V*/ {0x3f,0x40,0x38,0x40,0x3f},/*W*/
    {0x63,0x14,0x08,0x14,0x63},/*X*/ {0x07,0x08,0x70,0x08,0x07},/*Y*/
    {0x61,0x51,0x49,0x45,0x43},/*Z*/ {0x00,0x7f,0x41,0x41,0x00},/*[*/
    {0x02,0x04,0x08,0x10,0x20},/*\*/ {0x00,0x41,0x41,0x7f,0x00},/*]*/
    {0x04,0x02,0x01,0x02,0x04},/*^*/ {0x40,0x40,0x40,0x40,0x40},/*_*/
    {0x00,0x01,0x02,0x04,0x00},/*`*/ {0x20,0x54,0x54,0x54,0x78},/*a*/
    {0x7f,0x48,0x44,0x44,0x38},/*b*/ {0x38,0x44,0x44,0x44,0x20},/*c*/
    {0x38,0x44,0x44,0x48,0x7f},/*d*/ {0x38,0x54,0x54,0x54,0x18},/*e*/
    {0x08,0x7e,0x09,0x01,0x02},/*f*/ {0x0c,0x52,0x52,0x52,0x3e},/*g*/
    {0x7f,0x08,0x04,0x04,0x78},/*h*/ {0x00,0x44,0x7d,0x40,0x00},/*i*/
    {0x20,0x40,0x44,0x3d,0x00},/*j*/ {0x7f,0x10,0x28,0x44,0x00},/*k*/
    {0x00,0x41,0x7f,0x40,0x00},/*l*/ {0x7c,0x04,0x18,0x04,0x78},/*m*/
    {0x7c,0x08,0x04,0x04,0x78},/*n*/ {0x38,0x44,0x44,0x44,0x38},/*o*/
    {0x7c,0x14,0x14,0x14,0x08},/*p*/ {0x08,0x14,0x14,0x18,0x7c},/*q*/
    {0x7c,0x08,0x04,0x04,0x08},/*r*/ {0x48,0x54,0x54,0x54,0x20},/*s*/
    {0x04,0x3f,0x44,0x40,0x20},/*t*/ {0x3c,0x40,0x40,0x20,0x7c},/*u*/
    {0x1c,0x20,0x40,0x20,0x1c},/*v*/ {0x3c,0x40,0x30,0x40,0x3c},/*w*/
    {0x44,0x28,0x10,0x28,0x44},/*x*/ {0x0c,0x50,0x50,0x50,0x3c},/*y*/
    {0x44,0x64,0x54,0x4c,0x44},/*z*/ {0x00,0x08,0x36,0x41,0x00},/*{*/
    {0x00,0x00,0x7f,0x00,0x00},/*|*/ {0x00,0x41,0x36,0x08,0x00},/*}*/
    {0x10,0x08,0x08,0x10,0x08},/*~*/ {0x00,0x00,0x00,0x00,0x00},/*DEL*/
};

static esp_err_t ssd1306_cmd(uint8_t cmd)
{
    uint8_t buf[2] = {0x00, cmd};          /* 0x00 = control byte for command */
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

static esp_err_t ssd1306_data(const uint8_t *data, size_t len)
{
    /* i2c_master_transmit takes one buffer; build header+data in one shot.
     * Static buffer (no heap churn per frame push) — len is bounded by
     * FB_SIZE + 1 = 513 bytes. */
    static uint8_t tmp[FB_SIZE + 1];
    tmp[0] = 0x40;                         /* 0x40 = control byte for data */
    memcpy(tmp + 1, data, len);
    return i2c_master_transmit(s_dev, tmp, len + 1, 100);
}

static void oled_clear_fb(void);
void oled_update(void);                    /* used by oled_init before its defn */
static void oled_clear_fb(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

/* Render one 5x7 glyph into the framebuffer at pixel (x, y). */
static void put_glyph(uint8_t x, uint8_t y, char c)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) return;
    if (c < ' ' || c > 0x7e) c = ' ';
    const uint8_t *g = FONT5X7[c - ' '];
    for (uint8_t col = 0; col < 5; col++) {
        if (x + col >= OLED_WIDTH) break;
        uint8_t bits = g[col];
        for (uint8_t row = 0; row < 7; row++) {
            if (y + row >= OLED_HEIGHT) break;
            if (!(bits & (1 << row))) continue;   /* LSB = top pixel */
            uint16_t fb_idx = (y + row) / 8 * OLED_WIDTH + (x + col);
            s_fb[fb_idx] |= (1 << ((y + row) % 8));
        }
    }
}

/* ------------------------------------------------------------------ */

esp_err_t oled_init(void)
{
    s_inited = false;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA,
        .scl_io_num = OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c dev: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);          /* release bus so nothing lingers */
        s_bus = NULL;
        return err;
    }

    /* Init sequence mirrors Adafruit_SSD1306 for 128x32 exactly (no mirror /
     * rotate). Segment remap 0xA1 + COM scan 0xC8 is the standard non-mirrored
     * orientation for a 0.91" 128x32 panel, matching the reference sketch. */
    static const uint8_t init_cmds[] = {
        0xAE,        /* display off */
        0xD5, 0x80,  /* display clock divide ratio = 0x80 (Adafruit default) */
        0xA8, 0x1F,  /* multiplex ratio = 31 (32-1), 128x32 panel */
        0xD3, 0x00,  /* display offset = 0 */
        0x40,        /* start line = 0 */
        0x8D, 0x14,  /* charge pump enable */
        0x20, 0x00,  /* memory mode = horizontal */
        0xA1,        /* segment remap = left-to-right (no mirror) */
        0xC8,        /* COM output scan = normal (no rotate) */
        0xDA, 0x02,  /* COM pins hardware config for 128x32 */
        0x81, 0x8F,  /* contrast = 0x8F (Adafruit 32px default) */
        0xD9, 0xF1,  /* pre-charge period */
        0xDB, 0x40,  /* VCOMH deselect level */
        0xA4,        /* resume from RAM content */
        0xA6,        /* normal display (not inverted) */
        0xAF,        /* display on */
    };
    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        err = ssd1306_cmd(init_cmds[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init cmd %u: %s", (unsigned)i, esp_err_to_name(err));
            i2c_master_bus_rm_device(s_dev); /* fully detach on failure */
            i2c_del_master_bus(s_bus);
            s_dev = NULL;
            s_bus = NULL;
            return err;
        }
    }

    s_inited = true;
    oled_clear_fb();
    oled_update();
    return ESP_OK;
}

void oled_clear(void) { oled_clear_fb(); }

void oled_draw_text(uint8_t x, uint8_t y, const char *s)
{
    uint8_t x0 = x;
    while (*s) {
        if (*s == '\n') {
            y += 8;                        /* next text row (7 px glyph + 1) */
            x = x0;
        } else {
            put_glyph(x, y, *s);
            x += 6;                        /* 5 px glyph + 1 px spacing */
        }
        s++;
    }
}

void oled_invert_region(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    if (x > OLED_WIDTH) x = OLED_WIDTH;
    if (y > OLED_HEIGHT) y = OLED_HEIGHT;
    if (w > OLED_WIDTH - x) w = OLED_WIDTH - x;
    if (h > OLED_HEIGHT - y) h = OLED_HEIGHT - y;
    for (uint16_t py = y; py < y + h; py++) {
        for (uint16_t px = x; px < x + w; px++) {
            uint16_t idx = py / 8 * OLED_WIDTH + px;
            s_fb[idx] ^= (1 << (py % 8));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Basic graphics primitives (framebuffer only; caller pushes).        */
/* ------------------------------------------------------------------ */

void oled_set_pixel(uint8_t x, uint8_t y)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) return;
    uint16_t idx = y / 8 * OLED_WIDTH + x;
    s_fb[idx] |= (1 << (y % 8));
}

void oled_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT || w == 0 || h == 0) return;
    if (w > OLED_WIDTH - x)  w = OLED_WIDTH - x;
    if (h > OLED_HEIGHT - y) h = OLED_HEIGHT - y;
    for (uint8_t dy = 0; dy < h; dy++)
        for (uint8_t dx = 0; dx < w; dx++)
            s_fb[(y + dy) / 8 * OLED_WIDTH + x + dx] |= (1 << ((y + dy) % 8));
}

void oled_draw_hline(uint8_t x, uint8_t y, uint8_t w) { oled_fill_rect(x, y, w, 1); }
void oled_draw_vline(uint8_t x, uint8_t y, uint8_t h) { oled_fill_rect(x, y, 1, h); }

static int isqrt(int n)
{
    int r = 0;
    for (int b = 1 << 15; b; b >>= 1) {   /* integer square root, no libm */
        int t = r + b;
        if (t * t <= n) r = t;
    }
    return r;
}

void oled_fill_circle(uint8_t cx, uint8_t cy, uint8_t r)
{
    for (int dy = -(int)r; dy <= (int)r; dy++) {
        int h = isqrt((int)r * r - dy * dy);
        oled_fill_rect((uint8_t)(cx - h), (uint8_t)(cy + dy), (uint8_t)(2 * h + 1), 1);
    }
}

void oled_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2)
{
    /* Rasterize by horizontal scanlines. Collect the two edge intersections
     * at each y (half-open so vertices aren't double-counted), fill between. */
    int ymin = y0, ymax = y0;
    if (y1 < ymin) ymin = y1;
    if (y1 > ymax) ymax = y1;
    if (y2 < ymin) ymin = y2;
    if (y2 > ymax) ymax = y2;
    for (int y = ymin; y <= ymax; y++) {
        int xs[2], n = 0;
        /* helper closure via a tiny local scan routine */
        for (int e = 0; e < 3; e++) {
            int ax, ay, bx, by;
            if (e == 0) { ax = x0; ay = y0; bx = x1; by = y1; }
            else if (e == 1) { ax = x1; ay = y1; bx = x2; by = y2; }
            else { ax = x2; ay = y2; bx = x0; by = y0; }
            if (by == ay) continue;                 /* horizontal edge */
            int lo = (ay < by) ? ay : by;
            int hi = (ay > by) ? ay : by;
            if (y < lo || y >= hi) continue;        /* half-open */
            if (n >= 2) continue;
            xs[n++] = ax + (int)((long)(bx - ax) * (y - ay) / (by - ay));
        }
        if (n == 2) {
            int a = xs[0] < xs[1] ? xs[0] : xs[1];
            int b = xs[0] < xs[1] ? xs[1] : xs[0];
            oled_fill_rect((uint8_t)a, (uint8_t)y, (uint8_t)(b - a + 1), 1);
        } else if (n == 1) {
            oled_set_pixel((uint8_t)xs[0], (uint8_t)y);
        }
    }
}

void oled_draw_roundrect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint8_t r)
{
    if (w < 2 || h < 2) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    oled_draw_hline(x + r, y, w - 2 * r);          /* top */
    oled_draw_hline(x + r, y + h - 1, w - 2 * r);  /* bottom */
    oled_draw_vline(x, y + r, h - 2 * r);          /* left */
    oled_draw_vline(x + w - 1, y + r, h - 2 * r);  /* right */
    for (int dy = -(int)r; dy <= (int)r; dy++) {   /* 4 corner arcs */
        int dx = isqrt((int)r * r - dy * dy);
        oled_set_pixel(x + r - (uint8_t)dx, y + r + (uint8_t)dy);
        oled_set_pixel(x + w - 1 - r + (uint8_t)dx, y + r + (uint8_t)dy);
        oled_set_pixel(x + r - (uint8_t)dx, y + h - 1 - r + (uint8_t)dy);
        oled_set_pixel(x + w - 1 - r + (uint8_t)dx, y + h - 1 - r + (uint8_t)dy);
    }
}

void oled_update(void)
{
    if (!s_inited) return;           /* panel mati / init gagal -> skip I2C */
    /* Horizontal addressing, exactly like Adafruit_SSD1306::display():
     * set the column range, set the page range, then push the whole buffer
     * in one shot. This is what prevents the scrambled/rotated output. */
    ssd1306_cmd(0x21);                     /* column address range */
    ssd1306_cmd(0x00);
    ssd1306_cmd(OLED_WIDTH - 1);
    ssd1306_cmd(0x22);                     /* page address range */
    ssd1306_cmd(0x00);
    ssd1306_cmd(FB_PAGES - 1);
    ssd1306_data(s_fb, FB_SIZE);
}
