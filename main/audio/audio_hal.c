#include "audio_hal.h"
#include "mimi_config.h"

#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/gpio.h"

/* esp_codec_dev component headers */
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "audio_hal";

/* I2C */
static i2c_master_bus_handle_t s_i2c_bus = NULL;

/* I2S channels */
static i2s_chan_handle_t s_i2s_tx = NULL;
static i2s_chan_handle_t s_i2s_rx = NULL;

/* Codec devices (esp_codec_dev) */
static esp_codec_dev_handle_t s_output_dev = NULL;
static esp_codec_dev_handle_t s_input_dev = NULL;

/* ─── I2C Bus ─── */

static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = MIMI_I2C_NUM,
        .sda_io_num = MIMI_I2C_SDA,
        .scl_io_num = MIMI_I2C_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d)", MIMI_I2C_SDA, MIMI_I2C_SCL);

    /* Probe codecs (i2c_master_probe uses 7-bit addresses) */
    esp_err_t es8311_probe = i2c_master_probe(s_i2c_bus, ES8311_CODEC_DEFAULT_ADDR >> 1, 100);
    esp_err_t es7210_probe = i2c_master_probe(s_i2c_bus, ES7210_CODEC_DEFAULT_ADDR >> 1, 100);

    ESP_LOGI(TAG, "ES8311 (0x%02x) probe: %s", ES8311_CODEC_DEFAULT_ADDR, es8311_probe == ESP_OK ? "FOUND" : "NOT FOUND");
    ESP_LOGI(TAG, "ES7210 (0x%02x) probe: %s", ES7210_CODEC_DEFAULT_ADDR, es7210_probe == ESP_OK ? "FOUND" : "NOT FOUND");

    if (es8311_probe != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 speaker DAC not detected! Speaker will not work.");
    }
    if (es7210_probe != ESP_OK) {
        ESP_LOGW(TAG, "ES7210 mic ADC not detected! Microphone will not work.");
    }

    return ESP_OK;
}

/* ─── I2S Channels ─── */

static esp_err_t init_i2s(void)
{
    /* Create I2S channel pair (TX + RX on same port) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIMI_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = MIMI_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = MIMI_I2S_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* --- TX channel: Standard mode (speaker via ES8311) --- */
    i2s_std_config_t tx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = MIMI_AUDIO_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = MIMI_I2S_MCLK,
            .bclk = MIMI_I2S_BCLK,
            .ws   = MIMI_I2S_LRCLK,
            .dout = MIMI_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ret = i2s_channel_init_std_mode(s_i2s_tx, &tx_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2S TX initialized (STD mode, %dHz, DOUT=%d)", MIMI_AUDIO_SAMPLE_RATE, MIMI_I2S_DOUT);

    /* --- RX channel: TDM mode (mic via ES7210, 4-channel ADC) --- */
    i2s_tdm_config_t rx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = MIMI_AUDIO_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   /* Shared with TX */
            .bclk = I2S_GPIO_UNUSED,   /* Shared with TX */
            .ws   = I2S_GPIO_UNUSED,   /* Shared with TX */
            .dout = I2S_GPIO_UNUSED,
            .din  = MIMI_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ret = i2s_channel_init_tdm_mode(s_i2s_rx, &rx_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S RX TDM init failed: %s, trying STD mode...", esp_err_to_name(ret));

        /* Fallback: try standard mode for RX */
        i2s_std_config_t rx_std_cfg = {
            .clk_cfg = {
                .sample_rate_hz = MIMI_AUDIO_SAMPLE_RATE,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            },
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = I2S_GPIO_UNUSED,
                .ws   = I2S_GPIO_UNUSED,
                .dout = I2S_GPIO_UNUSED,
                .din  = MIMI_I2S_DIN,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv   = false,
                },
            },
        };
        ret = i2s_channel_init_std_mode(s_i2s_rx, &rx_std_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S RX STD init also failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "I2S RX initialized (STD fallback mode, %dHz, DIN=%d)", MIMI_AUDIO_SAMPLE_RATE, MIMI_I2S_DIN);
    } else {
        ESP_LOGI(TAG, "I2S RX initialized (TDM mode, 4 slots, %dHz, DIN=%d)", MIMI_AUDIO_SAMPLE_RATE, MIMI_I2S_DIN);
    }

    return ESP_OK;
}

/* ─── Codec Initialization via esp_codec_dev ─── */

static esp_err_t init_codecs(void)
{
    /* --- ES8311 (Speaker DAC) --- */
    audio_codec_i2c_cfg_t es8311_i2c_cfg = {
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *es8311_ctrl = audio_codec_new_i2c_ctrl(&es8311_i2c_cfg);
    if (!es8311_ctrl) {
        ESP_LOGE(TAG, "Failed to create ES8311 I2C ctrl");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = es8311_ctrl,
        .pa_pin = MIMI_SPEAKER_EN,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .use_mclk = true,
    };
    const audio_codec_if_t *es8311_codec = es8311_codec_new(&es8311_cfg);
    if (!es8311_codec) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec interface");
        return ESP_FAIL;
    }

    /* --- ES7210 (Mic ADC) --- */
    audio_codec_i2c_cfg_t es7210_i2c_cfg = {
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *es7210_ctrl = audio_codec_new_i2c_ctrl(&es7210_i2c_cfg);
    if (!es7210_ctrl) {
        ESP_LOGE(TAG, "Failed to create ES7210 I2C ctrl");
        return ESP_FAIL;
    }

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = es7210_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *es7210_codec = es7210_codec_new(&es7210_cfg);
    if (!es7210_codec) {
        ESP_LOGE(TAG, "Failed to create ES7210 codec interface");
        return ESP_FAIL;
    }

    /* --- Create I2S data interfaces (wraps our I2S channel handles) --- */
    audio_codec_i2s_cfg_t i2s_tx_cfg = {
        .port = MIMI_I2S_NUM,
        .tx_handle = s_i2s_tx,
    };
    const audio_codec_data_if_t *i2s_tx_data = audio_codec_new_i2s_data(&i2s_tx_cfg);
    if (!i2s_tx_data) {
        ESP_LOGE(TAG, "Failed to create I2S TX data interface");
        return ESP_FAIL;
    }

    audio_codec_i2s_cfg_t i2s_rx_cfg = {
        .port = MIMI_I2S_NUM,
        .rx_handle = s_i2s_rx,
    };
    const audio_codec_data_if_t *i2s_rx_data = audio_codec_new_i2s_data(&i2s_rx_cfg);
    if (!i2s_rx_data) {
        ESP_LOGE(TAG, "Failed to create I2S RX data interface");
        return ESP_FAIL;
    }

    /* --- Create esp_codec_dev output device (ES8311 + I2S TX) --- */
    esp_codec_dev_cfg_t out_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_codec,
        .data_if = i2s_tx_data,
    };
    s_output_dev = esp_codec_dev_new(&out_cfg);
    if (!s_output_dev) {
        ESP_LOGE(TAG, "Failed to create output codec device");
        return ESP_FAIL;
    }

    /* --- Create esp_codec_dev input device (ES7210 + I2S RX) --- */
    esp_codec_dev_cfg_t in_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_codec,
        .data_if = i2s_rx_data,
    };
    s_input_dev = esp_codec_dev_new(&in_cfg);
    if (!s_input_dev) {
        ESP_LOGE(TAG, "Failed to create input codec device");
        return ESP_FAIL;
    }

    /* Open output device — configures ES8311 registers + enables I2S TX */
    esp_codec_dev_sample_info_t out_fs = {
        .bits_per_sample = MIMI_AUDIO_BITS,
        .channel = 2,
        .sample_rate = MIMI_AUDIO_SAMPLE_RATE,
    };
    int ret = esp_codec_dev_open(s_output_dev, &out_fs);
    if (ret != 0) {
        ESP_LOGW(TAG, "ES8311 codec open failed: %d (continuing anyway)", ret);
    } else {
        esp_codec_dev_set_out_vol(s_output_dev, 60);
        ESP_LOGI(TAG, "ES8311 codec opened (DAC, %dHz, vol=60)", MIMI_AUDIO_SAMPLE_RATE);
    }

    /* Open input device — configures ES7210 registers + enables I2S RX */
    esp_codec_dev_sample_info_t in_fs = {
        .bits_per_sample = MIMI_AUDIO_BITS,
        .channel = 2,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = MIMI_AUDIO_SAMPLE_RATE,
    };
    ret = esp_codec_dev_open(s_input_dev, &in_fs);
    if (ret != 0) {
        ESP_LOGW(TAG, "ES7210 codec open failed: %d (continuing anyway)", ret);
    } else {
        esp_codec_dev_set_in_gain(s_input_dev, 30.0);
        ESP_LOGI(TAG, "ES7210 codec opened (ADC, %dHz, gain=30dB)", MIMI_AUDIO_SAMPLE_RATE);
    }

    return ESP_OK;
}

/* ─── Speaker PA GPIO ─── */

static esp_err_t init_speaker_pa(void)
{
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << MIMI_SPEAKER_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&pa_cfg);
    if (ret != ESP_OK) return ret;

    gpio_set_level(MIMI_SPEAKER_EN, 0);  /* Start disabled */
    ESP_LOGI(TAG, "Speaker PA GPIO %d configured (disabled)", MIMI_SPEAKER_EN);
    return ESP_OK;
}

/* ─── Public API ─── */

esp_err_t audio_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing audio HAL...");

    esp_err_t ret;

    ret = init_speaker_pa();
    if (ret != ESP_OK) return ret;

    ret = init_i2c();
    if (ret != ESP_OK) return ret;

    /* I2S must be initialized before codecs — esp_codec_dev needs I2S handles */
    ret = init_i2s();
    if (ret != ESP_OK) return ret;

    ret = init_codecs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Codec init had issues, continuing...");
    }

    ESP_LOGI(TAG, "Audio HAL initialized successfully");
    return ESP_OK;
}

esp_codec_dev_handle_t audio_hal_get_output_dev(void)
{
    return s_output_dev;
}

esp_codec_dev_handle_t audio_hal_get_input_dev(void)
{
    return s_input_dev;
}

i2s_chan_handle_t audio_hal_get_tx(void)
{
    return s_i2s_tx;
}

i2s_chan_handle_t audio_hal_get_rx(void)
{
    return s_i2s_rx;
}

void audio_hal_speaker_pa(bool enable)
{
    gpio_set_level(MIMI_SPEAKER_EN, enable ? 1 : 0);
}

esp_err_t audio_hal_set_volume(int volume)
{
    if (!s_output_dev) return ESP_ERR_INVALID_STATE;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    return esp_codec_dev_set_out_vol(s_output_dev, volume);
}
