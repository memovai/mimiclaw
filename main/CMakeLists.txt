idf_component_register(
    SRCS
        "mimi.c"
        "wifi/wifi_manager.c"
        "llm/llm_proxy.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_http_client esp_event json esp-tls
)
