/* Audio playback: reads an MP3 file from the mounted USB volume, decodes it
 * with minimp3, and streams 16-bit stereo PCM to the PCM5102A over I2S.
 *
 * Runs on its own FreeRTOS task so the UI stays responsive.
 *
 * Concurrency model:
 *   - audio_task() is the ONLY place that calls i2s_channel_write() and the
 *     decoders. The button/UI path never touches the I2S channel directly.
 *   - audio_play()/audio_stop()/audio_play_test_tone() only set s_cancel and
 *     post a task notification. The notification value is the command.
 *   - eSetValueWithOverwrite is intentional: if several requests arrive while
 *     the task is busy, only the newest one matters (the path is read from
 *     s_play_path under the mutex when the task wakes up).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "minimp3.h"
#include "audio.h"

static const char *TAG = "audio";

/* Task notification commands. */
#define AUDIO_CMD_NONE       0
#define AUDIO_CMD_PLAY_FILE  1
#define AUDIO_CMD_TEST_TONE  2

static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_mtx = NULL;
static SemaphoreHandle_t s_idle = NULL;
static char s_play_path[160];
static volatile bool s_playing = false;
static volatile bool s_cancel = false;

static i2s_chan_handle_t s_tx = NULL;
static uint32_t s_cur_rate = 0;

/* Bounded I2S write logging: log an error immediately, and a progress line
 * only every I2S_LOG_INTERVAL frames (keeps the audio task from being
 * flooded with per-frame logs). */
static uint32_t s_write_seq = 0;
#define I2S_LOG_INTERVAL 100

/* Input buffer is large enough that a normal MP3 frame (<= ~1.5 KB) never
 * straddles the buffer edge except at EOF. */
#define IN_BUF_SIZE (16 * 1024)

/* Refill the input buffer before decoding once the remaining data drops below
 * this threshold. Keeps minimp3 from seeing a truncated frame at the boundary
 * of the fixed-size buffer. */
#define MP3_REFILL_THRESHOLD 2048

/* minimp3 PCM buffer: MINIMP3_MAX_SAMPLES_PER_FRAME already covers the
 * maximum interleaved samples of one MPEG frame (1152 samples/ch * 2 ch).
 * mp3d_sample_t is int16_t unless MINIMP3_FLOAT_OUTPUT is defined. */
#define PCM_BUF_SAMPLES MINIMP3_MAX_SAMPLES_PER_FRAME

/* Mono -> stereo conversion workspace (2x the maximum mono frame). */
#define STEREO_BUF_SAMPLES (MINIMP3_MAX_SAMPLES_PER_FRAME * 2)

#define SILENCE_MS 30

/* Test tone parameters. */
#define TONE_SAMPLE_RATE  44100u
#define TONE_HZ           1000u
#define TONE_SECONDS      3u
#define TONE_CHUNK_FRAMES 512u
/* ~15% of full scale to avoid clipping the PAM8403 input stage. */
#define TONE_AMPLITUDE ((int16_t)(32767.0f * 0.15f))
#define TONE_TWO_PI 6.28318530718f

/* ------------------------------------------------------------------ */

static esp_err_t set_sample_rate(uint32_t rate)
{
    if (rate == s_cur_rate) return ESP_OK;

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    /* The clock may only be reconfigured while the channel is stopped. */
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx), TAG, "disable");
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &clk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconfig clock to %lu Hz failed: %s",
                 (unsigned long)rate, esp_err_to_name(err));
        /* Try to leave the channel usable at the previous rate. */
        i2s_channel_enable(s_tx);
        return err;
    }
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable");
    s_cur_rate = rate;
    ESP_LOGI(TAG, "I2S sample rate set to %lu Hz", (unsigned long)rate);
    return ESP_OK;
}

/* Write decoded PCM to the DAC. `nsamples_per_ch` is samples per channel.
 * Mono input is duplicated into a stereo frame here so the I2S slot mode can
 * stay fixed at STEREO. Logging is rate-limited to avoid starving streaming. */
static void write_pcm(const int16_t *pcm, int nsamples_per_ch, int channels)
{
    static int16_t stereo[STEREO_BUF_SAMPLES];
    const int16_t *src = pcm;

    if (channels == 1) {
        if (nsamples_per_ch > (int)(STEREO_BUF_SAMPLES / 2)) {
            ESP_LOGE(TAG, "mono->stereo overflow: %d samples", nsamples_per_ch);
            return;
        }
        for (int i = 0; i < nsamples_per_ch; i++) {
            stereo[2 * i]     = pcm[i];
            stereo[2 * i + 1] = pcm[i];
        }
        src = stereo;
    } else if (channels != 2) {
        ESP_LOGE(TAG, "unsupported channel count: %d", channels);
        return;
    }

    size_t bytes = (size_t)nsamples_per_ch * 2 * sizeof(int16_t);
    size_t written = 0;
    esp_err_t result = i2s_channel_write(s_tx, src, bytes, &written, portMAX_DELAY);

    /* Rate-limited logging: always report errors; otherwise one progress line
     * every I2S_LOG_INTERVAL frames so the log does not stall the stream. */
    s_write_seq++;
    if (result != ESP_OK || written != bytes) {
        ESP_LOGE(TAG, "I2S error: result=%d requested=%u written=%u",
                 result, (unsigned)bytes, (unsigned)written);
    } else if ((s_write_seq % I2S_LOG_INTERVAL) == 0) {
        ESP_LOGI(TAG, "I2S OK: frames=%lu written=%u",
                 (unsigned long)s_write_seq, (unsigned)written);
    }
}

/* Send a short burst of silence so the DAC never holds the last sample or
 * floats the data line while idle/after a stop/cancel/failure. */
static void output_silence(int ms)
{
    if (!s_tx) return;

    uint32_t rate = s_cur_rate ? s_cur_rate : 44100;
    uint32_t frames = (uint32_t)(((uint64_t)rate * (uint32_t)ms) / 1000u);
    static int16_t zero[TONE_CHUNK_FRAMES * 2];   /* static -> zero-initialized */

    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > TONE_CHUNK_FRAMES) n = TONE_CHUNK_FRAMES;
        size_t written = 0;
        i2s_channel_write(s_tx, zero, (size_t)n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        done += n;
    }
}

/* ------------------------------------------------------------------ */
/* MP3 (minimp3)                                                       */
/* ------------------------------------------------------------------ */

static void play_mp3(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "MP3 fopen FAILED: %s (errno=%d)", path, errno);
        s_playing = false;
        return;
    }
    ESP_LOGI(TAG, "MP3 open OK: %s", path);

    /* static: mp3dec_t is ~6.5 KB and the decoder's internal scratch can
     * exceed 13 KB — far too large for a task stack, even 8 KB. */
    static uint8_t inbuf[IN_BUF_SIZE];
    static int16_t pcm[PCM_BUF_SAMPLES];   /* == MINIMP3_MAX_SAMPLES_PER_FRAME */
    static mp3dec_t dec;

    size_t inpos = 0, inlen = 0;
    uint64_t decoded_frames = 0;

    mp3dec_init(&dec);
    mp3dec_frame_info_t info;

    bool first = true;

    s_playing = true;
    s_write_seq = 0;

    for (;;) {
        if (s_cancel) break;

        /* Guard against a corrupted stream pushing inpos past inlen (which
         * would make `inlen - inpos` underflow on size_t). */
        if (inpos > inlen) {
            ESP_LOGE(TAG, "MP3 buffer state invalid: inpos=%u inlen=%u",
                     (unsigned)inpos, (unsigned)inlen);
            break;
        }

        /* Refill once the remaining data drops below the threshold so minimp3
         * never sees a truncated frame at the boundary of the fixed buffer. */
        size_t remaining = inlen - inpos;
        if (remaining < MP3_REFILL_THRESHOLD) {
            if (remaining > 0 && inpos > 0) {
                memmove(inbuf, inbuf + inpos, remaining);
            }
            inlen = remaining;
            inpos = 0;

            size_t free_space = IN_BUF_SIZE - inlen;
            if (free_space > 0) {
                size_t got = fread(inbuf + inlen, 1, free_space, f);
                inlen += got;
            }
        }

        if (inlen - inpos < 4) break;   /* EOF: nothing decodable left */

        memset(&info, 0, sizeof(info));
        int n = mp3dec_decode_frame(&dec, inbuf + inpos, (int)(inlen - inpos), pcm, &info);

        /* A corrupt file must never advance inpos beyond inlen: reject a
         * frame_bytes that is negative or larger than what is buffered. */
        if (info.frame_bytes < 0 ||
            (size_t)info.frame_bytes > (inlen - inpos)) {
            ESP_LOGE(TAG, "Invalid MP3 frame size: %d, available=%u",
                     info.frame_bytes, (unsigned)(inlen - inpos));
            break;
        }

        if (info.frame_bytes > 0) {
            inpos += (size_t)info.frame_bytes;
        }

        if (n > 0) {
            if (info.channels != 1 && info.channels != 2) {
                ESP_LOGE(TAG, "Invalid MP3 channels: %d", info.channels);
                break;
            }
            if (info.hz <= 0) {
                ESP_LOGE(TAG, "Invalid MP3 sample rate");
                break;
            }

            /* (Re)configure I2S on the first frame and whenever the stream's
             * sample rate changes (e.g. a file that changes rate mid-stream
             * or when a new stream starts). VBR changes bitrate, not sample
             * rate, so this normally only fires once per file. */
            if (first || s_cur_rate != (uint32_t)info.hz) {
                first = false;

                if (set_sample_rate((uint32_t)info.hz) != ESP_OK) {
                    ESP_LOGE(TAG, "Cannot set sample rate: %d", info.hz);
                    break;
                }
                ESP_LOGI(TAG, "MP3: rate=%d, channels=%d", info.hz, info.channels);
            }

            write_pcm(pcm, n, info.channels);
            decoded_frames++;
            continue;
        }

        if (info.frame_bytes == 0) {
            /* No frame decoded and no bytes skipped: incomplete/corrupt frame.
             * Skip one byte as a last-resort resync; clamp at buffer end so a
             * fully-undecodable tail cannot loop forever. */
            inpos++;
            if (inpos >= inlen) {
                inpos = inlen;
            }
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "MP3 done: %llu frame(s) decoded (path=%s)",
             (unsigned long long)decoded_frames, path);
    s_playing = false;
}

/* ------------------------------------------------------------------ */
/* WAV support: uncompressed PCM (RIFF/WAVE).                          */
/* ------------------------------------------------------------------ */

static bool is_wav(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".wav") == 0;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void play_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "WAV fopen FAILED: %s (errno=%d)", path, errno);
        s_playing = false;
        return;
    }
    ESP_LOGI(TAG, "WAV open OK: %s", path);

    uint8_t riff[12];
    if (fread(riff, 1, 12, f) != 12 ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        ESP_LOGW(TAG, "%s: not a RIFF/WAVE file", path);
        fclose(f);
        s_playing = false;
        return;
    }

    int channels = 0;
    uint32_t rate = 0;
    uint16_t bits = 0;
    uint32_t data_remaining = 0;
    uint8_t cid[4];
    uint32_t csize;

    /* Walk chunks to find "fmt " then "data". Stop once "data" is located. */
    bool have_fmt = false;
    bool have_data = false;
    while (fread(cid, 1, 4, f) == 4 && fread(&csize, 4, 1, f) == 1) {
        csize = le32((const uint8_t *)&csize);
        if (memcmp(cid, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            size_t rd = fread(fmt, 1, (csize < sizeof(fmt)) ? csize : sizeof(fmt), f);
            if (rd < 16 || le16(fmt) != 1) {        /* 1 = uncompressed PCM */
                ESP_LOGW(TAG, "%s: unsupported WAV format %u", path, rd >= 2 ? le16(fmt) : 0);
                fclose(f);
                s_playing = false;
                return;
            }
            channels = le16(fmt + 2);
            rate     = le32(fmt + 4);
            bits     = le16(fmt + 14);
            have_fmt = true;
            if (csize > sizeof(fmt)) fseek(f, csize - sizeof(fmt), SEEK_CUR);
        } else if (memcmp(cid, "data", 4) == 0) {
            data_remaining = csize;
            have_data = true;
            break;
        } else {
            fseek(f, csize, SEEK_CUR);
            if (csize & 1) fseek(f, 1, SEEK_CUR);   /* chunks are word-aligned */
        }
    }

    if (!have_fmt || !have_data || !rate || !channels || (bits != 8 && bits != 16)) {
        ESP_LOGW(TAG, "%s: unsupported WAV ch=%d rate=%lu bits=%u fmt=%d data=%d",
                 path, channels, (unsigned long)rate, bits, have_fmt, have_data);
        fclose(f);
        s_playing = false;
        return;
    }
    if (channels != 1 && channels != 2) {
        ESP_LOGW(TAG, "%s: unsupported channel count %d", path, channels);
        fclose(f);
        s_playing = false;
        return;
    }

    ESP_LOGI(TAG, "WAV stream: rate=%lu Hz, ch=%d, bits=%u, data=%lu bytes",
             (unsigned long)rate, channels, bits, (unsigned long)data_remaining);

    if (set_sample_rate(rate) != ESP_OK) {
        ESP_LOGE(TAG, "%s: cannot set sample rate %lu Hz", path, (unsigned long)rate);
        fclose(f);
        s_playing = false;
        return;
    }

    s_playing = true;
    s_write_seq = 0;

    #define WAV_CHUNK 4096
    static uint8_t  buf[WAV_CHUNK];
    static int16_t  conv[WAV_CHUNK];

    uint64_t total_bytes = 0;
    while (!s_cancel && data_remaining > 0) {
        size_t want = (data_remaining < sizeof(buf)) ? data_remaining : sizeof(buf);
        size_t got = fread(buf, 1, want, f);
        if (got == 0) break;
        data_remaining -= (uint32_t)got;
        total_bytes += got;

        if (bits == 16) {
            int samples = (int)(got / 2);            /* interleaved int16 per channel */
            write_pcm((const int16_t *)buf, samples / channels, channels);
        } else {
            /* 8-bit unsigned -> 16-bit signed, then mono is expanded inside
             * write_pcm() if needed. */
            for (size_t i = 0; i < got; i++) conv[i] = (int16_t)(((int)buf[i] - 128) << 8);
            write_pcm(conv, (int)got / channels, channels);
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "WAV done: %llu byte(s) streamed (path=%s)",
             (unsigned long long)total_bytes, path);
    s_playing = false;
}

/* ------------------------------------------------------------------ */
/* Test tone (diagnostic): 1 kHz sine straight to I2S, no USB/decoder. */
/* ------------------------------------------------------------------ */

static void test_tone_run(void)
{
    if (set_sample_rate(TONE_SAMPLE_RATE) != ESP_OK) {
        ESP_LOGE(TAG, "test tone: cannot set %lu Hz", (unsigned long)TONE_SAMPLE_RATE);
        return;
    }

    ESP_LOGI(TAG, "TEST TONE: 1000 Hz sine, 44100 Hz, 16-bit stereo, ~%u s",
             TONE_SECONDS);

    const uint32_t total_frames = TONE_SAMPLE_RATE * TONE_SECONDS;
    static int16_t tone[TONE_CHUNK_FRAMES * 2];

    float phase = 0.0f;
    const float step = TONE_TWO_PI * (float)TONE_HZ / (float)TONE_SAMPLE_RATE;

    s_playing = true;
    s_write_seq = 0;

    uint32_t done = 0;
    while (done < total_frames && !s_cancel) {
        uint32_t n = total_frames - done;
        if (n > TONE_CHUNK_FRAMES) n = TONE_CHUNK_FRAMES;
        for (uint32_t i = 0; i < n; i++) {
            int16_t v = (int16_t)(sinf(phase) * TONE_AMPLITUDE);
            tone[2 * i]     = v;
            tone[2 * i + 1] = v;
            phase += step;
            if (phase >= TONE_TWO_PI) phase -= TONE_TWO_PI;
        }
        write_pcm(tone, (int)n, 2);
        done += n;
    }

    s_playing = false;
}

/* Dispatch by file type: WAV streams directly, everything else goes to the
 * MP3 decoder (minimp3). */
static void play_file(const char *path)
{
    if (is_wav(path)) play_wav(path);
    else              play_mp3(path);
}

/* ------------------------------------------------------------------ */

static void audio_task(void *arg)
{
    for (;;) {
        uint32_t cmd = 0;
        xTaskNotifyWait(0, ULONG_MAX, &cmd, portMAX_DELAY);

        if (cmd == AUDIO_CMD_PLAY_FILE) {
            s_cancel = false;
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            char path[160];
            strncpy(path, s_play_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
            xSemaphoreGive(s_mtx);
            play_file(path);
        } else if (cmd == AUDIO_CMD_TEST_TONE) {
            test_tone_run();
        }
        /* AUDIO_CMD_NONE (stop): playback already aborted via s_cancel. */

        output_silence(SILENCE_MS);
        xSemaphoreGive(s_idle);
    }
}

/* ------------------------------------------------------------------ */

esp_err_t audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 240;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s new channel");

    /* PCM5102A FMT selects Philips vs left-justified. Honour the hardware
     * strap via PCM5102_LEFT_JUSTIFIED (see audio.h). */
    i2s_std_slot_config_t slot =
#if PCM5102_LEFT_JUSTIFIED
        I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
#else
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
#endif

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = 44100,   /* default; set_sample_rate() follows the file */
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = slot,
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s init std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");
    s_cur_rate = 44100;

    /* With auto_clear_after_cb = false (IDF default) the freshly-allocated DMA
     * buffers are not guaranteed zeroed. Flush them with silence now so the
     * DAC does not clock out uninitialized samples (a source of power-on
     * digital noise) before the first PLAY press. */
    output_silence(SILENCE_MS);

    s_mtx = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mtx, ESP_ERR_NO_MEM, TAG, "mutex");
    s_idle = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_idle, ESP_ERR_NO_MEM, TAG, "idle semaphore");

    ESP_LOGI(TAG, "I2S config: rate=44100 Hz, bits=16, mode=%s, MCLK=GPIO%d, BCLK=GPIO%d, WS=GPIO%d, DOUT=GPIO%d, DIN=unused",
#if PCM5102_LEFT_JUSTIFIED
             "left-justified (MSB)",
#else
             "Philips (I2S)",
#endif
             PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT);
    ESP_LOGI(TAG, "I2S DMA: descriptors=%u, frames=%u, channel enabled=yes",
             chan_cfg.dma_desc_num, chan_cfg.dma_frame_num);

    return ESP_OK;
}

void audio_create_task(void)
{
    BaseType_t err = xTaskCreate(audio_task, "audio", 32768, NULL, 5, &s_task);
    if (err != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "audio task creation failed");
    }
}

void audio_play(const char *path)
{
    if (!s_task) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strncpy(s_play_path, path, sizeof(s_play_path) - 1);
    s_play_path[sizeof(s_play_path) - 1] = 0;
    s_cancel = true;                    /* interrupt whatever is playing */
    xSemaphoreGive(s_mtx);
    xTaskNotify(s_task, AUDIO_CMD_PLAY_FILE, eSetValueWithOverwrite);
}

void audio_play_test_tone(void)
{
    if (!s_task) return;
    s_cancel = true;                    /* stop any current file playback */
    xTaskNotify(s_task, AUDIO_CMD_TEST_TONE, eSetValueWithOverwrite);
}

void audio_stop(void)
{
    if (!s_task) return;
    s_cancel = true;
    xTaskNotify(s_task, AUDIO_CMD_NONE, eSetValueWithOverwrite);
}

esp_err_t audio_stop_and_wait(uint32_t timeout_ms)
{
    if (!s_task || !s_idle) return ESP_ERR_INVALID_STATE;

    while (xSemaphoreTake(s_idle, 0) == pdTRUE) {
    }
    audio_stop();
    if (xSemaphoreTake(s_idle, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "audio stop timed out after %lu ms", (unsigned long)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool audio_is_playing(void)
{
    return s_playing;
}
