#include "ggwave/ggwave_buzzer.h"
#include "mimi_config.h"

#include "ggwave/ggwave.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t kMaxPacketBytes = 64;
constexpr size_t kQueueDepth = 8;
constexpr int kSampleRate = 24000;
constexpr int kSamplesPerFrame = 512;
constexpr int kVolume = 35;
constexpr size_t kMonoChunkSamples = 256;

struct queued_text_t {
    char *text;
};

static const char *TAG = "ggwave_audio";
static QueueHandle_t s_queue;
static i2s_chan_handle_t s_i2s_tx;
static bool s_ready;

static size_t packet_length(const char *text, size_t remaining)
{
    size_t length = std::min(remaining, kMaxPacketBytes);
    if (length == remaining) return length;
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length;
    return length > 0 ? length : std::min(remaining, kMaxPacketBytes);
}

static ggwave_Parameters encoder_parameters(size_t payload_length)
{
    auto parameters = GGWave::getDefaultParameters();
    parameters.payloadLength = static_cast<int>(payload_length);
    parameters.sampleRateInp = kSampleRate;
    parameters.sampleRateOut = kSampleRate;
    parameters.sampleRate = kSampleRate;
    parameters.samplesPerFrame = kSamplesPerFrame;
    parameters.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16;
    parameters.sampleFormatOut = GGWAVE_SAMPLE_FORMAT_I16;
    parameters.operatingMode = GGWAVE_OPERATING_MODE_TX | GGWAVE_OPERATING_MODE_USE_DSS;
    return parameters;
}

static esp_err_t init_i2s()
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_i2s_tx, nullptr), TAG,
                        "Failed to allocate I2S channel");

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = static_cast<gpio_num_t>(MIMI_AUDIO_BCLK_GPIO),
            .ws = static_cast<gpio_num_t>(MIMI_AUDIO_LRCLK_GPIO),
            .dout = static_cast<gpio_num_t>(MIMI_AUDIO_DIN_GPIO),
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {},
        },
    };
    esp_err_t err = i2s_channel_init_std_mode(s_i2s_tx, &config);
    if (err != ESP_OK) {
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
        return err;
    }
    return ESP_OK;
}

static bool prepare_encoder()
{
    GGWave::setLogFile(stderr);
    GGWave::Protocols::tx().only(GGWAVE_PROTOCOL_AUDIBLE_FASTEST);

    GGWave self_test;
    static constexpr char kSelfTest[] = "test";
    if (!self_test.prepare(encoder_parameters(sizeof(kSelfTest) - 1))) {
        ESP_LOGE(TAG, "ggwave audio prepare failed");
        return false;
    }
    ESP_LOGI(TAG, "ggwave audio heap: %d bytes in PSRAM", self_test.heapSize());
    if (!self_test.init(sizeof(kSelfTest) - 1, kSelfTest,
                        GGWAVE_PROTOCOL_AUDIBLE_FASTEST, kVolume)) {
        ESP_LOGE(TAG, "ggwave audible-fastest protocol init failed");
        return false;
    }
    const uint32_t self_test_bytes = self_test.encode();
    if (self_test_bytes == 0 || !self_test.txWaveform()) {
        ESP_LOGE(TAG, "ggwave waveform generation failed: bytes=%lu waveform=%p",
                 static_cast<unsigned long>(self_test_bytes), self_test.txWaveform());
        return false;
    }
    ESP_LOGI(TAG, "ggwave audible-fastest encoder ready (%lu self-test bytes)",
             static_cast<unsigned long>(self_test_bytes));
    return true;
}

static bool write_mono_as_stereo(const int16_t *mono, size_t sample_count)
{
    int16_t stereo[kMonoChunkSamples * 2];
    for (size_t offset = 0; offset < sample_count;) {
        const size_t count = std::min(kMonoChunkSamples, sample_count - offset);
        for (size_t i = 0; i < count; ++i) {
            stereo[2 * i] = mono[offset + i];
            stereo[2 * i + 1] = mono[offset + i];
        }
        size_t bytes_written = 0;
        const esp_err_t err = i2s_channel_write(s_i2s_tx, stereo, count * 2 * sizeof(int16_t),
                                                &bytes_written, portMAX_DELAY);
        if (err != ESP_OK || bytes_written != count * 2 * sizeof(int16_t)) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
            return false;
        }
        offset += count;
    }
    return true;
}

static void transmit_packet(const char *payload, size_t payload_length)
{
    GGWave encoder;
    if (!encoder.prepare(encoder_parameters(payload_length)) ||
        !encoder.init(static_cast<int>(payload_length), payload,
                      GGWAVE_PROTOCOL_AUDIBLE_FASTEST, kVolume)) {
        ESP_LOGE(TAG, "ggwave rejected %d-byte payload", static_cast<int>(payload_length));
        return;
    }
    const uint32_t waveform_bytes = encoder.encode();
    const auto *waveform = static_cast<const int16_t *>(encoder.txWaveform());
    if (!waveform_bytes || !waveform) {
        ESP_LOGE(TAG, "ggwave produced no audio waveform");
        return;
    }

    ESP_LOGI(TAG, "Transmitting ggwave packet: %d bytes, %.2f seconds",
             static_cast<int>(payload_length),
             static_cast<double>(waveform_bytes / sizeof(int16_t)) / kSampleRate);
    if (i2s_channel_enable(s_i2s_tx) != ESP_OK) return;
    write_mono_as_stereo(waveform, waveform_bytes / sizeof(int16_t));

    const int16_t silence[kMonoChunkSamples * 2] = {};
    size_t ignored = 0;
    i2s_channel_write(s_i2s_tx, silence, sizeof(silence), &ignored, portMAX_DELAY);
    i2s_channel_disable(s_i2s_tx);
}

static void ggwave_player_task(void *)
{
    for (;;) {
        queued_text_t item = {};
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE || !item.text) continue;

        const size_t text_length = strlen(item.text);
        for (size_t offset = 0; offset < text_length;) {
            const size_t length = packet_length(item.text + offset, text_length - offset);
            transmit_packet(item.text + offset, length);
            offset += length;
            if (offset < text_length) vTaskDelay(pdMS_TO_TICKS(80));
        }
        free(item.text);
    }
}

} // namespace

extern "C" esp_err_t ggwave_buzzer_init(void)
{
    if (s_ready) return ESP_OK;
    if (!prepare_encoder()) return ESP_FAIL;
    ESP_RETURN_ON_ERROR(init_i2s(), TAG, "I2S audio output initialization failed");

    s_queue = xQueueCreate(kQueueDepth, sizeof(queued_text_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(ggwave_player_task, "ggwave_tx", 8192, nullptr, 3, nullptr, 0) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = nullptr;
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "NS4168 I2S ready: BCLK=%d LRCLK=%d DIN=%d, %d Hz",
             MIMI_AUDIO_BCLK_GPIO, MIMI_AUDIO_LRCLK_GPIO, MIMI_AUDIO_DIN_GPIO, kSampleRate);
    return ESP_OK;
}

extern "C" esp_err_t ggwave_buzzer_enqueue(const char *text)
{
    if (!text || !text[0]) return ESP_OK;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    queued_text_t item = { .text = strdup(text) };
    if (!item.text) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Queued full ggwave reply: %d bytes", static_cast<int>(strlen(item.text)));
    if (xQueueSend(s_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(item.text);
        ESP_LOGW(TAG, "ggwave queue full; reply was not transmitted");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
