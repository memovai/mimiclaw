#include "voice_channel.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "input/button_input.h"
#include "display/display_driver.h"
#include "audio/audio_hal.h"
#include "audio/audio_player.h"
#include "voice/stt_client.h"
#include "voice/tts_client.h"
#include "voice/mp3_encoder.h"
#include "voice/mp3_decoder.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "voice_channel";

typedef enum {
    VOICE_STATE_IDLE,
    VOICE_STATE_RECORDING,
    VOICE_STATE_PROCESSING_STT,
    VOICE_STATE_WAITING_LLM,
    VOICE_STATE_PROCESSING_TTS,
    VOICE_STATE_PLAYING,
    VOICE_STATE_ERROR,
} voice_state_t;

static voice_state_t s_state = VOICE_STATE_IDLE;
static QueueHandle_t s_response_queue = NULL;
static TaskHandle_t s_task = NULL;

static int16_t *s_recording_buf = NULL;
static size_t s_recording_len = 0;
static uint8_t *s_tts_buf = NULL;

static const char *state_to_string(voice_state_t state)
{
    switch (state) {
        case VOICE_STATE_IDLE: return "IDLE";
        case VOICE_STATE_RECORDING: return "RECORDING";
        case VOICE_STATE_PROCESSING_STT: return "PROCESSING_STT";
        case VOICE_STATE_WAITING_LLM: return "WAITING_LLM";
        case VOICE_STATE_PROCESSING_TTS: return "PROCESSING_TTS";
        case VOICE_STATE_PLAYING: return "PLAYING";
        case VOICE_STATE_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static void set_state(voice_state_t new_state)
{
    s_state = new_state;
    ESP_LOGI(TAG, "State: %s", state_to_string(new_state));

    switch (new_state) {
        case VOICE_STATE_IDLE:
            display_set_state(DISPLAY_STATE_IDLE);
            break;
        case VOICE_STATE_RECORDING:
            display_set_state(DISPLAY_STATE_LISTENING);
            break;
        case VOICE_STATE_PROCESSING_STT:
        case VOICE_STATE_WAITING_LLM:
        case VOICE_STATE_PROCESSING_TTS:
            display_set_state(DISPLAY_STATE_THINKING);
            break;
        case VOICE_STATE_PLAYING:
            display_set_state(DISPLAY_STATE_SPEAKING);
            break;
        case VOICE_STATE_ERROR:
            display_set_state(DISPLAY_STATE_ERROR);
            break;
    }
}

static void button_callback(button_event_t event)
{
    if (event == BTN_EVENT_WAKE_PRESS) {
        if (s_state == VOICE_STATE_IDLE) {
            ESP_LOGI(TAG, "Button pressed: start recording");
            set_state(VOICE_STATE_RECORDING);
            s_recording_len = 0;
        }
    } else if (event == BTN_EVENT_WAKE_RELEASE) {
        if (s_state == VOICE_STATE_RECORDING) {
            ESP_LOGI(TAG, "Button released: stop recording (%d samples)", 
                     (int)(s_recording_len / sizeof(int16_t)));
            set_state(VOICE_STATE_PROCESSING_STT);
        }
    }
}

static void record_audio_chunk(void)
{
    if (s_state != VOICE_STATE_RECORDING) return;

    esp_codec_dev_handle_t in_dev = audio_hal_get_input_dev();
    const size_t chunk_samples = 160;
    const size_t stereo_bytes = chunk_samples * 2 * sizeof(int16_t);
    int16_t stereo_buf[chunk_samples * 2];

    int ret = esp_codec_dev_read(in_dev, stereo_buf, stereo_bytes);
    if (ret != 0) {
        ESP_LOGW(TAG, "Mic read error: %d", ret);
        return;
    }

    size_t max_bytes = MIMI_VOICE_RECORDING_BUF_SIZE;
    if (s_recording_len + chunk_samples * sizeof(int16_t) > max_bytes) {
        ESP_LOGW(TAG, "Recording buffer full (%d bytes), stopping", (int)s_recording_len);
        set_state(VOICE_STATE_PROCESSING_STT);
        return;
    }

    for (size_t i = 0; i < chunk_samples; i++) {
        s_recording_buf[s_recording_len / sizeof(int16_t)] = stereo_buf[i * 2];
        s_recording_len += sizeof(int16_t);
    }
}

static void process_recording(void)
{
    if (s_recording_len == 0) {
        ESP_LOGW(TAG, "No audio recorded");
        set_state(VOICE_STATE_IDLE);
        return;
    }

    ESP_LOGI(TAG, "Transcribing %d bytes of audio...", (int)s_recording_len);

    char text[512];
    esp_err_t err = stt_transcribe(s_recording_buf, s_recording_len, text, sizeof(text));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STT failed: %s", esp_err_to_name(err));
        set_state(VOICE_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        set_state(VOICE_STATE_IDLE);
        return;
    }

    if (strlen(text) == 0) {
        ESP_LOGW(TAG, "Empty transcription");
        set_state(VOICE_STATE_IDLE);
        return;
    }

    ESP_LOGI(TAG, "Transcribed: \"%s\"", text);

    mimi_msg_t msg = {0};
    strncpy(msg.channel, MIMI_CHAN_VOICE, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "local_user", sizeof(msg.chat_id) - 1);
    msg.content = strdup(text);

    if (msg.content) {
        set_state(VOICE_STATE_WAITING_LLM);
        message_bus_push_inbound(&msg);
    } else {
        ESP_LOGE(TAG, "Failed to allocate message content");
        set_state(VOICE_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        set_state(VOICE_STATE_IDLE);
    }
}

static void handle_response(const char *text)
{
    if (!text || strlen(text) == 0) {
        ESP_LOGW(TAG, "Empty response text");
        set_state(VOICE_STATE_IDLE);
        return;
    }
    
    /* Prevent duplicate processing */
    static char last_text[64] = {0};
    if (strncmp(text, last_text, sizeof(last_text) - 1) == 0) {
        ESP_LOGW(TAG, "Duplicate response detected, ignoring");
        set_state(VOICE_STATE_IDLE);
        return;
    }
    snprintf(last_text, sizeof(last_text), "%.63s", text);

    /* Parse JSON if present to extract output_to_user field */
    char *tts_text = NULL;
    cJSON *json = cJSON_Parse(text);
    if (json) {
        /* Check if it's a tool call structure (has "name" field) - reject it */
        cJSON *name = cJSON_GetObjectItem(json, "name");
        if (name && cJSON_IsString(name)) {
            ESP_LOGE(TAG, "Response contains tool call structure, not user text. Ignoring.");
            cJSON_Delete(json);
            set_state(VOICE_STATE_IDLE);
            return;
        }
        
        /* Try to extract output_to_user */
        cJSON *output = cJSON_GetObjectItem(json, "output_to_user");
        if (output && cJSON_IsString(output)) {
            tts_text = strdup(output->valuestring);
            ESP_LOGI(TAG, "Extracted output_to_user from JSON");
        }
        cJSON_Delete(json);
    }
    
    /* If not JSON or no output_to_user field, use the text as-is */
    if (!tts_text) {
        /* Check if it looks like JSON but we couldn't parse it */
        if (text[0] == '{' || text[0] == '[') {
            ESP_LOGE(TAG, "Response looks like JSON but no valid text field. Raw: %.100s", text);
            set_state(VOICE_STATE_IDLE);
            return;
        }
        tts_text = strdup(text);
    }
    
    if (!tts_text || strlen(tts_text) == 0) {
        ESP_LOGW(TAG, "No valid text to synthesize");
        free(tts_text);
        set_state(VOICE_STATE_IDLE);
        return;
    }

    ESP_LOGI(TAG, "Synthesizing response: \"%.50s...\"", tts_text);
    set_state(VOICE_STATE_PROCESSING_TTS);

    size_t wav_len = 0;
    esp_err_t err = tts_synthesize(tts_text, s_tts_buf, MIMI_VOICE_TTS_BUF_SIZE, &wav_len);
    free(tts_text);
    
    if (err != ESP_OK || wav_len == 0) {
        ESP_LOGE(TAG, "TTS failed: %s", esp_err_to_name(err));
        set_state(VOICE_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        set_state(VOICE_STATE_IDLE);
        return;
    }

    ESP_LOGI(TAG, "Playing %d bytes of audio...", (int)wav_len);
    set_state(VOICE_STATE_PLAYING);

    /* TTS returns raw PCM, play directly */
    err = audio_player_play_pcm((const int16_t *)s_tts_buf, wav_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Playback failed: %s", esp_err_to_name(err));
    }

    set_state(VOICE_STATE_IDLE);
}

static void voice_task(void *arg)
{
    ESP_LOGI(TAG, "Voice channel task started on core %d", xPortGetCoreID());

    char *response_text = NULL;

    while (1) {
        if (xQueueReceive(s_response_queue, &response_text, pdMS_TO_TICKS(10)) == pdTRUE) {
            handle_response(response_text);
            free(response_text);
            response_text = NULL;
            continue;
        }

        if (s_state == VOICE_STATE_RECORDING) {
            record_audio_chunk();
        } else if (s_state == VOICE_STATE_PROCESSING_STT) {
            process_recording();
            s_recording_len = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t voice_channel_init(void)
{
    ESP_LOGI(TAG, "Initializing voice channel...");

    s_recording_buf = heap_caps_malloc(MIMI_VOICE_RECORDING_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_recording_buf) {
        ESP_LOGE(TAG, "Failed to allocate recording buffer (%d bytes)", 
                 MIMI_VOICE_RECORDING_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    s_tts_buf = heap_caps_malloc(MIMI_VOICE_TTS_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_tts_buf) {
        ESP_LOGE(TAG, "Failed to allocate TTS buffer (%d bytes)", MIMI_VOICE_TTS_BUF_SIZE);
        free(s_recording_buf);
        return ESP_ERR_NO_MEM;
    }

    s_response_queue = xQueueCreate(4, sizeof(char *));
    if (!s_response_queue) {
        ESP_LOGE(TAG, "Failed to create response queue");
        free(s_recording_buf);
        free(s_tts_buf);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = button_input_init(button_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Button init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mp3_encoder_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MP3 encoder init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mp3_decoder_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MP3 decoder init failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        voice_task, "voice_channel",
        MIMI_VOICE_STACK, NULL,
        MIMI_VOICE_PRIO, &s_task,
        MIMI_VOICE_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create voice task");
        return ESP_FAIL;
    }

    set_state(VOICE_STATE_IDLE);
    ESP_LOGI(TAG, "Voice channel ready (recording: %d KB, TTS: %d KB)",
             MIMI_VOICE_RECORDING_BUF_SIZE / 1024, MIMI_VOICE_TTS_BUF_SIZE / 1024);

    return ESP_OK;
}

void voice_channel_send_response(const char *text)
{
    if (!text || !s_response_queue) return;

    char *text_copy = strdup(text);
    if (!text_copy) {
        ESP_LOGE(TAG, "Failed to duplicate response text");
        return;
    }

    if (xQueueSend(s_response_queue, &text_copy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Response queue full, dropping message");
        free(text_copy);
    }
}

const char *voice_channel_get_state(void)
{
    return state_to_string(s_state);
}
