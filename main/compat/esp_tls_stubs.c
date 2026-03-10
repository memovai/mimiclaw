#include "esp_err.h"
#include "esp_transport_ssl.h"
#include "esp_tls.h"

// Weak stubs for builds where certificate bundle/ALPN are disabled.
// These allow linking but provide no functionality.

__attribute__((weak)) esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_ERR_NOT_SUPPORTED;
}

__attribute__((weak)) void esp_transport_ssl_set_alpn_protocol(esp_transport_handle_t t, const char **alpn_protos)
{
    (void)t;
    (void)alpn_protos;
}
