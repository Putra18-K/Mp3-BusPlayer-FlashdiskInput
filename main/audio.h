#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* ================================================================== */
/* Hardware / build configuration                                     */
/* ================================================================== */

/* I2S pins to the PCM5102A (custom wiring). Override in CMake if needed. */
#ifndef PIN_I2S_BCLK
#define PIN_I2S_BCLK 17
#endif
#ifndef PIN_I2S_WS
#define PIN_I2S_WS   15
#endif
#ifndef PIN_I2S_DOUT
#define PIN_I2S_DOUT 16
#endif
#ifndef PIN_I2S_MCLK
#define PIN_I2S_MCLK 18   /* drives PCM5102A SCK */
#endif

/* PCM5102A FMT pin selects the serial data format:
 *   FMT = LOW  -> I2S / Philips (bit delay = 1 BCLK)
 *   FMT = HIGH -> Left-justified / MSB (no bit delay)
 *
 * The project requirement is "I2S Philips standard", so the default is 0.
 * If your PCM5102A module has the format strap (often labelled H3/FMT) tied
 * HIGH, set this to 1 so the ESP32-S3 transmits left-justified/MSB data
 * matching what the DAC is decoding. A mismatch here produces exactly the
 * "digital noise / distorted garbage" symptom. */
#ifndef PCM5102_LEFT_JUSTIFIED
#define PCM5102_LEFT_JUSTIFIED 0
#endif

/* Diagnostic self-test: when enabled, every PLAY button press emits a 1 kHz
 * sine wave directly to I2S (no USB, no file, no decoder). Use this to split
 * an I2S/hardware problem from a USB/decoder problem. Set to 0 for normal
 * operation. */
#ifndef AUDIO_TEST_TONE
#define AUDIO_TEST_TONE 0
#endif

/* ================================================================== */

/* Configure the I2S peripheral (call once from app_main before starting task). */
esp_err_t audio_init(void);

/* Start the audio worker task (call once from app_main). */
void audio_create_task(void);

/* Request playback of a music file (MP3 or WAV) at an absolute path like
 * "/usb0/01-song.mp3". Non-blocking: returns immediately; playback happens
 * on the audio task. */
void audio_play(const char *path);

/* Stop current playback. Non-blocking. */
void audio_stop(void);

/* Stop playback and wait until the audio task has closed its current file. */
esp_err_t audio_stop_and_wait(uint32_t timeout_ms);

/* Emit a 1 kHz diagnostic sine for ~3 s (used when AUDIO_TEST_TONE is on). */
void audio_play_test_tone(void);

/* True while a song is actively being decoded/streamed. */
bool audio_is_playing(void);
