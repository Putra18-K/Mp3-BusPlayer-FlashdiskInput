/* USB flash drive access via the native USB-OTG host on the ESP32-S3, using
 * the official `usb_host_msc` component (ESP-IDF 5.x/6.x). The volume is
 * mounted as FatFS at /usb0 and read with standard POSIX calls.
 *
 * The usb_host_msc driver is event-driven: a callback reports device
 * connect/disconnect, and a small task performs the actual mount/unmount. The
 * old `esp_msc_host` (esp-iot-solution) polling API no longer exists on the
 * registry, so `usb_is_connected()` here just reflects the state the mount
 * task maintains. */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#include "usb/msc_host_vfs.h"
#include "ffconf.h"
#include "usb.h"

static const char *TAG = "usb";

/* ------------------------------------------------------------------ */
/* Shared state (single flash drive). s_state mirrors the filesystem status
 * so the UI can distinguish "no drive" from "drive present but unsupported". */
static volatile usb_state_t s_state = USB_NONE;
static msc_host_device_handle_t s_device = NULL;
static msc_host_vfs_handle_t s_vfs = NULL;
/* Stale handle kept when a mount fails (e.g. unsupported filesystem) so the
 * later EV_DISCONNECTED for the same device is recognized and s_state can be
 * reset to USB_NONE. Without it the UI would stay on "flashdisk harus FAT32"
 * forever and the next drive could not mount. */
static msc_host_device_handle_t s_stale_device = NULL;

/* Deferred event message from the MSC callback to the mount task. */
typedef struct {
    enum { EV_CONNECTED, EV_DISCONNECTED } id;
    union {
        uint8_t addr;                  /* EV_CONNECTED: USB device address */
        msc_host_device_handle_t dev;  /* EV_DISCONNECTED: device handle   */
    } data;
} msc_msg_t;

static QueueHandle_t s_queue;

/* ------------------------------------------------------------------ */
/* MSC driver callback: the driver calls this from its background task.
 * We only post a message here; the actual mount/unmount happens in
 * usb_mount_task() so no blocking VFS work runs inside the driver.    */
/* ------------------------------------------------------------------ */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    msc_msg_t msg;
    if (event->event == MSC_DEVICE_CONNECTED) {
        ESP_LOGI(TAG, "MSC event: DEVICE CONNECTED (addr=%u)", event->device.address);
        msg.id = EV_CONNECTED;
        msg.data.addr = event->device.address;
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        ESP_LOGI(TAG, "MSC event: DEVICE DISCONNECTED");
        msg.id = EV_DISCONNECTED;
        msg.data.dev = event->device.handle;
    } else {
        return;                         /* suspend/resume, ignore */
    }
    xQueueSend(s_queue, &msg, 0);
}

/* ------------------------------------------------------------------ */
/* Mount / unmount the drive, keeping the shared state in sync.       */
/* ------------------------------------------------------------------ */
static void usb_mount_task(void *arg)
{
    for (;;) {
        msc_msg_t msg;
        if (!xQueueReceive(s_queue, &msg, portMAX_DELAY)) continue;

        if (msg.id == EV_CONNECTED) {
            ESP_LOGI(TAG, "installing device addr=%u...", msg.data.addr);
            esp_err_t err = msc_host_install_device(msg.data.addr, &s_device);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "msc_host_install_device FAILED: %s", esp_err_to_name(err));
                continue;
            }
            const esp_vfs_fat_mount_config_t mnt = {
                .format_if_mount_failed = false,
                .max_files = 3,
                .allocation_unit_size = 8192,
            };
            err = msc_host_vfs_register(s_device, USB_BASE_PATH, &mnt, &s_vfs);
            if (err != ESP_OK) {
                /* FatFS only supports FAT12/16/32 (and exFAT if patched).
                 * NTFS/exFAT/raw volumes fail here — report it instead of
                 * silently pretending there is no drive. */
                ESP_LOGW(TAG, "msc_host_vfs_register FAILED: %s (unsupported filesystem?)",
                         esp_err_to_name(err));
                msc_host_uninstall_device(s_device);
                /* Keep the stale handle so the disconnect event for this device
                 * is still recognized (see s_stale_device). */
                s_stale_device = s_device;
                s_device = NULL;
                s_state = USB_UNSUPPORTED;
                continue;
            }
            s_state = USB_READY;
            ESP_LOGI(TAG, "flash drive mounted at %s", USB_BASE_PATH);
        } else {
            /* EV_DISCONNECTED: ignore a stale event for an unknown handle,
             * but still recognize a handle we failed to mount earlier so the
             * unsupported state can be cleared when that drive is removed. */
            if (msg.data.dev != s_device && msg.data.dev != s_stale_device) continue;
            if (msg.data.dev == s_stale_device) s_stale_device = NULL;
            if (s_vfs) {
                msc_host_vfs_unregister(s_vfs);
                s_vfs = NULL;
            }
            if (s_device) {
                msc_host_uninstall_device(s_device);
                s_device = NULL;
            }
            s_state = USB_NONE;
            ESP_LOGI(TAG, "flash drive removed");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pump the USB host library event loop so devices can be freed on
 * removal (allows re-plugging a drive). Runs forever.               */
/* ------------------------------------------------------------------ */
static void usb_host_events_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        if (usb_host_lib_handle_events(portMAX_DELAY, &flags) != ESP_OK) continue;
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();     /* best-effort, for replug support */
        }
    }
}

/* ------------------------------------------------------------------ */

esp_err_t usb_init(void)
{
    s_queue = xQueueCreate(4, sizeof(msc_msg_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    const usb_host_config_t host_cfg = { .intr_flags = ESP_INTR_FLAG_LOWMED };
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "usb_host_install: %s", esp_err_to_name(err));
        return err;
    }

    const msc_host_driver_config_t msc_cfg = {
        .create_backround_task = true,   /* "backround" is the driver's spelling */
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    err = msc_host_install(&msc_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "msc_host_install: %s", esp_err_to_name(err));
        return err;
    }

    xTaskCreate(usb_host_events_task, "usb-host", 4096, NULL, 4, NULL);
    xTaskCreate(usb_mount_task, "usb-mount", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "usb host ready");
    return ESP_OK;
}

usb_state_t usb_get_state(void)
{
    return s_state;
}

bool usb_is_connected(void)
{
    return s_state == USB_READY;
}

/* Natural, case-insensitive name ordering so the playlist follows the order of
 * the files as stored on the drive (numeric runs by value, not byte order):
 *   "001.mp3" < "002.mp3" < "010.mp3" < "020.mp3"
 *   "1.mp3"   < "1a.mp3"  < "1b.mp3"
 * This is what makes index 0 the real "first" file, so a RESET to start_play(0)
 * lands on 001 instead of whatever entry readdir() happened to return first. */
static int playlist_name_cmp(const void *pa, const void *pb)
{
    const char *a = (const char *)pa;
    const char *b = (const char *)pb;
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            while (*a == '0') a++;          /* skip leading zeros */
            while (*b == '0') b++;
            const char *da = a, *db = b;
            while (*da >= '0' && *da <= '9') da++;
            while (*db >= '0' && *db <= '9') db++;
            size_t la = da - a, lb = db - b;
            if (la != lb) return (la < lb) ? -1 : 1;   /* fewer digits => smaller */
            int c = strncmp(a, b, la);
            if (c) return (c < 0) ? -1 : 1;
            a = da; b = db;                 /* equal numeric value, keep going */
        } else {
            int ca = tolower((unsigned char)*a);
            int cb = tolower((unsigned char)*b);
            if (ca != cb) return (ca < cb) ? -1 : 1;
            a++; b++;
        }
    }
    return (*a) ? 1 : ((*b) ? -1 : 0);
}

/* True if the filename ends in an extension we can actually play (.mp3 / .wav). */
static bool is_music_file(const char *n)
{
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    if (strcasecmp(dot, ".mp3") == 0) return true;
    if (strcasecmp(dot, ".wav") == 0) return true;
    return false;
}

esp_err_t usb_scan_playlist(char (*names)[128], int max, int *count)
{
    *count = 0;
    DIR *d = opendir(USB_BASE_PATH);
    if (!d) {
        ESP_LOGW(TAG, "opendir %s FAILED (errno=%d)", USB_BASE_PATH, errno);
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *e;
    while (*count < max && (e = readdir(d))) {
        const char *n = e->d_name;
        if (!is_music_file(n)) continue;        /* only .mp3/.wav; dirs have no such ext */
        strncpy(names[*count], n, 127);
        names[*count][127] = 0;
        ESP_LOGI(TAG, "  found: %s", n);
        (*count)++;
    }
    closedir(d);
    if (*count > 1) {
        qsort(names, *count, sizeof(names[0]), playlist_name_cmp);
    }
    ESP_LOGI(TAG, "scan done: %d music file(s) on %s", *count, USB_BASE_PATH);
    return ESP_OK;
}

static uint16_t le16u(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32u(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                                  ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Fast MP3 duration estimate from the first frame header — no minimp3 decoder,
 * no full-file decode (which would freeze the UI for minutes and overflow a
 * small task stack). bitrate_kbps + sample rate are enough to estimate length
 * from the file size. Slightly off for VBR files, but only the progress bar
 * uses it. */
static bool mp3_parse_header(const uint8_t *h, uint32_t *bitrate_kbps, uint32_t *rate_hz)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;   /* 11-bit sync */

    uint8_t ver   = (h[1] >> 3) & 0x3;   /* 3=MPEG1, 2=MPEG2, 0=MPEG2.5, 1=reserved */
    uint8_t layer = (h[1] >> 1) & 0x3;   /* 3=Layer I, 2=Layer II, 1=Layer III, 0=reserved */
    uint8_t br    = (h[2] >> 4) & 0xF;   /* bitrate index */
    uint8_t sr    = (h[2] >> 2) & 0x3;   /* sample-rate index */
    if (br == 0 || br == 15 || sr == 3 || layer == 0 || ver == 1) return false;

    static const uint16_t br_m1_l1[16] = {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0};
    static const uint16_t br_m1_l2[16] = {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0};
    static const uint16_t br_m1_l3[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const uint16_t br_m2_l1[16] = {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0};
    static const uint16_t br_m2_l2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};

    if (ver == 3) {                          /* MPEG1 */
        if (layer == 3) *bitrate_kbps = br_m1_l1[br];
        else if (layer == 2) *bitrate_kbps = br_m1_l2[br];
        else *bitrate_kbps = br_m1_l3[br];
        *rate_hz = (sr == 0) ? 44100 : (sr == 1) ? 48000 : 32000;
    } else {                                 /* MPEG2 / 2.5 */
        if (layer == 3) *bitrate_kbps = br_m2_l1[br];
        else *bitrate_kbps = br_m2_l2[br];
        if (ver == 2) *rate_hz = (sr == 0) ? 22050 : (sr == 1) ? 24000 : 16000;
        else          *rate_hz = (sr == 0) ? 11025 : (sr == 1) ? 12000 : 8000;
    }
    return *bitrate_kbps > 0 && *rate_hz > 0;
}

/* Real playback length of one track, in ms.
 * - WAV: bytes in the "data" chunk / byte rate from the "fmt " header.
 * - MP3: fast estimate from file size + first-frame header (see above). */
esp_err_t usb_get_duration(const char *name, uint32_t *dur_ms)
{
    *dur_ms = 0;
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", USB_BASE_PATH, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "duration: cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    const char *dot = strrchr(name, '.');
    if (dot && strcasecmp(dot, ".wav") == 0) {
        uint8_t riff[12];
        if (fread(riff, 1, 12, f) != 12 ||
            memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
            fclose(f);
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint16_t ch = 0, bits = 0;
        uint32_t rate = 0, data = 0;
        uint8_t cid[4];
        uint32_t csz;
        while (fread(cid, 1, 4, f) == 4 && fread(&csz, 4, 1, f) == 1) {
            csz = le32u((const uint8_t *)&csz);
            if (memcmp(cid, "fmt ", 4) == 0) {
                uint8_t fmt[16];
                size_t rd = fread(fmt, 1, (csz < 16) ? csz : 16, f);
                if (rd >= 16 && le16u(fmt) == 1) {   /* 1 = PCM */
                    ch = le16u(fmt + 2); rate = le32u(fmt + 4); bits = le16u(fmt + 14);
                }
                if (csz > 16) fseek(f, csz - 16, SEEK_CUR);
            } else if (memcmp(cid, "data", 4) == 0) {
                data = csz;
                break;
            } else {
                fseek(f, csz, SEEK_CUR);
                if (csz & 1) fseek(f, 1, SEEK_CUR);   /* chunks are word-aligned */
            }
        }
        fclose(f);
        if (!ch || !rate || (bits != 8 && bits != 16)) return ESP_ERR_INVALID_RESPONSE;
        uint64_t bps = (uint64_t)ch * (bits / 8) * rate;
        *dur_ms = bps ? (uint32_t)(((uint64_t)data * 1000) / bps) : 0;
        return ESP_OK;
    }

    /* MP3: find the first valid frame header, then estimate from file size. */
    static uint8_t probe[64 * 1024];
    size_t got = fread(probe, 1, sizeof(probe), f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (got < 4 || size <= 0) return ESP_ERR_INVALID_RESPONSE;

    for (size_t i = 0; i + 4 <= got; i++) {
        uint32_t br = 0, rate = 0;
        if (!mp3_parse_header(&probe[i], &br, &rate)) continue;
        /* bytes * 8 bits/byte / (kbps * 1000) = seconds; * 1000 -> ms. */
        uint64_t ms = ((uint64_t)size * 8) / br;
        *dur_ms = (ms > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)ms;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_RESPONSE;
}
