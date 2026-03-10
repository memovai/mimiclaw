#include "http_proxy.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include "esp_log.h"
#include "nvs.h"
#include "esp_tls.h"
#include "compat/mbedtls_compat.h"

static const char *TAG = "proxy";

/* Only show warnings/errors by default; reduce polling noise */
__attribute__((constructor)) static void proxy_log_level(void)
{
    esp_log_level_set(TAG, ESP_LOG_WARN);
}

static char     s_proxy_host[64] = {0};
static uint16_t s_proxy_port     = 0;
static char     s_proxy_type[8] = "http"; // "http" or "socks5"

esp_err_t http_proxy_init(void)
{
    /* Start with build-time defaults */
    if (MIMI_SECRET_PROXY_HOST[0] != '\0' && MIMI_SECRET_PROXY_PORT[0] != '\0') {
        strncpy(s_proxy_host, MIMI_SECRET_PROXY_HOST, sizeof(s_proxy_host) - 1);
        s_proxy_port = (uint16_t)atoi(MIMI_SECRET_PROXY_PORT);
        if (MIMI_SECRET_PROXY_TYPE[0] != '\0') {
            strncpy(s_proxy_type, MIMI_SECRET_PROXY_TYPE, sizeof(s_proxy_type) - 1);
        }
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_PROXY, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[64] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_PROXY_HOST, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_proxy_host, tmp, sizeof(s_proxy_host) - 1);
            uint16_t port = 0;
            if (nvs_get_u16(nvs, MIMI_NVS_KEY_PROXY_PORT, &port) == ESP_OK && port) {
                s_proxy_port = port;
            }
            // Read proxy type from NVS
            len = sizeof(tmp);
            memset(tmp, 0, sizeof(tmp));
            if (nvs_get_str(nvs, "proxy_type", tmp, &len) == ESP_OK && tmp[0]) {
                strncpy(s_proxy_type, tmp, sizeof(s_proxy_type) - 1);
            }
        }
        nvs_close(nvs);
    }

    if (s_proxy_host[0] && s_proxy_port) {
        ESP_LOGI(TAG, "Proxy configured: %s:%d (%s)", s_proxy_host, s_proxy_port, s_proxy_type);
    } else {
        ESP_LOGI(TAG, "No proxy configured (direct connection)");
    }
    return ESP_OK;
}

esp_err_t http_proxy_set(const char *host, uint16_t port, const char *type)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_PROXY, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_PROXY_HOST, host));
    ESP_ERROR_CHECK(nvs_set_u16(nvs, MIMI_NVS_KEY_PROXY_PORT, port));
    ESP_ERROR_CHECK(nvs_set_str(nvs, "proxy_type", type));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_proxy_host, host, sizeof(s_proxy_host) - 1);
    s_proxy_port = port;
    strncpy(s_proxy_type, type, sizeof(s_proxy_type) - 1);
    ESP_LOGI(TAG, "Proxy set to %s:%d (%s)", s_proxy_host, s_proxy_port, s_proxy_type);
    return ESP_OK;
}

esp_err_t http_proxy_clear(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_PROXY, NVS_READWRITE, &nvs));
    nvs_erase_key(nvs, MIMI_NVS_KEY_PROXY_HOST);
    nvs_erase_key(nvs, MIMI_NVS_KEY_PROXY_PORT);
    nvs_erase_key(nvs, "proxy_type");
    nvs_commit(nvs);
    nvs_close(nvs);

    s_proxy_host[0] = '\0';
    s_proxy_port = 0;
    strcpy(s_proxy_type, "http"); // Reset to default
    ESP_LOGI(TAG, "Proxy cleared");
    return ESP_OK;
}

bool http_proxy_is_enabled(void)
{
    return s_proxy_host[0] != '\0' && s_proxy_port != 0;
}

/* ── Proxied TLS connection ───────────────────────────────────── */

struct proxy_conn {
    int         sock;   /* raw TCP socket (for timeout control) */
    esp_tls_t  *tls;    /* esp_tls handle owns TLS + socket lifecycle */
};

/* Read a line from socket (up to CR-LF). Returns length or -1. */
static int sock_read_line(int fd, char *buf, int max, int timeout_ms)
{
    int pos = 0;
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (pos < max - 1) {
        char c;
        int r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') { buf[pos] = '\0'; return pos; }
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

/* Open TCP + CONNECT tunnel for HTTP proxy, returns socket fd or -1 */
static int open_connect_tunnel(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", s_proxy_port);

    if (getaddrinfo(s_proxy_host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for proxy %s", s_proxy_host);
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { freeaddrinfo(res); return -1; }

    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "TCP connect to proxy %s:%d failed", s_proxy_host, s_proxy_port);
        freeaddrinfo(res); close(sock); return -1;
    }
    freeaddrinfo(res);
    ESP_LOGI(TAG, "Connected to proxy %s:%d", s_proxy_host, s_proxy_port);

    char req[256];
    int len = snprintf(req, sizeof(req),
        "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\n\r\n", host, port, host, port);

    if (send(sock, req, len, 0) != len) {
        ESP_LOGE(TAG, "Failed to send CONNECT"); close(sock); return -1;
    }

    char line[256];
    if (sock_read_line(sock, line, sizeof(line), timeout_ms) < 0) {
        ESP_LOGE(TAG, "No response from proxy"); close(sock); return -1;
    }
    if (strstr(line, "200") == NULL) {
        ESP_LOGE(TAG, "CONNECT rejected: %s", line); close(sock); return -1;
    }

    /* Consume remaining response headers */
    while (sock_read_line(sock, line, sizeof(line), timeout_ms) > 0) { }

    ESP_LOGI(TAG, "CONNECT tunnel established to %s:%d", host, port);
    return sock;
}

/* Open TCP + SOCKS5 tunnel, returns socket fd or -1 */
static int open_socks5_tunnel(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", s_proxy_port);

    if (getaddrinfo(s_proxy_host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolve failed for proxy %s", s_proxy_host);
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { freeaddrinfo(res); return -1; }

    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "TCP connect to proxy %s:%d failed", s_proxy_host, s_proxy_port);
        freeaddrinfo(res); close(sock); return -1;
    }
    freeaddrinfo(res);
    ESP_LOGI(TAG, "Connected to SOCKS5 proxy %s:%d", s_proxy_host, s_proxy_port);

    /* SOCKS5 handshake: version 5, no authentication */
    unsigned char handshake[3] = { 0x05, 0x01, 0x00 };
    if (send(sock, handshake, sizeof(handshake), 0) != sizeof(handshake)) {
        ESP_LOGE(TAG, "Failed to send SOCKS5 handshake"); close(sock); return -1;
    }

    /* Receive handshake response */
    unsigned char handshake_resp[2];
    if (recv(sock, handshake_resp, sizeof(handshake_resp), 0) != sizeof(handshake_resp)) {
        ESP_LOGE(TAG, "No response from SOCKS5 proxy"); close(sock); return -1;
    }

    if (handshake_resp[0] != 0x05 || handshake_resp[1] != 0x00) {
        ESP_LOGE(TAG, "SOCKS5 handshake failed: version=%d, auth=%d", handshake_resp[0], handshake_resp[1]);
        close(sock); return -1;
    }

    /* SOCKS5 connect request: version 5, connect command, reserved, domain name address type */
    size_t host_len = strlen(host);
    size_t req_len = 4 + 1 + host_len + 2;
    unsigned char *req = malloc(req_len);
    if (!req) { close(sock); return -1; }

    req[0] = 0x05; /* version */
    req[1] = 0x01; /* connect command */
    req[2] = 0x00; /* reserved */
    req[3] = 0x03; /* domain name address type */
    req[4] = (unsigned char)host_len; /* domain name length */
    memcpy(req + 5, host, host_len); /* domain name */
    req[5 + host_len] = (unsigned char)(port >> 8); /* port high byte */
    req[6 + host_len] = (unsigned char)(port & 0xFF); /* port low byte */

    if (send(sock, req, req_len, 0) != (ssize_t)req_len) {
        ESP_LOGE(TAG, "Failed to send SOCKS5 connect request");
        free(req); close(sock); return -1;
    }
    free(req);

    /* Receive connect response */
    unsigned char resp[256];
    int resp_len = recv(sock, resp, sizeof(resp), 0);
    if (resp_len < 10) {
        ESP_LOGE(TAG, "No response from SOCKS5 proxy"); close(sock); return -1;
    }

    if (resp[0] != 0x05 || resp[1] != 0x00) {
        ESP_LOGE(TAG, "SOCKS5 connect failed: version=%d, status=%d", resp[0], resp[1]);
        close(sock); return -1;
    }

    ESP_LOGI(TAG, "SOCKS5 tunnel established to %s:%d", host, port);
    return sock;
}

proxy_conn_t *proxy_conn_open(const char *host, int port, int timeout_ms)
{
    if (!http_proxy_is_enabled()) {
        ESP_LOGE(TAG, "proxy_conn_open called but no proxy configured");
        return NULL;
    }

    int sock;
    if (strcmp(s_proxy_type, "socks5") == 0) {
        sock = open_socks5_tunnel(host, port, timeout_ms);
    } else {
        sock = open_connect_tunnel(host, port, timeout_ms);
    }
    if (sock < 0) return NULL;

    proxy_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) { close(sock); return NULL; }
    conn->sock = sock;

    /* ── TLS handshake via esp_tls over tunnel ───────────────── */
    conn->tls = esp_tls_init();
    if (!conn->tls) {
        ESP_LOGE(TAG, "esp_tls_init failed");
        close(sock); free(conn); return NULL;
    }

    /* Inject our CONNECT-tunnel socket and skip TCP connect phase */
    esp_tls_set_conn_sockfd(conn->tls, sock);
    esp_tls_set_conn_state(conn->tls, ESP_TLS_CONNECTING);

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = MIMI_CRT_BUNDLE_ATTACH,
        .timeout_ms = timeout_ms,
    };

    int ret = esp_tls_conn_new_sync(host, strlen(host), port, &cfg, conn->tls);
    if (ret <= 0) {
        ESP_LOGE(TAG, "TLS handshake failed over proxy tunnel");
        esp_tls_conn_destroy(conn->tls);
        /* esp_tls_conn_destroy closes the socket */
        free(conn);
        return NULL;
    }

    ESP_LOGI(TAG, "TLS handshake OK with %s:%d via proxy", host, port);
    return conn;
}

int proxy_conn_write(proxy_conn_t *conn, const char *data, int len)
{
    int written = 0;
    while (written < len) {
        ssize_t ret = esp_tls_conn_write(conn->tls, data + written, len - written);
        if (ret > 0) {
            written += (int)ret;
        } else if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        } else {
            ESP_LOGE(TAG, "esp_tls_conn_write error: %d", (int)ret);
            return -1;
        }
    }
    return written;
}

int proxy_conn_read(proxy_conn_t *conn, char *buf, int len, int timeout_ms)
{
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(conn->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t ret = esp_tls_conn_read(conn->tls, buf, len);
    if (ret == ESP_TLS_ERR_SSL_WANT_READ) return 0;
    if (ret == 0) return 0;
    if (ret < 0) {
        ESP_LOGE(TAG, "esp_tls_conn_read error: %d", (int)ret);
        return -1;
    }
    return (int)ret;
}

void proxy_conn_close(proxy_conn_t *conn)
{
    if (!conn) return;
    if (conn->tls) {
        esp_tls_conn_destroy(conn->tls);
    }
    free(conn);
}
