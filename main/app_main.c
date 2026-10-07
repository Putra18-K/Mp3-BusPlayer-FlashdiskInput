/* ESP32-S3 school-bus MP3 player.
 *
 * Flow: USB flash drive (FAT32) is read via the native USB host and mounted at
 * /usb. The OLED shows the music library (2 songs, current highlighted). A
 * single button plays the NEXT song on each press, wrapping from the last track
 * back to the first. A second button RESETS playback to the first song.
 * Pressing mid-song skips immediately; a song that ends simply stops and waits
 * for the next press.
 *
 * Wiring (custom, override pin macros in the *-header if yours differ):
 *   OLED  SDA=GPIO9  SCL=GPIO8   (0.91" SSD1306, 128x32, addr 0x3C)
 *   I2S   BCLK=17 WS=15 DOUT=16 MCLK=18  -> PCM5102A -> 3.5mm jack -> preamp
 *   BTN   PLAY=12  RESET=11               (buttons to GND, internal pull-up)
 *   USB   native host: D-=GPIO19 D+=GPIO20 (fixed by S3 silicon)
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "oled.h"
#include "audio.h"
#include "usb.h"

static const char *TAG = "main";

/* Monotonic milliseconds (esp_timer; not affected by tick jitter). */
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void update_oled(void)
{
    static uint32_t last_error_ms;
    esp_err_t err = oled_update();
    if (err != ESP_OK && now_ms() - last_error_ms >= 1000) {
        last_error_ms = now_ms();
        ESP_LOGW(TAG, "OLED update failed: %s", esp_err_to_name(err));
    }
}

#define PIN_BTN_PLAY  12
#define PIN_BTN_RESET 11     /* second button: reset to first song       */
#define BTN_SAMPLE_MS  10    /* poll interval (ms)                       */
#define BTN_STABLE_CNT 5     /* samples a level must hold (~50 ms) before
                              * it is accepted, to reject contact bounce */

#define MAX_TRACKS 450

/* UI state machine (mirrors the reference Arduino sketch). */
typedef enum { MODE_BOOT_TITLE, MODE_TITLE_DONE, MODE_CLICK, MODE_NOUSB, MODE_SCANNING, MODE_PLAYER } ui_mode_t;

static const char TITLE[] = "SMARTNAVI VOICE\n   ANNOUNCER";
#define TITLE_CHAR_MS      250   /* ms between title characters         */
#define TITLE_HOLD_MS      3000  /* full title hold before waiting for USB */

/* Shared playlist state. The large arrays are guarded by s_mtx; the scalar
 * flags are volatile so cross-task visibility/ordering is not optimized away. */
static char           s_names[MAX_TRACKS][128];
static int            s_count = 0;
static int            s_sel = 0;         /* currently playing index; -1 = none yet */
static volatile bool  s_playing = false;
static volatile bool  s_mounted = false;
static volatile bool  s_scanning = false;    /* true while the drive is being scanned */
static SemaphoreHandle_t s_mtx = NULL;

/* State-machine state. s_btn_press/s_btn_reset_press are set by button_task,
 * consumed by ui_task. */
static ui_mode_t   s_mode = MODE_BOOT_TITLE;
typedef enum {
    BUTTON_PLAY,
    BUTTON_RESET,
} button_event_t;

static QueueHandle_t s_button_queue = NULL;
static int         s_title_chars = 0;
static uint32_t    s_title_last = 0;
static uint32_t    s_title_full_ms = 0;    /* when title became fully drawn */
static uint32_t    s_start_ms = 0;      /* monotonic ms when current file began  */
static uint32_t    s_dur_ms = 0;        /* real duration of current file (ms)    */
static bool        s_music_running = false;  /* true while inside the play window */

/* ------------------------------------------------------------------ */

/* Icons replicating the reference sketch (play triangle + music note). */
static void play_icon(int x, int y)
{
    oled_fill_triangle(x, y, x, y + 6, x + 5, y + 3);
}

static void music_icon(int x, int y)
{
    oled_draw_vline(x + 2, y, 5);
    oled_draw_hline(x + 2, y, 4);
    oled_draw_vline(x + 5, y, 5);
    oled_fill_circle(x + 1, y + 5, 1);
    oled_fill_circle(x + 4, y + 5, 1);
}

/* Truncate a long filename: first 15 chars + "..." (18-char budget). */
static void truncate_name(const char *in, char *out)
{
    snprintf(out, 16, "%.15s", in);
    if (strlen(in) > 15) {
        out[15] = '.'; out[16] = '.'; out[17] = '.'; out[18] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Per-mode screens.                                                   */
/* ------------------------------------------------------------------ */

static void render_boot_title(void)
{
    oled_clear();
    char buf[sizeof(TITLE)];
    strncpy(buf, TITLE, s_title_chars);
    buf[s_title_chars] = 0;
    oled_draw_text(22, 8, buf);
    update_oled();
}

static void render_click(void)
{
    oled_clear();
    oled_draw_roundrect(1, 1, 126, 30, 4);
    oled_draw_text(29, 7, "KLIK TOMBOL");
    oled_draw_text(28, 18, "UNTUK LANJUT");
    update_oled();
}

static void render_nousb(void)
{
    oled_clear();
    const char *m = "mencari usb";
    oled_draw_text((OLED_WIDTH - strlen(m) * 6) / 2, 12, m);
    update_oled();
}

static void render_unsupported(void)
{
    oled_clear();
    const char *l1 = "flashdisk harus";
    const char *l2 = "FAT32";
    oled_draw_text((OLED_WIDTH - strlen(l1) * 6) / 2, 7, l1);
    oled_draw_text((OLED_WIDTH - strlen(l2) * 6) / 2, 17, l2);
    update_oled();
}

/* Indeterminate loading bar while the drive is being scanned. It is pure
 * animation (we don't know the total file count up front), which keeps the
 * scan UI alive and hides the blocking directory read. */
static void render_scanning(void)
{
    oled_clear();
    const char *m = "mencari file";
    oled_draw_text((OLED_WIDTH - strlen(m) * 6) / 2, 4, m);

    const uint8_t bx = 4;
    const uint8_t bw = 120;
    const uint8_t by = 20;
    const uint8_t bh = 6;
    const uint8_t bar_w = 22;

    /* Frame. */
    oled_draw_hline(bx, by, bw);
    oled_draw_hline(bx, by + bh - 1, bw);
    oled_draw_vline(bx, by, bh);
    oled_draw_vline(bx + bw - 1, by, bh);

    /* Ping-pong fill so the bar keeps moving without jumping. */
    int range = (int)(bw - bar_w);
    int period = range > 0 ? range : 1;
    int phase = (int)((now_ms() / 6) % (period * 2));
    if (phase >= period) phase = period * 2 - phase;
    oled_fill_rect((uint8_t)(bx + phase), (uint8_t)(by + 1), bar_w, (uint8_t)(bh - 2));

    update_oled();
}

static void render_player(void)
{
    char names[3][19];
    int count;
    int sel;
    bool running;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    count = s_count;
    sel = s_sel;
    running = s_music_running;
    if (count > 0) {
        int cur = (sel >= 0 && sel < count) ? sel : 0;
        int prev = (cur == 0) ? count - 1 : cur - 1;
        int next = (cur + 1) % count;
        truncate_name(s_names[prev], names[0]);
        truncate_name(s_names[cur], names[1]);
        truncate_name(s_names[next], names[2]);
    }
    xSemaphoreGive(s_mtx);

    oled_clear();
    if (count == 0) {
        const char *m = "Tidak ada lagu";
        oled_draw_text((OLED_WIDTH - strlen(m) * 6) / 2, 12, m);
        update_oled();
        return;
    }

    if (running) play_icon(2, 2);
    else music_icon(2, 2);
    music_icon(2, 13);
    music_icon(2, 24);
    oled_draw_text(11, 1, names[0]);
    oled_draw_text(11, 12, names[1]);
    oled_draw_text(11, 23, names[2]);

    if (running && s_dur_ms > 0) {
        uint32_t elapsed = (now_ms() >= s_start_ms) ? now_ms() - s_start_ms : 0;
        if (elapsed > s_dur_ms) elapsed = s_dur_ms;
        uint32_t width = (uint32_t)((uint64_t)elapsed * OLED_WIDTH / s_dur_ms);
        if (width > 0) oled_invert_region(0, 11, (uint8_t)width, 10);
    }
    oled_draw_hline(0, 21, OLED_WIDTH);
    update_oled();
}

/* ------------------------------------------------------------------ */

static void start_play(int i)
{
    /* Diagnostic self-test path: ignore the playlist and emit a 1 kHz sine
     * straight to I2S. This isolates I2S/DAC/amp problems from USB + decoder
     * problems (see audio.h). */
#if AUDIO_TEST_TONE
    ESP_LOGI(TAG, "PLAY #%d: TEST TONE (1 kHz, no file)", i);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_sel = i;
    s_playing = true;
    xSemaphoreGive(s_mtx);
    audio_play_test_tone();
    s_dur_ms = 3000;
    s_start_ms = now_ms();
    s_music_running = true;
    return;
#endif

    /* Snapshot the count + chosen name under the mutex so a concurrent
     * scan/remount (which rewrites s_count and s_names) cannot race us. */
    char name[128];
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int cnt = s_count;
    if (cnt <= 0) {
        xSemaphoreGive(s_mtx);
        return;
    }
    i %= cnt; if (i < 0) i += cnt;
    strncpy(name, s_names[i], sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    s_sel = i;
    s_playing = true;
    xSemaphoreGive(s_mtx);

    char path[160];
    snprintf(path, sizeof(path), "%s/%s", USB_BASE_PATH, name);

    ESP_LOGI(TAG, "PLAY #%d: %s (path=%s)", i, name, path);
    audio_play(path);

    /* Real duration drives the progress bar (not a hardcoded value). */
    uint32_t d = 0;
    if (usb_get_duration(name, &d) != ESP_OK) d = 0;
    s_dur_ms = d;
    s_start_ms = now_ms();
    s_music_running = true;
    ESP_LOGI(TAG, "duration estimate: %lu ms", (unsigned long)d);
}

/* ------------------------------------------------------------------ */
/* Button task: poll + debounce. Both buttons are active-low to GND.    */
/* Only sets press flags; the UI state machine reacts to them.          */
/* ------------------------------------------------------------------ */
static void button_task(void *arg)
{
    int stable_play  = 1;                 /* debounced level; starts released */
    int cnt_play     = 0;
    int stable_reset = 1;
    int cnt_reset    = 0;
    ESP_LOGI(TAG, "btn: PLAY=GPIO%d  RESET=GPIO%d  pull-up, active-low",
             PIN_BTN_PLAY, PIN_BTN_RESET);
    for (;;) {
        /* --- PLAY button --- */
        int lvl_play = gpio_get_level(PIN_BTN_PLAY);
        if (lvl_play == stable_play) {
            cnt_play = 0;
        } else if (++cnt_play >= BTN_STABLE_CNT) {
            stable_play = lvl_play;
            cnt_play = 0;
            if (stable_play == 0) {
                button_event_t event = BUTTON_PLAY;
                if (xQueueSend(s_button_queue, &event, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "button queue full: PLAY");
                }
                ESP_LOGI(TAG, "btn: PLAY press");
            }
        }

        /* --- RESET button --- */
        int lvl_reset = gpio_get_level(PIN_BTN_RESET);
        if (lvl_reset == stable_reset) {
            cnt_reset = 0;
        } else if (++cnt_reset >= BTN_STABLE_CNT) {
            stable_reset = lvl_reset;
            cnt_reset = 0;
            if (stable_reset == 0) {
                button_event_t event = BUTTON_RESET;
                if (xQueueSend(s_button_queue, &event, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "button queue full: RESET");
                }
                ESP_LOGI(TAG, "btn: RESET press");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_SAMPLE_MS));
    }
}

/* ------------------------------------------------------------------ */
/* UI task: state machine driving every screen. The button only sets    */
/* s_btn_press; this task reacts to it depending on the current mode.  */
/* ------------------------------------------------------------------ */

static void enter_player(void)
{
    s_mode = MODE_PLAYER;
    s_sel = -1;                     /* first press = track 0 (sketch behavior) */
    s_music_running = false;
    s_dur_ms = 0;
}

static void update_progress(void)
{
    if (!s_music_running || s_dur_ms == 0) return;
    if (now_ms() - s_start_ms >= s_dur_ms) {
        char name[128] = {0};
        bool valid = false;
        s_music_running = false;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (s_sel >= 0 && s_sel < s_count) {
            strncpy(name, s_names[s_sel], sizeof(name) - 1);
            valid = true;
        }
        xSemaphoreGive(s_mtx);
        if (valid) ESP_LOGI(TAG, "SELESAI: %s", name);
    }
}

static void ui_task(void *arg)
{
    for (;;) {
        uint32_t now = now_ms();
        bool pressed = false;
        bool reset_pressed = false;
        button_event_t event;
        while (xQueueReceive(s_button_queue, &event, 0) == pdTRUE) {
            if (event == BUTTON_RESET) reset_pressed = true;
            if (event == BUTTON_PLAY) pressed = true;
        }

        switch (s_mode) {
        case MODE_BOOT_TITLE:
            if (s_title_chars < (int)strlen(TITLE)) {
                if (now - s_title_last >= TITLE_CHAR_MS) {
                    s_title_last = now;
                    s_title_chars++;
                }
            } else {
                /* Title is fully drawn; hold it like the reference sketch. */
                s_mode = MODE_TITLE_DONE;
                s_title_full_ms = now;
            }
            render_boot_title();
            break;

        case MODE_TITLE_DONE:
            /* Hold the fully-drawn title (reference sketch's MODE_TUNGGU_JUDUL),
             * then wait for USB before showing the "KLIK TOMBOL" screen. */
            if (now - s_title_full_ms >= TITLE_HOLD_MS) {
                switch (usb_get_state()) {
                case USB_READY:
                    s_mode = MODE_SCANNING;     /* drive present -> start scan */
                    break;
                case USB_UNSUPPORTED:
                    s_mode = MODE_NOUSB;        /* drive present, wrong FS */
                    break;
                case USB_NONE:
                default:
                    s_mode = MODE_NOUSB;
                    break;
                }
            }
            break;

        case MODE_CLICK:
            if (usb_get_state() != USB_READY) {
                s_mode = MODE_NOUSB;        /* drive vanished/unsupported */
                render_nousb();
            } else if (pressed) {
                enter_player();             /* render_player happens next loop */
            } else {
                render_click();
            }
            break;

        case MODE_NOUSB:
            if (usb_get_state() == USB_UNSUPPORTED) {
                render_unsupported();       /* NTFS/exFAT: tell user to use FAT32 */
            } else if (s_scanning || usb_get_state() == USB_READY) {
                s_mode = MODE_SCANNING;     /* drive now present -> scanning */
            } else {
                render_nousb();
            }
            break;

        case MODE_SCANNING:
            if (s_mounted) {
                s_mode = MODE_CLICK;        /* scan finished -> invite press */
            } else if (usb_get_state() != USB_READY) {
                s_mode = MODE_NOUSB;        /* drive vanished during scan */
            } else {
                render_scanning();          /* keep the loading bar moving */
            }
            break;

        case MODE_PLAYER:
            if (usb_get_state() != USB_READY) {   /* drive removed / unsupported */
                audio_stop();
                s_music_running = false;
                s_mode = MODE_NOUSB;
                break;
            }
            if (reset_pressed) start_play(0);     /* reset -> first track */
            else if (pressed) start_play(s_sel + 1);   /* next track, wraps */
            update_progress();
            render_player();
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

/* ------------------------------------------------------------------ */
/* USB task: watch for the drive being mounted / removed.              */
/* ------------------------------------------------------------------ */
static void usb_task(void *arg)
{
    bool waiting_logged = false;            /* log "menunggu usb" only once per idle */
    for (;;) {
        usb_state_t usb = usb_get_state();
        if (usb == USB_READY && !s_mounted && !s_scanning) {
            waiting_logged = false;
            /* static: 64 * 128 = 8 KB, far too large for a 4 KB task stack. */
            static char tmp[MAX_TRACKS][128];
            int cnt = 0;

            /* Enter scanning state; mounted stays false until the scan is done. */
            ESP_LOGI(TAG, "flash drive detected -> scanning (OLED: mencari)");
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_scanning = true;
            xSemaphoreGive(s_mtx);
            vTaskDelay(pdMS_TO_TICKS(300));   /* let the OLED show "mencari" */

            bool ok = usb_scan_playlist(tmp, MAX_TRACKS, &cnt) == ESP_OK;
            bool still_ready = usb_get_state() == USB_READY;
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            if (ok && still_ready) {
                memcpy(s_names, tmp, sizeof(tmp));
                s_count = cnt;
                s_mounted = true;
            } else {
                s_count = 0;
                s_mounted = false;
            }
            s_sel = -1;
            s_playing = false;
            s_scanning = false;
            xSemaphoreGive(s_mtx);
            ESP_LOGI(TAG, "drive scan %s, %d track(s) -> library",
                     (ok && still_ready) ? "complete" : "discarded", (ok && still_ready) ? cnt : 0);
        } else if (usb != USB_READY && (s_mounted || s_scanning)) {
            waiting_logged = false;
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_mounted = false;
            s_scanning = false;
            s_count = 0;
            s_sel = -1;
            s_playing = false;
            xSemaphoreGive(s_mtx);
            audio_stop();
            ESP_LOGI(TAG, "drive removed or unsupported (OLED: menunggu usb)");
        } else if (usb == USB_NONE && !s_mounted && !waiting_logged) {
            waiting_logged = true;
            ESP_LOGI(TAG, "no flash drive (OLED: menunggu usb)");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "bus mp3 player boot");

    s_mtx = xSemaphoreCreateMutex();
    assert(s_mtx);
    s_button_queue = xQueueCreate(16, sizeof(button_event_t));
    assert(s_button_queue);

    if (oled_init() != ESP_OK) {
        ESP_LOGW(TAG, "OLED init failed; continuing without display");
    }
    oled_clear();
    oled_draw_text((OLED_WIDTH - strlen("Boot...") * 6) / 2, 12, "Boot...");
    update_oled();
    ESP_ERROR_CHECK(audio_init());
    audio_create_task();

    esp_err_t usb_err = usb_init();
    if (usb_err != ESP_OK) {
        ESP_LOGW(TAG, "USB init failed: %s", esp_err_to_name(usb_err));
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BTN_PLAY) | (1ULL << PIN_BTN_RESET),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    BaseType_t task_err = xTaskCreate(button_task, "buttons", 4096, NULL, 5, NULL);
    assert(task_err == pdPASS);
    task_err = xTaskCreate(ui_task, "ui", 4096, NULL, 3, NULL);
    assert(task_err == pdPASS);
    task_err = xTaskCreate(usb_task, "usb", 4096, NULL, 3, NULL);
    assert(task_err == pdPASS);

    ESP_LOGI(TAG, "boot complete");
    /* app_main returns; the created tasks keep running. */
}
