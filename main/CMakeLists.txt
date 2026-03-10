set(MIMI_MAIN_SRCS
    "mimi.c"
    "bus/message_bus.c"
    "wifi/wifi_manager.c"
    "channels/telegram/telegram_bot.c"
    "channels/feishu/feishu_bot.c"
    "llm/llm_proxy.c"
    "agent/agent_loop.c"
    "agent/context_builder.c"
    "memory/memory_store.c"
    "memory/session_mgr.c"
    "gateway/ws_server.c"
    "compat/esp_tls_stubs.c"
    "cli/serial_cli.c"
    "proxy/http_proxy.c"
    "cron/cron_service.c"
    "heartbeat/heartbeat.c"
    "tools/tool_registry.c"
    "tools/tool_cron.c"
    "tools/tool_web_search.c"
    "tools/tool_get_time.c"
    "tools/tool_files.c"
    "skills/skill_loader.c"
)

set(MIMI_MAIN_INCLUDE_DIRS "." "compat")

set(MIMI_MAIN_REQUIRES
    nvs_flash esp_wifi esp_netif esp_http_client esp_http_server
    esp_https_ota esp_event json spiffs console vfs app_update esp-tls
    esp_timer esp_websocket_client tcp_transport
)

if(MIMI_ENABLE_MICROPYTHON)
    list(APPEND MIMI_MAIN_SRCS
        "micropython/mpy_runner.c"
        "micropython/mpy_stubs.c"
        "micropython/mpy_uart_stubs.c"
    )
    list(APPEND MIMI_MAIN_INCLUDE_DIRS
        "micropython"
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_SOURCE_DIR}/third_party/micropython-helper/micropython"
        "${CMAKE_SOURCE_DIR}/third_party/micropython-helper/micropython/ports/esp32"
        "${CMAKE_SOURCE_DIR}/third_party/micropython-helper/micropython/ports/esp32/boards/${MICROPY_BOARD}"
    )
    list(APPEND MIMI_MAIN_REQUIRES esp_driver_gpio esp_driver_i2s)
endif()

idf_component_register(
    SRCS ${MIMI_MAIN_SRCS}
    INCLUDE_DIRS ${MIMI_MAIN_INCLUDE_DIRS}
    REQUIRES ${MIMI_MAIN_REQUIRES}
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
target_include_directories(${COMPONENT_LIB} PRIVATE ${IDF_PATH}/components/mbedtls/esp_crt_bundle/include)
if(MIMI_ENABLE_MICROPYTHON)
    if(DEFINED MICROPY_GENHDR_DIR)
        get_filename_component(_mimi_genhdr_parent "${MICROPY_GENHDR_DIR}" DIRECTORY)
        target_include_directories(${COMPONENT_LIB} PRIVATE "${_mimi_genhdr_parent}")
    endif()
    if(DEFINED MICROPY_TARGET)
        add_dependencies(${COMPONENT_LIB} ${MICROPY_TARGET})
    endif()
endif()
