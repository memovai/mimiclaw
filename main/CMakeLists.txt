idf_component_register(
    SRCS
        "mimi.c"
        "bus/message_bus.c"
        "wifi/wifi_manager.c"
        "telegram/telegram_bot.c"
        "llm/llm_proxy.c"
        "agent/agent_loop.c"
        "agent/context_builder.c"
        "memory/memory_store.c"
        "memory/session_mgr.c"
        "gateway/ws_server.c"
        "cli/serial_cli.c"
        "ota/ota_manager.c"
        "proxy/http_proxy.c"
        "tools/tool_registry.c"
        "tools/tool_web_search.c"
        "tools/tool_get_time.c"
        "tools/tool_files.c"
        "audio/audio_hal.c"
        "audio/audio_player.c"
        "audio/test_audio.c"
        "display/display_driver.c"
        "input/button_input.c"
        "voice/stt_client.c"
        "voice/tts_client.c"
        "voice/voice_channel.c"
        "voice/mp3_encoder.c"
        "voice/mp3_decoder.c"
        "wake/wake_word.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_http_client esp_http_server
        esp_https_ota esp_event json spiffs console vfs app_update esp-tls
        esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_lcd esp_driver_ledc
        shine libhelix esp_timer
)
