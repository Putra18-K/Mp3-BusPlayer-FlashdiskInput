#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Mounted path of the USB flash drive (native USB-OTG host, FAT32). The
 * usb_host_msc driver mounts devices as /usb0, /usb1, ... (single device
 * here, so always /usb0). */
#define USB_BASE_PATH "/usb0"

/* Filesystem state reported to the UI. Only USB_READY means a supported
 * filesystem (FAT32) is mounted and files can be read. */
typedef enum {
    USB_NONE = 0,        /* no drive physically connected */
    USB_READY,           /* drive mounted, filesystem supported */
    USB_UNSUPPORTED,     /* drive present but filesystem is NTFS/exFAT/etc. */
} usb_state_t;

/* Install the USB host + MSC driver and start the mount/unmount task.
 * Call once from app_main. */
esp_err_t usb_init(void);

/* Current USB filesystem state (see usb_state_t). */
usb_state_t usb_get_state(void);

/* True while a flash drive is connected and the volume is mounted. */
bool usb_is_connected(void);

/* Fill up to `max` filenames from the drive root that end in .mp3 (only files
 * the MP3 decoder can play). Returns ESP_OK; *count is set. Names have no
 * leading slash. */
esp_err_t usb_scan_playlist(char (*names)[128], int max, int *count);

/* Real playback length of one track (name from usb_scan_playlist, no slash)
 * in milliseconds. WAV reads the header; MP3 scans the frames (minimp3 core).
 * Returns ESP_OK and sets *dur_ms; ESP_ERR_NOT_FOUND / INVALID_RESPONSE on
 * failure. */
esp_err_t usb_get_duration(const char *name, uint32_t *dur_ms);
