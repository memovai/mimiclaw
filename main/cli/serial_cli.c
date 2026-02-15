#include "serial_cli.h"
#include "mimi_config.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
#include "llm/llm_proxy.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "proxy/http_proxy.h"
#include "tools/tool_web_search.h"
#include "audio/audio_player.h"
#include "audio/test_audio.h"
#include "display/display_driver.h"
#include "input/button_input.h"
#include "voice/stt_client.h"
#include "voice/tts_client.h"
#include "voice/voice_channel.h"
#include "wake/wake_word.h"
#include "audio/audio_hal.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "argtable3/argtable3.h"

static const char *TAG = "cli";

/* --- wifi_set command --- */
static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_set_args;

static int cmd_wifi_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wifi_set_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_set_args.end, argv[0]);
        return 1;
    }
    wifi_manager_set_credentials(wifi_set_args.ssid->sval[0],
                                  wifi_set_args.password->sval[0]);
    printf("WiFi credentials saved. Restart to apply.\n");
    return 0;
}

/* --- wifi_status command --- */
static int cmd_wifi_status(int argc, char **argv)
{
    printf("WiFi connected: %s\n", wifi_manager_is_connected() ? "yes" : "no");
    printf("IP: %s\n", wifi_manager_get_ip());
    return 0;
}

/* --- set_tg_token command --- */
static struct {
    struct arg_str *token;
    struct arg_end *end;
} tg_token_args;

static int cmd_set_tg_token(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_token_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tg_token_args.end, argv[0]);
        return 1;
    }
    telegram_set_token(tg_token_args.token->sval[0]);
    printf("Telegram bot token saved.\n");
    return 0;
}

/* --- set_api_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} api_key_args;

static int cmd_set_api_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&api_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, api_key_args.end, argv[0]);
        return 1;
    }
    llm_set_api_key(api_key_args.key->sval[0]);
    printf("API key saved.\n");
    return 0;
}

/* --- set_model command --- */
static struct {
    struct arg_str *model;
    struct arg_end *end;
} model_args;

static int cmd_set_model(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&model_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, model_args.end, argv[0]);
        return 1;
    }
    llm_set_model(model_args.model->sval[0]);
    printf("Model set.\n");
    return 0;
}

/* --- set_model_provider command --- */
static struct {
    struct arg_str *provider;
    struct arg_end *end;
} provider_args;

static int cmd_set_model_provider(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&provider_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, provider_args.end, argv[0]);
        return 1;
    }
    llm_set_provider(provider_args.provider->sval[0]);
    printf("Model provider set.\n");
    return 0;
}

/* --- set_api_url command --- */
static struct {
    struct arg_str *url;
    struct arg_end *end;
} api_url_args;

static int cmd_set_api_url(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&api_url_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, api_url_args.end, argv[0]);
        return 1;
    }
    llm_set_api_url(api_url_args.url->sval[0]);
    printf("API URL set.\n");
    return 0;
}

/* --- memory_read command --- */
static int cmd_memory_read(int argc, char **argv)
{
    char *buf = malloc(4096);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }
    if (memory_read_long_term(buf, 4096) == ESP_OK && buf[0]) {
        printf("=== MEMORY.md ===\n%s\n=================\n", buf);
    } else {
        printf("MEMORY.md is empty or not found.\n");
    }
    free(buf);
    return 0;
}

/* --- memory_write command --- */
static struct {
    struct arg_str *content;
    struct arg_end *end;
} memory_write_args;

static int cmd_memory_write(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&memory_write_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, memory_write_args.end, argv[0]);
        return 1;
    }
    memory_write_long_term(memory_write_args.content->sval[0]);
    printf("MEMORY.md updated.\n");
    return 0;
}

/* --- session_list command --- */
static int cmd_session_list(int argc, char **argv)
{
    printf("Sessions:\n");
    session_list();
    return 0;
}

/* --- session_clear command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_end *end;
} session_clear_args;

static int cmd_session_clear(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&session_clear_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, session_clear_args.end, argv[0]);
        return 1;
    }
    if (session_clear(session_clear_args.chat_id->sval[0]) == ESP_OK) {
        printf("Session cleared.\n");
    } else {
        printf("Session not found.\n");
    }
    return 0;
}

/* --- heap_info command --- */
static int cmd_heap_info(int argc, char **argv)
{
    printf("Internal free: %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("PSRAM free:    %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("Total free:    %d bytes\n",
           (int)esp_get_free_heap_size());
    return 0;
}

/* --- set_proxy command --- */
static struct {
    struct arg_str *host;
    struct arg_int *port;
    struct arg_end *end;
} proxy_args;

static int cmd_set_proxy(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&proxy_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, proxy_args.end, argv[0]);
        return 1;
    }
    http_proxy_set(proxy_args.host->sval[0], (uint16_t)proxy_args.port->ival[0]);
    printf("Proxy set. Restart to apply.\n");
    return 0;
}

/* --- clear_proxy command --- */
static int cmd_clear_proxy(int argc, char **argv)
{
    http_proxy_clear();
    printf("Proxy cleared. Restart to apply.\n");
    return 0;
}

/* --- set_search_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} search_key_args;

static int cmd_set_search_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&search_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, search_key_args.end, argv[0]);
        return 1;
    }
    tool_web_search_set_key(search_key_args.key->sval[0]);
    printf("Search API key saved.\n");
    return 0;
}

/* --- wifi_scan command --- */
static int cmd_wifi_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_manager_scan_and_print();
    return 0;
}

/* --- config_show command --- */
static void print_config(const char *label, const char *ns, const char *key,
                         const char *build_val, bool mask)
{
    char nvs_val[128] = {0};
    const char *source = "not set";
    const char *display = "(empty)";

    /* NVS takes highest priority */
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(nvs_val);
        if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
            source = "NVS";
            display = nvs_val;
        }
        nvs_close(nvs);
    }

    /* Fall back to build-time value */
    if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
        source = "build";
        display = build_val;
    }

    if (mask && strlen(display) > 6 && strcmp(display, "(empty)") != 0) {
        printf("  %-14s: %.4s****  [%s]\n", label, display, source);
    } else {
        printf("  %-14s: %s  [%s]\n", label, display, source);
    }
}

static int cmd_config_show(int argc, char **argv)
{
    printf("=== Current Configuration ===\n");
    print_config("WiFi SSID",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_SSID,     MIMI_SECRET_WIFI_SSID,  false);
    print_config("WiFi Pass",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_PASS,     MIMI_SECRET_WIFI_PASS,  true);
    print_config("TG Token",   MIMI_NVS_TG,     MIMI_NVS_KEY_TG_TOKEN, MIMI_SECRET_TG_TOKEN,   true);
    print_config("API Key",    MIMI_NVS_LLM,    MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_API_KEY,    true);
    print_config("Model",      MIMI_NVS_LLM,    MIMI_NVS_KEY_MODEL,    MIMI_SECRET_MODEL,      false);
    print_config("Provider",   MIMI_NVS_LLM,    MIMI_NVS_KEY_PROVIDER, MIMI_SECRET_MODEL_PROVIDER, false);
    print_config("API URL",    MIMI_NVS_LLM,    MIMI_NVS_KEY_API_URL,  MIMI_SECRET_API_URL,    false);
    print_config("Proxy Host", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_HOST, MIMI_SECRET_PROXY_HOST, false);
    print_config("Proxy Port", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_PORT, MIMI_SECRET_PROXY_PORT, false);
    print_config("Search Key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_SEARCH_KEY, true);
    printf("=============================\n");
    return 0;
}

/* --- config_reset command --- */
static int cmd_config_reset(int argc, char **argv)
{
    const char *namespaces[] = {
        MIMI_NVS_WIFI, MIMI_NVS_TG, MIMI_NVS_LLM, MIMI_NVS_PROXY, MIMI_NVS_SEARCH
    };
    for (int i = 0; i < 5; i++) {
        nvs_handle_t nvs;
        if (nvs_open(namespaces[i], NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_all(nvs);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    printf("All NVS config cleared. Build-time defaults will be used on restart.\n");
    return 0;
}

/* --- play_tone command --- */
static struct {
    struct arg_int *freq;
    struct arg_int *ms;
    struct arg_end *end;
} play_tone_args;

static int cmd_play_tone(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&play_tone_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, play_tone_args.end, argv[0]);
        return 1;
    }
    int freq = play_tone_args.freq->ival[0];
    int ms = play_tone_args.ms->ival[0];
    if (freq < 20 || freq > 20000) {
        printf("Frequency must be 20-20000 Hz\n");
        return 1;
    }
    if (ms < 10 || ms > 30000) {
        printf("Duration must be 10-30000 ms\n");
        return 1;
    }
    printf("Playing %d Hz for %d ms (repeating 10x, Ctrl+C to stop)...\n", freq, ms);
    for (int i = 0; i < 10; i++) {
        printf("  [%d/10]\n", i + 1);
        esp_err_t ret = audio_player_play_tone((uint32_t)freq, (uint32_t)ms);
        if (ret != ESP_OK) {
            printf("Play tone failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    printf("Done.\n");
    return 0;
}

/* --- audio_test command --- */
static int cmd_audio_test(int argc, char **argv)
{
    audio_test_run();
    return 0;
}

/* --- display_test command --- */
static int cmd_display_test(int argc, char **argv)
{
    printf("Initializing display...\n");
    esp_err_t ret = display_init();
    if (ret != ESP_OK) {
        printf("Display init failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("Cycling colors (2s each)...\n");
    const struct { const char *name; uint16_t color; } colors[] = {
        {"RED",    DISPLAY_COLOR_RED},
        {"GREEN",  DISPLAY_COLOR_GREEN},
        {"BLUE",   DISPLAY_COLOR_BLUE},
        {"YELLOW", DISPLAY_COLOR_YELLOW},
        {"WHITE",  DISPLAY_COLOR_WHITE},
        {"BLACK",  DISPLAY_COLOR_BLACK},
    };
    for (int i = 0; i < 6; i++) {
        printf("  %s\n", colors[i].name);
        display_fill_color(colors[i].color);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    printf("Display test done.\n");
    return 0;
}

/* --- button_test command --- */
static void button_test_cb(button_event_t event)
{
    const char *names[] = {"WAKE_PRESS", "WAKE_RELEASE", "MUTE_PRESS", "VOLUME_PRESS"};
    printf(">>> BUTTON EVENT: %s\n", names[event]);
}

static int cmd_button_test(int argc, char **argv)
{
    static bool inited = false;
    if (!inited) {
        esp_err_t ret = button_input_init(button_test_cb);
        if (ret != ESP_OK) {
            printf("Button init failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        inited = true;
    }
    printf("Buttons active — press any button (events print in real-time)\n");
    printf("Wake=GPIO0 (top), Mute=GPIO39 (left), Volume=GPIO40 (right)\n");
    return 0;
}

/* --- stt_test command: record N seconds, transcribe via faster-whisper --- */
static struct {
    struct arg_int *seconds;
    struct arg_end *end;
} stt_test_args;

static int cmd_stt_test(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&stt_test_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, stt_test_args.end, argv[0]);
        return 1;
    }

    int secs = stt_test_args.seconds->ival[0];
    if (secs < 1) secs = 1;
    if (secs > 10) secs = 10;

    esp_codec_dev_handle_t in_dev = audio_hal_get_input_dev();
    if (!in_dev) {
        printf("Mic not initialized\n");
        return 1;
    }

    /* Record mono 16kHz 16-bit into PSRAM */
    size_t total_bytes = MIMI_AUDIO_SAMPLE_RATE * sizeof(int16_t) * secs;
    int16_t *pcm_buf = heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM);
    if (!pcm_buf) {
        printf("Failed to allocate %d bytes for recording\n", (int)total_bytes);
        return 1;
    }

    printf("Recording %d seconds... speak now!\n", secs);

    /* Read mic via esp_codec_dev — reads stereo (2ch), we take left channel */
    const int chunk_frames = 512;
    const int stereo_chunk_bytes = chunk_frames * 2 * sizeof(int16_t);
    int16_t *stereo_buf = malloc(stereo_chunk_bytes);
    if (!stereo_buf) {
        free(pcm_buf);
        printf("malloc failed\n");
        return 1;
    }

    size_t mono_written = 0;
    size_t total_samples = total_bytes / sizeof(int16_t);
    while (mono_written < total_samples) {
        size_t frames_needed = total_samples - mono_written;
        if (frames_needed > (size_t)chunk_frames) frames_needed = chunk_frames;

        int ret = esp_codec_dev_read(in_dev, stereo_buf, frames_needed * 2 * sizeof(int16_t));
        if (ret != 0) {
            printf("Mic read error: %d\n", ret);
            break;
        }

        /* Extract left channel (every other sample) */
        for (size_t i = 0; i < frames_needed; i++) {
            pcm_buf[mono_written + i] = stereo_buf[i * 2];
        }
        mono_written += frames_needed;
    }
    free(stereo_buf);

    size_t recorded_bytes = mono_written * sizeof(int16_t);
    printf("Recorded %d samples (%d bytes, %.1f sec)\n",
           (int)mono_written, (int)recorded_bytes,
           (float)mono_written / MIMI_AUDIO_SAMPLE_RATE);

    /* Transcribe */
    printf("Sending to STT server...\n");
    char text[512] = {0};
    esp_err_t err = stt_transcribe(pcm_buf, recorded_bytes, text, sizeof(text));
    free(pcm_buf);

    if (err != ESP_OK) {
        printf("STT failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Transcription: \"%s\"\n", text);
    return 0;
}

/* --- tts_test command: synthesize text and play through speaker --- */
static struct {
    struct arg_str *text;
    struct arg_end *end;
} tts_test_args;

static int cmd_tts_test(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tts_test_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tts_test_args.end, argv[0]);
        return 1;
    }

    const char *text = tts_test_args.text->sval[0];
    printf("Synthesizing: \"%s\"\n", text);

    /* Allocate buffer in PSRAM for audio response (up to 512KB) */
    const size_t buf_cap = 512 * 1024;
    uint8_t *wav_buf = heap_caps_malloc(buf_cap, MALLOC_CAP_SPIRAM);
    if (!wav_buf) {
        printf("Failed to allocate TTS buffer\n");
        return 1;
    }

    size_t wav_len = 0;
    esp_err_t err = tts_synthesize(text, wav_buf, buf_cap, &wav_len);
    if (err != ESP_OK) {
        printf("TTS failed: %s\n", esp_err_to_name(err));
        free(wav_buf);
        return 1;
    }

    printf("Got %d bytes audio, playing...\n", (int)wav_len);

    /* Try to play as WAV first, fall back to raw PCM */
    if (wav_len >= 44 && memcmp(wav_buf, "RIFF", 4) == 0) {
        err = audio_player_play_wav(wav_buf, wav_len);
    } else {
        /* Assume raw 16-bit mono PCM */
        err = audio_player_play_pcm((const int16_t *)wav_buf, wav_len);
    }
    free(wav_buf);

    if (err != ESP_OK) {
        printf("Playback failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("TTS playback complete\n");
    return 0;
}

/* --- wake_test command: init wake word and listen for 30s --- */
static void wake_test_cb(void)
{
    printf("\n>>> WAKE WORD DETECTED! <<<\n");
}

static int cmd_wake_test(int argc, char **argv)
{
    static bool inited = false;
    if (!inited) {
        printf("Initializing wake word engine...\n");
        esp_err_t ret = wake_word_init(wake_test_cb);
        if (ret != ESP_OK) {
            printf("Wake word init failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        inited = true;
    }

    printf("Starting wake word detection — say \"Jarvis\"...\n");
    printf("Listening for 30 seconds...\n");
    wake_word_start();

    vTaskDelay(pdMS_TO_TICKS(30000));

    wake_word_stop();
    printf("Wake word test done\n");
    return 0;
}

/* --- voice_status command --- */
static int cmd_voice_status(int argc, char **argv)
{
    const char *state = voice_channel_get_state();
    printf("Voice channel state: %s\n", state);
    
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    printf("Memory: Internal=%d bytes, PSRAM=%d KB\n", 
           (int)internal_free, (int)(psram_free / 1024));
    
    return 0;
}

/* --- voice_test command --- */
static int cmd_voice_test(int argc, char **argv)
{
    printf("=== Voice Channel Test ===\n");
    printf("This test simulates the full voice interaction flow:\n");
    printf("1. Record 3 seconds of audio\n");
    printf("2. Transcribe via STT\n");
    printf("3. Display transcribed text\n");
    printf("4. Synthesize mock response via TTS\n");
    printf("5. Play audio\n\n");
    
    printf("Recording 3 seconds... Speak now!\n");
    
    const int duration_s = 3;
    const size_t samples = MIMI_AUDIO_SAMPLE_RATE * duration_s;
    const size_t mono_bytes = samples * sizeof(int16_t);
    const size_t stereo_bytes = samples * 2 * sizeof(int16_t);
    
    int16_t *mono_buf = heap_caps_malloc(mono_bytes, MALLOC_CAP_SPIRAM);
    int16_t *stereo_buf = heap_caps_malloc(stereo_bytes, MALLOC_CAP_SPIRAM);
    
    if (!mono_buf || !stereo_buf) {
        printf("ERROR: Failed to allocate buffers\n");
        free(mono_buf);
        free(stereo_buf);
        return 1;
    }
    
    esp_codec_dev_handle_t in_dev = audio_hal_get_input_dev();
    size_t recorded = 0;
    
    display_set_state(DISPLAY_STATE_LISTENING);
    
    while (recorded < stereo_bytes) {
        size_t chunk = (stereo_bytes - recorded) > 4096 ? 4096 : (stereo_bytes - recorded);
        int ret = esp_codec_dev_read(in_dev, (uint8_t *)stereo_buf + recorded, chunk);
        if (ret != 0) {
            printf("ERROR: Mic read failed: %d\n", ret);
            break;
        }
        recorded += chunk;
    }
    
    for (size_t i = 0; i < samples; i++) {
        mono_buf[i] = stereo_buf[i * 2];
    }
    
    free(stereo_buf);
    
    printf("Recording complete. Transcribing...\n");
    display_set_state(DISPLAY_STATE_THINKING);
    
    char text[512];
    esp_err_t err = stt_transcribe(mono_buf, mono_bytes, text, sizeof(text));
    free(mono_buf);
    
    if (err != ESP_OK) {
        printf("ERROR: STT failed: %s\n", esp_err_to_name(err));
        display_set_state(DISPLAY_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        display_set_state(DISPLAY_STATE_IDLE);
        return 1;
    }
    
    printf("Transcribed text: \"%s\"\n", text);
    
    const char *mock_response = "Voice channel test successful. Audio playback working.";
    printf("Synthesizing mock response: \"%s\"\n", mock_response);
    
    uint8_t *tts_buf = heap_caps_malloc(MIMI_VOICE_TTS_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!tts_buf) {
        printf("ERROR: Failed to allocate TTS buffer\n");
        display_set_state(DISPLAY_STATE_IDLE);
        return 1;
    }
    
    size_t wav_len = 0;
    err = tts_synthesize(mock_response, tts_buf, MIMI_VOICE_TTS_BUF_SIZE, &wav_len);
    
    if (err != ESP_OK || wav_len == 0) {
        printf("ERROR: TTS failed: %s\n", esp_err_to_name(err));
        free(tts_buf);
        display_set_state(DISPLAY_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(2000));
        display_set_state(DISPLAY_STATE_IDLE);
        return 1;
    }
    
    printf("Playing response (%d bytes)...\n", (int)wav_len);
    display_set_state(DISPLAY_STATE_SPEAKING);
    
    err = audio_player_play_wav(tts_buf, wav_len);
    free(tts_buf);
    
    if (err != ESP_OK) {
        printf("ERROR: Playback failed: %s\n", esp_err_to_name(err));
    }
    
    display_set_state(DISPLAY_STATE_IDLE);
    printf("Voice test complete!\n");
    
    return 0;
}

/* --- restart command --- */
static int cmd_restart(int argc, char **argv)
{
    printf("Restarting...\n");
    esp_restart();
    return 0;  /* unreachable */
}

esp_err_t serial_cli_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "mimi> ";
    repl_config.max_cmdline_length = 256;

    /* USB Serial JTAG */
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));

    /* Register commands */
    esp_console_register_help_command();

    /* wifi_set */
    wifi_set_args.ssid = arg_str1(NULL, NULL, "<ssid>", "WiFi SSID");
    wifi_set_args.password = arg_str1(NULL, NULL, "<password>", "WiFi password");
    wifi_set_args.end = arg_end(2);
    esp_console_cmd_t wifi_set_cmd = {
        .command = "wifi_set",
        .help = "Set WiFi SSID and password",
        .func = &cmd_wifi_set,
        .argtable = &wifi_set_args,
    };
    esp_console_cmd_register(&wifi_set_cmd);

    /* wifi_status */
    esp_console_cmd_t wifi_status_cmd = {
        .command = "wifi_status",
        .help = "Show WiFi connection status",
        .func = &cmd_wifi_status,
    };
    esp_console_cmd_register(&wifi_status_cmd);

    /* wifi_scan */
    esp_console_cmd_t wifi_scan_cmd = {
        .command = "wifi_scan",
        .help = "Scan and list nearby WiFi APs",
        .func = &cmd_wifi_scan,
    };
    esp_console_cmd_register(&wifi_scan_cmd);

    /* set_tg_token */
    tg_token_args.token = arg_str1(NULL, NULL, "<token>", "Telegram bot token");
    tg_token_args.end = arg_end(1);
    esp_console_cmd_t tg_token_cmd = {
        .command = "set_tg_token",
        .help = "Set Telegram bot token",
        .func = &cmd_set_tg_token,
        .argtable = &tg_token_args,
    };
    esp_console_cmd_register(&tg_token_cmd);

    /* set_api_key */
    api_key_args.key = arg_str1(NULL, NULL, "<key>", "LLM API key");
    api_key_args.end = arg_end(1);
    esp_console_cmd_t api_key_cmd = {
        .command = "set_api_key",
        .help = "Set LLM API key",
        .func = &cmd_set_api_key,
        .argtable = &api_key_args,
    };
    esp_console_cmd_register(&api_key_cmd);

    /* set_model */
    model_args.model = arg_str1(NULL, NULL, "<model>", "Model identifier");
    model_args.end = arg_end(1);
    esp_console_cmd_t model_cmd = {
        .command = "set_model",
        .help = "Set LLM model (default: " MIMI_LLM_DEFAULT_MODEL ")",
        .func = &cmd_set_model,
        .argtable = &model_args,
    };
    esp_console_cmd_register(&model_cmd);

    /* set_model_provider */
    provider_args.provider = arg_str1(NULL, NULL, "<provider>", "Model provider (anthropic|openai)");
    provider_args.end = arg_end(1);
    esp_console_cmd_t provider_cmd = {
        .command = "set_model_provider",
        .help = "Set LLM model provider (default: " MIMI_LLM_PROVIDER_DEFAULT ")",
        .func = &cmd_set_model_provider,
        .argtable = &provider_args,
    };
    esp_console_cmd_register(&provider_cmd);

    /* set_api_url */
    api_url_args.url = arg_str1(NULL, NULL, "<url>", "API endpoint URL");
    api_url_args.end = arg_end(1);
    esp_console_cmd_t api_url_cmd = {
        .command = "set_api_url",
        .help = "Set custom API endpoint URL (e.g., LiteLLM proxy)",
        .func = &cmd_set_api_url,
        .argtable = &api_url_args,
    };
    esp_console_cmd_register(&api_url_cmd);

    /* memory_read */
    esp_console_cmd_t mem_read_cmd = {
        .command = "memory_read",
        .help = "Read MEMORY.md",
        .func = &cmd_memory_read,
    };
    esp_console_cmd_register(&mem_read_cmd);

    /* memory_write */
    memory_write_args.content = arg_str1(NULL, NULL, "<content>", "Content to write");
    memory_write_args.end = arg_end(1);
    esp_console_cmd_t mem_write_cmd = {
        .command = "memory_write",
        .help = "Write to MEMORY.md",
        .func = &cmd_memory_write,
        .argtable = &memory_write_args,
    };
    esp_console_cmd_register(&mem_write_cmd);

    /* session_list */
    esp_console_cmd_t sess_list_cmd = {
        .command = "session_list",
        .help = "List all sessions",
        .func = &cmd_session_list,
    };
    esp_console_cmd_register(&sess_list_cmd);

    /* session_clear */
    session_clear_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Chat ID to clear");
    session_clear_args.end = arg_end(1);
    esp_console_cmd_t sess_clear_cmd = {
        .command = "session_clear",
        .help = "Clear a session",
        .func = &cmd_session_clear,
        .argtable = &session_clear_args,
    };
    esp_console_cmd_register(&sess_clear_cmd);

    /* heap_info */
    esp_console_cmd_t heap_cmd = {
        .command = "heap_info",
        .help = "Show heap memory usage",
        .func = &cmd_heap_info,
    };
    esp_console_cmd_register(&heap_cmd);

    /* set_search_key */
    search_key_args.key = arg_str1(NULL, NULL, "<key>", "Brave Search API key");
    search_key_args.end = arg_end(1);
    esp_console_cmd_t search_key_cmd = {
        .command = "set_search_key",
        .help = "Set Brave Search API key for web_search tool",
        .func = &cmd_set_search_key,
        .argtable = &search_key_args,
    };
    esp_console_cmd_register(&search_key_cmd);

    /* set_proxy */
    proxy_args.host = arg_str1(NULL, NULL, "<host>", "Proxy host/IP");
    proxy_args.port = arg_int1(NULL, NULL, "<port>", "Proxy port");
    proxy_args.end = arg_end(2);
    esp_console_cmd_t proxy_cmd = {
        .command = "set_proxy",
        .help = "Set HTTP proxy (e.g. set_proxy 192.168.1.83 7897)",
        .func = &cmd_set_proxy,
        .argtable = &proxy_args,
    };
    esp_console_cmd_register(&proxy_cmd);

    /* clear_proxy */
    esp_console_cmd_t clear_proxy_cmd = {
        .command = "clear_proxy",
        .help = "Remove proxy configuration",
        .func = &cmd_clear_proxy,
    };
    esp_console_cmd_register(&clear_proxy_cmd);

    /* config_show */
    esp_console_cmd_t config_show_cmd = {
        .command = "config_show",
        .help = "Show current configuration (build-time + NVS)",
        .func = &cmd_config_show,
    };
    esp_console_cmd_register(&config_show_cmd);

    /* config_reset */
    esp_console_cmd_t config_reset_cmd = {
        .command = "config_reset",
        .help = "Clear all NVS overrides, revert to build-time defaults",
        .func = &cmd_config_reset,
    };
    esp_console_cmd_register(&config_reset_cmd);

    /* restart */
    esp_console_cmd_t restart_cmd = {
        .command = "restart",
        .help = "Restart the device",
        .func = &cmd_restart,
    };
    esp_console_cmd_register(&restart_cmd);

    /* play_tone */
    play_tone_args.freq = arg_int1(NULL, NULL, "<freq>", "Frequency in Hz");
    play_tone_args.ms = arg_int1(NULL, NULL, "<ms>", "Duration in ms");
    play_tone_args.end = arg_end(2);
    esp_console_cmd_t play_tone_cmd = {
        .command = "play_tone",
        .help = "Play a sine wave tone (e.g. play_tone 440 2000)",
        .func = &cmd_play_tone,
        .argtable = &play_tone_args,
    };
    esp_console_cmd_register(&play_tone_cmd);

    /* audio_test */
    esp_console_cmd_t audio_test_cmd = {
        .command = "audio_test",
        .help = "Run audio debug test (tone + mic loopback)",
        .func = &cmd_audio_test,
    };
    esp_console_cmd_register(&audio_test_cmd);

    /* display_test */
    esp_console_cmd_t display_test_cmd = {
        .command = "display_test",
        .help = "Initialize display and cycle through colors",
        .func = &cmd_display_test,
    };
    esp_console_cmd_register(&display_test_cmd);

    /* button_test */
    esp_console_cmd_t button_test_cmd = {
        .command = "button_test",
        .help = "Initialize buttons and listen for press events",
        .func = &cmd_button_test,
    };
    esp_console_cmd_register(&button_test_cmd);

    /* stt_test */
    stt_test_args.seconds = arg_int1(NULL, NULL, "<seconds>", "Recording duration (1-10)");
    stt_test_args.end = arg_end(1);
    esp_console_cmd_t stt_test_cmd = {
        .command = "stt_test",
        .help = "Record audio and transcribe via faster-whisper (e.g. stt_test 3)",
        .func = &cmd_stt_test,
        .argtable = &stt_test_args,
    };
    esp_console_cmd_register(&stt_test_cmd);

    /* tts_test */
    tts_test_args.text = arg_str1(NULL, NULL, "<text>", "Text to synthesize");
    tts_test_args.end = arg_end(1);
    esp_console_cmd_t tts_test_cmd = {
        .command = "tts_test",
        .help = "Synthesize text to speech and play (e.g. tts_test \"Hello world\")",
        .func = &cmd_tts_test,
        .argtable = &tts_test_args,
    };
    esp_console_cmd_register(&tts_test_cmd);

    /* voice_status */
    esp_console_cmd_t voice_status_cmd = {
        .command = "voice_status",
        .help = "Show voice channel state and memory usage",
        .func = &cmd_voice_status,
    };
    esp_console_cmd_register(&voice_status_cmd);

    /* voice_test */
    esp_console_cmd_t voice_test_cmd = {
        .command = "voice_test",
        .help = "Test full voice interaction flow (record → STT → TTS → play)",
        .func = &cmd_voice_test,
    };
    esp_console_cmd_register(&voice_test_cmd);

    /* wake_test */
    esp_console_cmd_t wake_test_cmd = {
        .command = "wake_test",
        .help = "Listen for wake word \"Jarvis\" for 30 seconds",
        .func = &cmd_wake_test,
    };
    esp_console_cmd_register(&wake_test_cmd);

    /* Start REPL */
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Serial CLI started");

    return ESP_OK;
}
