#include "voice/voice_channel.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <ctype.h>

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"

#include "cJSON.h"
#include "mbedtls/base64.h"

static const char *TAG = "voice";

/*
 * I2S timing / slot style selection:
 *   0: Philips (I2S, 1-bit delay after WS edge)
 *   1: MSB (left-justified, no 1-bit delay)
 *   2: PCM (short frame sync, ws_width=1, ws_pol=true)
 *
 * Many DAC/codec parts are sensitive to this. If your audio sounds like loud
 * "沙沙" noise but speech is partially recognizable, this is a prime suspect.
 */
#ifndef MIMI_VOICE_I2S_STD_SLOT_STYLE
#define MIMI_VOICE_I2S_STD_SLOT_STYLE 0
#endif

#ifndef MIMI_VOICE_I2S_DMA_DESC_NUM
#define MIMI_VOICE_I2S_DMA_DESC_NUM 6
#endif

#ifndef MIMI_VOICE_I2S_DMA_FRAME_NUM
#define MIMI_VOICE_I2S_DMA_FRAME_NUM 240
#endif

#ifndef MIMI_VOICE_TX_SILENCE_TAIL_MS
#define MIMI_VOICE_TX_SILENCE_TAIL_MS 400
#endif

#define MIMI_VOICE_TX_BYTES_PER_FRAME (2U * sizeof(int32_t))
#define MIMI_VOICE_TX_DMA_TOTAL_BYTES \
    ((uint32_t)MIMI_VOICE_I2S_DMA_DESC_NUM * (uint32_t)MIMI_VOICE_I2S_DMA_FRAME_NUM * (uint32_t)MIMI_VOICE_TX_BYTES_PER_FRAME)

#if MIMI_VOICE_I2S_STD_SLOT_STYLE == 1
#define MIMI_VOICE_I2S_SLOT_DEFAULT_CONFIG(bits, mono_or_stereo) \
    I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits, mono_or_stereo)
#elif MIMI_VOICE_I2S_STD_SLOT_STYLE == 2
#define MIMI_VOICE_I2S_SLOT_DEFAULT_CONFIG(bits, mono_or_stereo) \
    I2S_STD_PCM_SLOT_DEFAULT_CONFIG(bits, mono_or_stereo)
#else
#define MIMI_VOICE_I2S_SLOT_DEFAULT_CONFIG(bits, mono_or_stereo) \
    I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mono_or_stereo)
#endif

static const char *i2s_slot_style_str(void)
{
#if MIMI_VOICE_I2S_STD_SLOT_STYLE == 1
    return "MSB";
#elif MIMI_VOICE_I2S_STD_SLOT_STYLE == 2
    return "PCM";
#else
    return "PHILIPS";
#endif
}

/* =========================
 * Fallback config defaults
 * ========================= */

#ifndef MIMI_VOICE_ENABLED_DEFAULT
#define MIMI_VOICE_ENABLED_DEFAULT        0
#endif

#ifndef MIMI_VOICE_CHAT_ID
#define MIMI_VOICE_CHAT_ID                "voice_local"
#endif

#ifndef MIMI_VOICE_SAMPLE_RATE
#define MIMI_VOICE_SAMPLE_RATE            16000
#endif

#ifndef MIMI_VOICE_FRAME_MS
#define MIMI_VOICE_FRAME_MS               20
#endif

#ifndef MIMI_VOICE_MAX_UTTERANCE_MS
#define MIMI_VOICE_MAX_UTTERANCE_MS       10000
#endif

#ifndef MIMI_VOICE_SILENCE_END_MS
#define MIMI_VOICE_SILENCE_END_MS         600
#endif

#ifndef MIMI_VOICE_VAD_THRESHOLD
#define MIMI_VOICE_VAD_THRESHOLD          700
#endif

#ifndef MIMI_VOICE_CAPTURE_STACK
#define MIMI_VOICE_CAPTURE_STACK          (8 * 1024)
#endif

#ifndef MIMI_VOICE_TASK_PRIO
#define MIMI_VOICE_TASK_PRIO              5
#endif

#ifndef MIMI_VOICE_CORE
#define MIMI_VOICE_CORE                   0
#endif

#ifndef MIMI_SECRET_STT_URL
#define MIMI_SECRET_STT_URL               ""
#endif

#ifndef MIMI_SECRET_STT_API_KEY
#define MIMI_SECRET_STT_API_KEY           ""
#endif

#ifndef MIMI_SECRET_STT_MODEL
#define MIMI_SECRET_STT_MODEL             "qwen3-asr-flash"
#endif

#ifndef MIMI_SECRET_TTS_URL
#define MIMI_SECRET_TTS_URL               ""
#endif

#ifndef MIMI_SECRET_TTS_API_KEY
#define MIMI_SECRET_TTS_API_KEY           ""
#endif

#ifndef MIMI_SECRET_TTS_MODEL
#define MIMI_SECRET_TTS_MODEL             "qwen3-tts-flash"
#endif

#ifndef MIMI_SECRET_TTS_VOICE
#define MIMI_SECRET_TTS_VOICE             "Cherry"
#endif

#ifndef MIMI_SECRET_TTS_LANGUAGE
#define MIMI_SECRET_TTS_LANGUAGE          "English"
#endif

#ifndef MIMI_SECRET_API_KEY
#define MIMI_SECRET_API_KEY               ""
#endif

/* TTS text constraints (can override in mimi_secrets.h) */
#ifndef MIMI_VOICE_TTS_MAX_CHARS
#define MIMI_VOICE_TTS_MAX_CHARS          140
#endif

#ifndef MIMI_VOICE_I2S_PORT
#define MIMI_VOICE_I2S_PORT               0
#endif

#ifndef MIMI_VOICE_I2S_BCLK
#define MIMI_VOICE_I2S_BCLK               42
#endif

#ifndef MIMI_VOICE_I2S_WS
#define MIMI_VOICE_I2S_WS                 41
#endif

#ifndef MIMI_VOICE_I2S_DIN
#define MIMI_VOICE_I2S_DIN                40
#endif

#ifndef MIMI_VOICE_I2S_DOUT
#define MIMI_VOICE_I2S_DOUT               39
#endif

/* XVF3800 fixed digital format in your current design:
 * 16 kHz, stereo, 32-bit samples over I2S.
 */
#define VOICE_I2S_CHANNELS                 2
#define VOICE_I2S_BYTES_PER_SAMPLE         4
#define VOICE_I2S_BYTES_PER_STEREO_FRAME   (VOICE_I2S_CHANNELS * VOICE_I2S_BYTES_PER_SAMPLE)
#define VOICE_PCM_BITS                     16

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

typedef struct {
    uint16_t audio_format;   /* 1 = PCM */
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
} wav_fmt_t;

static bool s_enabled = false;
static bool s_i2s_ready = false;
static volatile bool s_is_playing = false;

static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static TaskHandle_t s_capture_task = NULL;
static SemaphoreHandle_t s_http_lock = NULL;

/* =========================
 * Secrets / config helpers
 * ========================= */

static const char *stt_api_url(void)
{
    return (MIMI_SECRET_STT_URL[0] != '\0') ? MIMI_SECRET_STT_URL : "";
}

static const char *stt_api_key(void)
{
    return (MIMI_SECRET_STT_API_KEY[0] != '\0') ? MIMI_SECRET_STT_API_KEY :
           (MIMI_SECRET_API_KEY[0] != '\0') ? MIMI_SECRET_API_KEY : "";
}

static const char *stt_model(void)
{
    return (MIMI_SECRET_STT_MODEL[0] != '\0') ? MIMI_SECRET_STT_MODEL : "qwen3-asr-flash";
}

static const char *tts_api_url(void)
{
    return (MIMI_SECRET_TTS_URL[0] != '\0') ? MIMI_SECRET_TTS_URL : "";
}

static const char *tts_api_key(void)
{
    return (MIMI_SECRET_TTS_API_KEY[0] != '\0') ? MIMI_SECRET_TTS_API_KEY :
           (MIMI_SECRET_API_KEY[0] != '\0') ? MIMI_SECRET_API_KEY : "";
}

static const char *tts_model(void)
{
    return (MIMI_SECRET_TTS_MODEL[0] != '\0') ? MIMI_SECRET_TTS_MODEL : "qwen3-tts-flash";
}

static const char *tts_voice(void)
{
    return (MIMI_SECRET_TTS_VOICE[0] != '\0') ? MIMI_SECRET_TTS_VOICE : "Cherry";
}

static const char *tts_language(void)
{
    return (MIMI_SECRET_TTS_LANGUAGE[0] != '\0') ? MIMI_SECRET_TTS_LANGUAGE : "English";
}

/* =========================
 * HTTP helpers
 * ========================= */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !resp || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    size_t need = resp->len + (size_t)evt->data_len + 1;
    if (need > resp->cap) {
        size_t new_cap = resp->cap ? resp->cap * 2 : 1024;
        while (new_cap < need) {
            new_cap *= 2;
        }
        char *tmp = realloc(resp->buf, new_cap);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        resp->buf = tmp;
        resp->cap = new_cap;
    }

    memcpy(resp->buf + resp->len, evt->data, evt->data_len);
    resp->len += (size_t)evt->data_len;
    resp->buf[resp->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_post_json(const char *url,
                                const char *bearer_key,
                                const char *json_body,
                                bool enable_sse,
                                http_resp_t *resp,
                                int *http_status_out)
{
    if (!url || !url[0] || !json_body || !resp) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(resp, 0, sizeof(*resp));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (bearer_key && bearer_key[0]) {
        char auth[320];
        snprintf(auth, sizeof(auth), "Bearer %s", bearer_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_header(client, "X-DashScope-SSE", enable_sse ? "enable" : "disable");
    esp_http_client_set_post_field(client, json_body, (int)strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && http_status_out) {
        *http_status_out = esp_http_client_get_status_code(client);
    }

    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t http_get_binary(const char *url, http_resp_t *resp, int *http_status_out)
{
    if (!url || !url[0] || !resp) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(resp, 0, sizeof(*resp));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .user_data = resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && http_status_out) {
        *http_status_out = esp_http_client_get_status_code(client);
    }

    esp_http_client_cleanup(client);
    return err;
}

/* =========================
 * Audio helpers
 * ========================= */

static void *malloc_prefer_spiram(size_t bytes)
{
    if (bytes == 0) {
        return NULL;
    }

    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (p) {
        return p;
    }
    return malloc(bytes);
}

static bool utf8_is_continuation_byte(uint8_t b)
{
    return (b & 0xC0U) == 0x80U;
}

static bool utf8_starts_with(const char *s, size_t i, size_t len, const char *lit)
{
    size_t lit_len = strlen(lit);
    if (i + lit_len > len) {
        return false;
    }
    return memcmp(s + i, lit, lit_len) == 0;
}

static bool is_speech_cut_punct(const char *s, size_t i, size_t len)
{
    const uint8_t b = (uint8_t)s[i];
    if (b == '\n' || b == '\r') {
        return true;
    }
    if (b == '.' || b == '!' || b == '?' || b == ',' || b == ';' || b == ':') {
        return true;
    }

    /* Common CJK punctuation in UTF-8 */
    if (utf8_starts_with(s, i, len, "。") ||
        utf8_starts_with(s, i, len, "！") ||
        utf8_starts_with(s, i, len, "？") ||
        utf8_starts_with(s, i, len, "，") ||
        utf8_starts_with(s, i, len, "；") ||
        utf8_starts_with(s, i, len, "：") ||
        utf8_starts_with(s, i, len, "、")) {
        return true;
    }

    return false;
}

static size_t utf8_truncate_for_tts(const char *text, size_t max_chars, size_t *out_char_count, bool *out_truncated)
{
    if (!text || max_chars == 0) {
        if (out_char_count) *out_char_count = 0;
        if (out_truncated) *out_truncated = false;
        return 0;
    }

    const size_t len = strlen(text);
    size_t char_count = 0;
    size_t last_punct_cut = 0;
    size_t i = 0;

    while (i < len) {
        if (char_count >= max_chars) {
            break;
        }

        if (!utf8_is_continuation_byte((uint8_t)text[i])) {
            char_count++;
            if (is_speech_cut_punct(text, i, len)) {
                /* Cut after this codepoint (best-effort) */
                size_t j = i + 1;
                while (j < len && utf8_is_continuation_byte((uint8_t)text[j])) {
                    j++;
                }
                last_punct_cut = j;
            }
        }
        i++;
    }

    if (out_char_count) {
        *out_char_count = char_count;
    }

    if (i >= len) {
        if (out_truncated) *out_truncated = false;
        return len;
    }

    /* Prefer cutting at punctuation, but avoid cutting too early */
    size_t cut = i;
    if (last_punct_cut > 0) {
        const size_t min_reasonable = (max_chars >= 20) ? (max_chars / 2) : 0;
        if (last_punct_cut >= min_reasonable) {
            cut = last_punct_cut;
        }
    }

    while (cut > 0 && utf8_is_continuation_byte((uint8_t)text[cut])) {
        cut--;
    }

    if (out_truncated) *out_truncated = true;
    return cut;
}

static char *voice_build_tts_text(const char *text)
{
    if (!text) {
        return NULL;
    }

    size_t char_count = 0;
    bool truncated = false;
    size_t cut_bytes = utf8_truncate_for_tts(text, MIMI_VOICE_TTS_MAX_CHARS, &char_count, &truncated);

    if (!truncated) {
        return NULL; /* caller can use original text */
    }

    char *out = (char *)malloc(cut_bytes + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, text, cut_bytes);
    out[cut_bytes] = '\0';

    ESP_LOGW(TAG, "TTS text truncated: max=%u chars, cut_bytes=%u", (unsigned)MIMI_VOICE_TTS_MAX_CHARS, (unsigned)cut_bytes);
    return out;
}

static int16_t fir5_s16_at_clamped(const int16_t *src, size_t src_samples, size_t idx)
{
    if (!src || src_samples == 0) {
        return 0;
    }

    size_t i0 = (idx >= 2) ? (idx - 2) : 0;
    size_t i1 = (idx >= 1) ? (idx - 1) : 0;
    size_t i2 = idx;
    if (i2 >= src_samples) i2 = src_samples - 1;
    size_t i3 = (idx + 1 < src_samples) ? (idx + 1) : (src_samples - 1);
    size_t i4 = (idx + 2 < src_samples) ? (idx + 2) : (src_samples - 1);

    int32_t acc =
        (int32_t)src[i0] * 1 +
        (int32_t)src[i1] * 4 +
        (int32_t)src[i2] * 6 +
        (int32_t)src[i3] * 4 +
        (int32_t)src[i4] * 1;

    acc = acc / 16;
    if (acc > INT16_MAX) acc = INT16_MAX;
    if (acc < INT16_MIN) acc = INT16_MIN;
    return (int16_t)acc;
}

static esp_err_t i2s_tx_write_silence_ms(uint32_t ms)
{
    if (!s_i2s_ready || !s_tx_chan || ms == 0) {
        return ESP_OK;
    }

    uint64_t frames_total = ((uint64_t)MIMI_VOICE_SAMPLE_RATE * (uint64_t)ms) / 1000ULL;
    while (frames_total > 0) {
        const size_t frames_chunk = (frames_total > 256) ? 256 : (size_t)frames_total;
        int32_t zeros[256 * 2] = {0};

        const uint8_t *p = (const uint8_t *)zeros;
        size_t bytes_total = frames_chunk * 2 * sizeof(int32_t);
        size_t bytes_sent = 0;

        while (bytes_sent < bytes_total) {
            size_t written = 0;
            esp_err_t err = i2s_channel_write(s_tx_chan,
                                              p + bytes_sent,
                                              bytes_total - bytes_sent,
                                              &written,
                                              pdMS_TO_TICKS(1000));
            if (err != ESP_OK) {
                return err;
            }
            if (written == 0) {
                return ESP_FAIL;
            }
            bytes_sent += written;
        }

        frames_total -= frames_chunk;
    }

    return ESP_OK;
}

static esp_err_t i2s_tx_overwrite_dma_with_zeros(void)
{
    if (!s_i2s_ready || !s_tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t remaining = MIMI_VOICE_TX_DMA_TOTAL_BYTES;
    while (remaining > 0) {
        int32_t zeros[256 * 2] = {0};
        size_t chunk = sizeof(zeros);
        if (chunk > remaining) {
            chunk = remaining;
        }

        const uint8_t *p = (const uint8_t *)zeros;
        size_t sent = 0;
        while (sent < chunk) {
            size_t written = 0;
            esp_err_t err = i2s_channel_write(s_tx_chan,
                                              p + sent,
                                              chunk - sent,
                                              &written,
                                              pdMS_TO_TICKS(1000));
            if (err != ESP_OK) {
                return err;
            }
            if (written == 0) {
                return ESP_FAIL;
            }
            sent += written;
        }

        remaining -= (uint32_t)chunk;
    }

    return ESP_OK;
}

static void pcm_s32_stereo_to_s16_mono(const uint8_t *src, size_t src_len, int16_t *dst, size_t *out_samples)
{
    size_t frames = src_len / VOICE_I2S_BYTES_PER_STEREO_FRAME;
    const int32_t *p = (const int32_t *)src;

    for (size_t i = 0; i < frames; i++) {
        int32_t l = p[i * 2 + 0];
        int32_t r = p[i * 2 + 1];

        /* XVF3800 / many I2S MEMS frontends deliver valid audio in high 16 bits of s32 slot. */
        int16_t ls = (int16_t)(l >> 16);
        int16_t rs = (int16_t)(r >> 16);
        int32_t mono = ((int32_t)ls + (int32_t)rs) / 2;

        if (mono > INT16_MAX) mono = INT16_MAX;
        if (mono < INT16_MIN) mono = INT16_MIN;
        dst[i] = (int16_t)mono;
    }

    if (out_samples) {
        *out_samples = frames;
    }
}

static uint32_t pcm_energy_absavg(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) return 0;

    uint64_t sum = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i];
        if (v < 0) v = -v;
        sum += (uint32_t)v;
    }
    return (uint32_t)(sum / samples);
}

static size_t wav_build_from_pcm16(const int16_t *pcm,
                                   size_t pcm_bytes,
                                   uint32_t sample_rate,
                                   uint16_t channels,
                                   uint8_t **out_buf)
{
    if (!pcm || !out_buf || pcm_bytes == 0) {
        return 0;
    }

    const size_t wav_size = 44 + pcm_bytes;
    uint8_t *buf = (uint8_t *)malloc(wav_size);
    if (!buf) {
        return 0;
    }

    const uint32_t byte_rate = sample_rate * channels * 2;
    const uint16_t block_align = channels * 2;
    const uint32_t riff_size = (uint32_t)(wav_size - 8);
    const uint32_t data_size = (uint32_t)pcm_bytes;

    memcpy(buf + 0, "RIFF", 4);
    memcpy(buf + 4, &riff_size, 4);
    memcpy(buf + 8, "WAVE", 4);

    memcpy(buf + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1;
    uint16_t bits_per_sample = 16;
    memcpy(buf + 16, &fmt_size, 4);
    memcpy(buf + 20, &audio_format, 2);
    memcpy(buf + 22, &channels, 2);
    memcpy(buf + 24, &sample_rate, 4);
    memcpy(buf + 28, &byte_rate, 4);
    memcpy(buf + 32, &block_align, 2);
    memcpy(buf + 34, &bits_per_sample, 2);

    memcpy(buf + 36, "data", 4);
    memcpy(buf + 40, &data_size, 4);
    memcpy(buf + 44, pcm, pcm_bytes);

    *out_buf = buf;
    return wav_size;
}

static esp_err_t wav_find_data_chunk(const uint8_t *wav,
                                     size_t wav_len,
                                     wav_fmt_t *fmt,
                                     const uint8_t **data_out,
                                     size_t *data_len_out)
{
    if (!wav || wav_len < 44 || !fmt || !data_out || !data_len_out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(fmt, 0, sizeof(*fmt));
    *data_out = NULL;
    *data_len_out = 0;

    size_t pos = 12;
    bool got_fmt = false;

    while (pos + 8 <= wav_len) {
        const uint8_t *chunk = wav + pos;
        uint32_t chunk_size = 0;
        memcpy(&chunk_size, chunk + 4, 4);

        size_t chunk_data_pos = pos + 8;
        if (chunk_data_pos > wav_len) {
            break;
        }

        size_t available = wav_len - chunk_data_pos;
        size_t declared = (size_t)chunk_size;
        size_t actual = declared <= available ? declared : available;

        char id[5] = {0};
        memcpy(id, chunk, 4);
        ESP_LOGI(TAG, "WAV chunk id=%s declared=%u actual=%u pos=%u",
                 id, (unsigned)chunk_size, (unsigned)actual, (unsigned)pos);

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (actual < 16) {
                ESP_LOGW(TAG, "WAV fmt chunk too short: %u", (unsigned)actual);
            } else {
                memcpy(&fmt->audio_format, wav + chunk_data_pos + 0, 2);
                memcpy(&fmt->channels, wav + chunk_data_pos + 2, 2);
                memcpy(&fmt->sample_rate, wav + chunk_data_pos + 4, 4);
                memcpy(&fmt->bits_per_sample, wav + chunk_data_pos + 14, 2);
                got_fmt = true;
            }
        } else if (memcmp(chunk, "data", 4) == 0) {
            *data_out = wav + chunk_data_pos;
            *data_len_out = actual;
            break;
        }

        size_t step = 8 + actual;
        if (declared <= available) {
            step += (declared & 1U);
        }

        if (step == 0 || pos + step <= pos) {
            break;
        }
        pos += step;
    }

    if (!*data_out || *data_len_out == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!got_fmt) {
        ESP_LOGW(TAG, "WAV fmt chunk not found, assume PCM16 mono/stereo fallback");
        fmt->audio_format = 1;
        fmt->channels = 1;
        fmt->sample_rate = MIMI_VOICE_SAMPLE_RATE;
        fmt->bits_per_sample = 16;
    }

    if (fmt->audio_format != 1) {
        ESP_LOGE(TAG, "Unsupported WAV audio_format=%u", (unsigned)fmt->audio_format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (fmt->bits_per_sample != 16) {
        ESP_LOGE(TAG, "Unsupported WAV bits_per_sample=%u", (unsigned)fmt->bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

/* =========================
 * STT / TTS JSON helpers
 * ========================= */

static char *build_data_url_from_wav(const uint8_t *wav, size_t wav_len)
{
    if (!wav || wav_len == 0) {
        return NULL;
    }

    size_t b64_len = 0;
    int rc = mbedtls_base64_encode(NULL, 0, &b64_len, wav, wav_len);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) {
        return NULL;
    }

    const char *prefix = "data:audio/wav;base64,";
    size_t prefix_len = strlen(prefix);
    char *out = (char *)malloc(prefix_len + b64_len + 1);
    if (!out) {
        return NULL;
    }

    memcpy(out, prefix, prefix_len);

    size_t actual = 0;
    rc = mbedtls_base64_encode((unsigned char *)(out + prefix_len),
                               b64_len,
                               &actual,
                               wav,
                               wav_len);
    if (rc != 0) {
        free(out);
        return NULL;
    }

    out[prefix_len + actual] = '\0';
    return out;
}

static esp_err_t parse_stt_response_text(const char *json, char *out_text, size_t out_size)
{
    if (!json || !out_text || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_text[0] = '\0';

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    cJSON *choice0 = (choices && cJSON_IsArray(choices)) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
    cJSON *content = message ? cJSON_GetObjectItem(message, "content") : NULL;

    if (cJSON_IsString(content) && content->valuestring) {
        strlcpy(out_text, content->valuestring, out_size);
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON_Delete(root);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t parse_tts_audio_url(const char *json, char *out_url, size_t out_size)
{
    if (!json || !out_url || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_url[0] = '\0';

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *output = cJSON_GetObjectItem(root, "output");
    cJSON *audio = output ? cJSON_GetObjectItem(output, "audio") : NULL;
    cJSON *url = audio ? cJSON_GetObjectItem(audio, "url") : NULL;

    if (cJSON_IsString(url) && url->valuestring && url->valuestring[0]) {
        strlcpy(out_url, url->valuestring, out_size);
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON_Delete(root);
    return ESP_ERR_NOT_FOUND;
}

/* =========================
 * Bus integration
 * ========================= */

static void push_voice_inbound(const char *text)
{
    if (!text || !text[0]) {
        return;
    }

    mimi_msg_t msg = {0};
    strlcpy(msg.channel, MIMI_CHAN_VOICE, sizeof(msg.channel));
    strlcpy(msg.chat_id, MIMI_VOICE_CHAT_ID, sizeof(msg.chat_id));
    msg.content = strdup(text);

    if (!msg.content) {
        ESP_LOGE(TAG, "No memory for voice inbound text");
        return;
    }

    if (message_bus_push_inbound(&msg) != ESP_OK) {
        ESP_LOGW(TAG, "Inbound queue full, drop voice transcript");
        free(msg.content);
    }
}

/* =========================
 * I2S init / playback
 * ========================= */

static esp_err_t i2s_init_xvf3800(void)
{
    esp_err_t err;

    i2s_chan_config_t chan_cfg = {
        .id = (i2s_port_t)MIMI_VOICE_I2S_PORT,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = MIMI_VOICE_I2S_DMA_DESC_NUM,
        .dma_frame_num = MIMI_VOICE_I2S_DMA_FRAME_NUM,
        .auto_clear_after_cb = false,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
    };

    err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t rx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIMI_VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIMI_VOICE_I2S_BCLK,
            .ws   = MIMI_VOICE_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIMI_VOICE_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    i2s_std_config_t tx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIMI_VOICE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIMI_VOICE_I2S_BCLK,
            .ws   = MIMI_VOICE_I2S_WS,
            .dout = MIMI_VOICE_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_rx_chan, &rx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_init_std_mode(s_tx_chan, &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Seed TX DMA with silence, otherwise some DAC/amps output "咚咚/沙沙" due to
     * undefined initial DMA content or repeating last buffer when idle.
     *
     * Only allowed before enabling the channel.
     */
    {
        int32_t zeros[128 * 2] = {0};
        size_t loaded = 0;
        (void)i2s_channel_preload_data(s_tx_chan, zeros, sizeof(zeros), &loaded);
    }

    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx enable failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx enable failed: %s", esp_err_to_name(err));
        return err;
    }

    s_i2s_ready = true;
    ESP_LOGI(TAG, "I2S ready: %dHz stereo s32 in / stereo s32 out (%s timing)",
             MIMI_VOICE_SAMPLE_RATE,
             i2s_slot_style_str());
    return ESP_OK;
}
static int16_t *resample_s16_mono_linear(const int16_t *src,
                                         size_t src_samples,
                                         uint32_t src_rate,
                                         uint32_t dst_rate,
                                         size_t *out_samples)
{
    if (!src || src_samples == 0 || !out_samples || src_rate == 0 || dst_rate == 0) {
        return NULL;
    }

    if (src_rate == dst_rate) {
        int16_t *copy = (int16_t *)malloc_prefer_spiram(src_samples * sizeof(int16_t));
        if (!copy) {
            return NULL;
        }
        memcpy(copy, src, src_samples * sizeof(int16_t));
        *out_samples = src_samples;
        return copy;
    }

    const bool is_downsampling = src_rate > dst_rate;

    size_t dst_samples = (size_t)(((uint64_t)src_samples * dst_rate) / src_rate);
    if (dst_samples == 0) {
        return NULL;
    }

    int16_t *dst = (int16_t *)malloc_prefer_spiram(dst_samples * sizeof(int16_t));
    if (!dst) {
        return NULL;
    }

    /* When downsampling (e.g. 24k -> 16k), naive linear interpolation tends to fold
     * high-frequency content above the new Nyquist into the audible band (aliasing),
     * often perceived as "沙沙" on sibilants/background.
     *
     * Apply a tiny 5-tap low-pass FIR [1,4,6,4,1]/16 on the source indices we touch.
     * This is cheap and improves subjective quality significantly without pulling in DSP deps.
     */
    for (size_t i = 0; i < dst_samples; i++) {
        float src_pos = ((float)i * (float)src_rate) / (float)dst_rate;
        size_t idx = (size_t)src_pos;
        float frac = src_pos - (float)idx;

        if (idx >= src_samples - 1) {
            dst[i] = src[src_samples - 1];
        } else {
            float a = (float)(is_downsampling ? fir5_s16_at_clamped(src, src_samples, idx) : src[idx]);
            float b = (float)(is_downsampling ? fir5_s16_at_clamped(src, src_samples, idx + 1) : src[idx + 1]);
            float v = a + (b - a) * frac;

            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            dst[i] = (int16_t)v;
        }
    }

    *out_samples = dst_samples;
    return dst;
}
static esp_err_t i2s_play_wav_pcm16(const uint8_t *wav, size_t wav_len)
{
    if (!s_i2s_ready || !s_tx_chan || !wav || wav_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    wav_fmt_t fmt;
    const uint8_t *pcm = NULL;
    size_t pcm_len = 0;

    esp_err_t err = wav_find_data_chunk(wav, wav_len, &fmt, &pcm, &pcm_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wav_find_data_chunk failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WAV fmt: format=%u channels=%u sample_rate=%u bits=%u data_len=%u",
             (unsigned)fmt.audio_format,
             (unsigned)fmt.channels,
             (unsigned)fmt.sample_rate,
             (unsigned)fmt.bits_per_sample,
             (unsigned)pcm_len);

    if (fmt.audio_format != 1 || fmt.bits_per_sample != 16) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int16_t *src16 = (const int16_t *)pcm;
    size_t src_samples_total = pcm_len / sizeof(int16_t);

    const int16_t *mono_src = NULL;
    int16_t *mono_owned = NULL;
    size_t mono_samples = 0;

    if (fmt.channels == 1) {
        mono_src = src16;
        mono_samples = src_samples_total;
    } else if (fmt.channels == 2) {
        mono_samples = src_samples_total / 2;
        mono_owned = (int16_t *)malloc_prefer_spiram(mono_samples * sizeof(int16_t));
        if (!mono_owned) {
            return ESP_ERR_NO_MEM;
        }

        for (size_t i = 0, j = 0; j < mono_samples; i += 2, j++) {
            int32_t v = ((int32_t)src16[i] + (int32_t)src16[i + 1]) / 2;
            mono_owned[j] = (int16_t)v;
        }
        mono_src = mono_owned;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int16_t *play_src = NULL;
    int16_t *play_owned = NULL;
    size_t play_samples = 0;

    if (fmt.sample_rate == MIMI_VOICE_SAMPLE_RATE) {
        play_src = mono_src;
        play_samples = mono_samples;
    } else {
        play_owned = resample_s16_mono_linear(
            mono_src,
            mono_samples,
            fmt.sample_rate,
            MIMI_VOICE_SAMPLE_RATE,
            &play_samples
        );
        free(mono_owned);
        mono_owned = NULL;

        if (!play_owned || play_samples == 0) {
            return ESP_ERR_NO_MEM;
        }
        play_src = play_owned;
    }

    ESP_LOGI(TAG, "Playback PCM: %u samples @ %u Hz (~%u ms)",
             (unsigned)play_samples,
             (unsigned)MIMI_VOICE_SAMPLE_RATE,
             (unsigned)((play_samples * 1000ULL) / MIMI_VOICE_SAMPLE_RATE));

    s_is_playing = true;

    size_t frames_total = play_samples;
    size_t frames_sent = 0;

    while (frames_sent < frames_total) {
        const size_t frames_chunk = (frames_total - frames_sent > 256) ? 256 : (frames_total - frames_sent);

        int32_t tx_buf[256 * 2];
        for (size_t i = 0; i < frames_chunk; i++) {
            int16_t s16 = play_src[frames_sent + i];
            int32_t s32 = ((int32_t)s16) << 16;
            tx_buf[i * 2 + 0] = s32;
            tx_buf[i * 2 + 1] = s32;
        }

        const uint8_t *p = (const uint8_t *)tx_buf;
        size_t bytes_total = frames_chunk * 2 * sizeof(int32_t);
        size_t bytes_sent = 0;

        while (bytes_sent < bytes_total) {
            size_t written = 0;
            err = i2s_channel_write(s_tx_chan,
                                    p + bytes_sent,
                                    bytes_total - bytes_sent,
                                    &written,
                                    pdMS_TO_TICKS(1000));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(err));
                free(play_owned);
                free(mono_owned);
                s_is_playing = false;
                return err;
            }
            if (written == 0) {
                ESP_LOGE(TAG, "i2s write returned 0 bytes");
                free(play_owned);
                free(mono_owned);
                s_is_playing = false;
                return ESP_FAIL;
            }
            bytes_sent += written;
        }

        frames_sent += frames_chunk;
    }

    /* Leave a short silence tail so the TX engine doesn't keep repeating the last
     * non-zero DMA buffer (often perceived as continuous "咚咚" when idle).
     */
    (void)i2s_tx_write_silence_ms(MIMI_VOICE_TX_SILENCE_TAIL_MS);
    (void)i2s_tx_overwrite_dma_with_zeros();

    free(play_owned);
    free(mono_owned);
    s_is_playing = false;
    return ESP_OK;
}

/* =========================
 * STT / TTS core
 * ========================= */

static esp_err_t stt_transcribe_pcm(const int16_t *pcm,
                                    size_t pcm_bytes,
                                    char *out_text,
                                    size_t out_text_size)
{
    if (!pcm || pcm_bytes == 0 || !out_text || out_text_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!stt_api_url()[0] || !stt_api_key()[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    out_text[0] = '\0';

    uint8_t *wav = NULL;
    size_t wav_len = wav_build_from_pcm16(pcm, pcm_bytes, MIMI_VOICE_SAMPLE_RATE, 1, &wav);
    if (!wav || wav_len == 0) {
        return ESP_ERR_NO_MEM;
    }

    char *data_url = build_data_url_from_wav(wav, wav_len);
    free(wav);
    if (!data_url) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", stt_model());
    cJSON_AddBoolToObject(root, "stream", false);

    cJSON *messages = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");

    cJSON *content = cJSON_CreateArray();
    cJSON *audio_item = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_item, "type", "input_audio");

    cJSON *input_audio = cJSON_CreateObject();
    cJSON_AddStringToObject(input_audio, "data", data_url);
    cJSON_AddItemToObject(audio_item, "input_audio", input_audio);
    cJSON_AddItemToArray(content, audio_item);

    cJSON_AddItemToObject(msg, "content", content);
    cJSON_AddItemToArray(messages, msg);
    cJSON_AddItemToObject(root, "messages", messages);

    cJSON *asr_options = cJSON_CreateObject();
    cJSON_AddBoolToObject(asr_options, "enable_itn", false);
    cJSON_AddItemToObject(root, "asr_options", asr_options);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(data_url);

    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    http_resp_t resp = {0};
    int http_status = 0;
    esp_err_t err = http_post_json(stt_api_url(), stt_api_key(), body, false, &resp, &http_status);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STT HTTP failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return err;
    }
    if (http_status < 200 || http_status >= 300) {
        ESP_LOGE(TAG, "STT HTTP status=%d body=%s", http_status, resp.buf ? resp.buf : "");
        free(resp.buf);
        return ESP_FAIL;
    }

    err = parse_stt_response_text(resp.buf ? resp.buf : "", out_text, out_text_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STT parse failed, body=%s", resp.buf ? resp.buf : "");
    } else {
        ESP_LOGI(TAG, "STT transcript: %s", out_text);
    }

    free(resp.buf);
    return err;
}

static esp_err_t tts_stream_play(const char *text)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tts_api_url()[0] || !tts_api_key()[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", tts_model());

    cJSON *input = cJSON_CreateObject();
    cJSON_AddStringToObject(input, "text", text);
    cJSON_AddStringToObject(input, "voice", tts_voice());
    cJSON_AddStringToObject(input, "language_type", tts_language());
    cJSON_AddItemToObject(body, "input", input);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    http_resp_t resp = {0};
    int http_status = 0;
    esp_err_t err = http_post_json(tts_api_url(), tts_api_key(), json, false, &resp, &http_status);
    free(json);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS HTTP failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return err;
    }
    if (http_status < 200 || http_status >= 300) {
        ESP_LOGE(TAG, "TTS HTTP status=%d body=%s", http_status, resp.buf ? resp.buf : "");
        free(resp.buf);
        return ESP_FAIL;
    }

    char wav_url[1024] = {0};
    err = parse_tts_audio_url(resp.buf ? resp.buf : "", wav_url, sizeof(wav_url));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS parse failed, body=%s", resp.buf ? resp.buf : "");
        free(resp.buf);
        return err;
    }
    free(resp.buf);

    ESP_LOGI(TAG, "TTS audio url: %s", wav_url);

    http_resp_t wav_resp = {0};
    http_status = 0;
    err = http_get_binary(wav_url, &wav_resp, &http_status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS wav download failed: %s", esp_err_to_name(err));
        free(wav_resp.buf);
        return err;
    }
    if (http_status < 200 || http_status >= 300) {
        ESP_LOGE(TAG, "TTS wav status=%d", http_status);
        free(wav_resp.buf);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TTS wav http_status=%d len=%d", http_status, (int)wav_resp.len);

    if (wav_resp.len >= 12) {
        ESP_LOGI(TAG, "TTS wav magic: %.4s / %.4s",
                wav_resp.buf,
                wav_resp.buf + 8);
    }

    if (wav_resp.len >= 4 && memcmp(wav_resp.buf, "RIFF", 4) != 0) {
        ESP_LOGE(TAG, "TTS response is not WAV, preview: %.120s", wav_resp.buf);
    }

    err = i2s_play_wav_pcm16((const uint8_t *)wav_resp.buf, wav_resp.len);
    free(wav_resp.buf);
    return err;
}

/* =========================
 * Voice capture loop
 * ========================= */

static void voice_capture_task(void *arg)
{
    (void)arg;

    const size_t frame_samples = (MIMI_VOICE_SAMPLE_RATE * MIMI_VOICE_FRAME_MS) / 1000;
    const size_t stereo_frame_bytes = frame_samples * VOICE_I2S_BYTES_PER_STEREO_FRAME;
    const size_t mono16_frame_bytes = frame_samples * sizeof(int16_t);
    const size_t max_frames = MIMI_VOICE_MAX_UTTERANCE_MS / MIMI_VOICE_FRAME_MS;
    const size_t silence_frames_end = MIMI_VOICE_SILENCE_END_MS / MIMI_VOICE_FRAME_MS;

    uint8_t *rx_buf = (uint8_t *)heap_caps_malloc(stereo_frame_bytes, MALLOC_CAP_SPIRAM);
    int16_t *mono_frame = (int16_t *)heap_caps_malloc(mono16_frame_bytes, MALLOC_CAP_SPIRAM);
    int16_t *utterance = (int16_t *)heap_caps_malloc(max_frames * frame_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);

    if (!rx_buf || !mono_frame || !utterance) {
        ESP_LOGE(TAG, "voice_capture_task alloc failed");
        free(rx_buf);
        free(mono_frame);
        free(utterance);
        vTaskDelete(NULL);
        return;
    }

    bool in_speech = false;
    size_t total_frames = 0;
    size_t silence_frames = 0;
    size_t start_frames = 0;
    TickType_t cooldown_until = 0;

    /* Simple adaptive noise floor */
    uint32_t noise_floor = MIMI_VOICE_VAD_THRESHOLD / 2;
    if (noise_floor < 100) noise_floor = 100;

    while (1) {
        if (!s_i2s_ready || !s_rx_chan) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (s_is_playing) {
            /* Avoid self-trigger during playback */
            vTaskDelay(pdMS_TO_TICKS(MIMI_VOICE_FRAME_MS));
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        if (cooldown_until != 0 && now < cooldown_until) {
            vTaskDelay(cooldown_until - now);
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan,
                                         rx_buf,
                                         stereo_frame_bytes,
                                         &bytes_read,
                                         pdMS_TO_TICKS(1000));
        if (err != ESP_OK || bytes_read == 0) {
            continue;
        }

        size_t mono_samples = 0;
        pcm_s32_stereo_to_s16_mono(rx_buf, bytes_read, mono_frame, &mono_samples);
        if (mono_samples == 0) {
            continue;
        }

        uint32_t energy = pcm_energy_absavg(mono_frame, mono_samples);

        /* Update noise floor only when not in speech */
        if (!in_speech) {
            noise_floor = (noise_floor * 15 + energy) / 16;
        }

        uint32_t dynamic_threshold = noise_floor + MIMI_VOICE_VAD_THRESHOLD;
        bool speech_now = (energy > dynamic_threshold);

        if (!in_speech) {
            if (!speech_now) {
                start_frames = 0;
                continue;
            }
            start_frames++;
            if (start_frames < MIMI_VOICE_VAD_START_FRAMES) {
                continue;
            }
            in_speech = true;
            total_frames = 0;
            silence_frames = 0;
            start_frames = 0;
        }

        if (total_frames < max_frames) {
            memcpy(&utterance[total_frames * frame_samples], mono_frame, mono16_frame_bytes);
            total_frames++;
        }

        if (speech_now) {
            silence_frames = 0;
        } else {
            silence_frames++;
        }

        bool end_by_silence = (silence_frames >= silence_frames_end);
        bool end_by_limit = (total_frames >= max_frames);

        if (!end_by_silence && !end_by_limit) {
            continue;
        }

        in_speech = false;

        /* Ignore ultra-short bursts */
        if (total_frames < MIMI_VOICE_VAD_MIN_FRAMES) {
            total_frames = 0;
            silence_frames = 0;
            cooldown_until = xTaskGetTickCount() + pdMS_TO_TICKS(MIMI_VOICE_STT_COOLDOWN_MS);
            continue;
        }

        size_t pcm_bytes = total_frames * frame_samples * sizeof(int16_t);
        char text[512] = {0};

        if (xSemaphoreTake(s_http_lock, pdMS_TO_TICKS(30000)) == pdTRUE) {
            esp_err_t stt_err = stt_transcribe_pcm(utterance, pcm_bytes, text, sizeof(text));
            xSemaphoreGive(s_http_lock);

            if (stt_err == ESP_OK && text[0]) {
                ESP_LOGI(TAG, "Voice STT: %s", text);
                push_voice_inbound(text);
            } else {
                ESP_LOGW(TAG, "STT failed or empty transcript");
            }
        }

        total_frames = 0;
        silence_frames = 0;
        cooldown_until = xTaskGetTickCount() + pdMS_TO_TICKS(MIMI_VOICE_STT_COOLDOWN_MS);
    }
}

/* =========================
 * Public API
 * ========================= */

esp_err_t voice_channel_init(void)
{
    s_enabled = (MIMI_VOICE_ENABLED_DEFAULT != 0) ||
                (stt_api_key()[0] && tts_api_key()[0]);

    if (!s_enabled) {
        ESP_LOGI(TAG, "Voice channel disabled (set STT/TTS API key or enable default)");
        return ESP_OK;
    }

    esp_err_t err = i2s_init_xvf3800();
    if (err != ESP_OK) {
        s_enabled = false;
        return ESP_OK;
    }

    s_http_lock = xSemaphoreCreateMutex();
    if (!s_http_lock) {
        ESP_LOGE(TAG, "Voice init failed: cannot allocate mutex");
        s_enabled = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t voice_channel_start(void)
{
    if (!s_enabled || !s_i2s_ready) {
        return ESP_OK;
    }

    if (!s_capture_task) {
        if (xTaskCreatePinnedToCore(voice_capture_task,
                                    "voice_cap",
                                    MIMI_VOICE_CAPTURE_STACK,
                                    NULL,
                                    MIMI_VOICE_TASK_PRIO,
                                    &s_capture_task,
                                    MIMI_VOICE_CORE) != pdPASS) {
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Voice channel started");
    return ESP_OK;
}

esp_err_t voice_channel_speak_text(const char *text)
{
    if (!s_enabled || !s_i2s_ready || !text || text[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_http_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    char *tts_text = voice_build_tts_text(text);
    esp_err_t err = tts_stream_play(tts_text ? tts_text : text);
    free(tts_text);

    xSemaphoreGive(s_http_lock);
    return err;
}

bool voice_channel_is_enabled(void)
{
    return s_enabled;
}

void voice_channel_get_status(voice_channel_status_t *status)
{
    if (!status) {
        return;
    }

    status->enabled = s_enabled;
    status->i2s_ready = s_i2s_ready;
    status->is_playing = s_is_playing;
    status->stt_configured = (stt_api_url()[0] != '\0' && stt_api_key()[0] != '\0');
    status->tts_configured = (tts_api_url()[0] != '\0' && tts_api_key()[0] != '\0');
}
