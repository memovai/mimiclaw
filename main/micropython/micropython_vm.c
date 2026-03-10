#include "micropython_vm.h"
#include "mimi_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "micropython_embed.h"

static const char *TAG = "upy_vm";

static SemaphoreHandle_t s_mutex = NULL;
static esp_timer_handle_t s_timeout_timer = NULL;

/* Shared stdout capture state (protected by mutex — only one exec at a time) */
static char  *s_stdout_buf  = NULL;
static size_t s_stdout_pos  = 0;
static size_t s_stdout_size = 0;

/* Timeout flag — set by timer callback, checked by VM hook in mpconfigport.h */
volatile int micropython_vm_timeout_flag = 0;

/* ---------- MicroPython HAL implementations ---------- */

/* Called by MicroPython for all print() / stdout output */
void mp_hal_stdout_tx_strn(const char *str, mp_uint_t len)
{
    if (!s_stdout_buf) return;
    size_t avail = (s_stdout_size > s_stdout_pos + 1)
                   ? s_stdout_size - s_stdout_pos - 1
                   : 0;
    size_t copy = (len < avail) ? len : avail;
    if (copy > 0) {
        memcpy(s_stdout_buf + s_stdout_pos, str, copy);
        s_stdout_pos += copy;
        s_stdout_buf[s_stdout_pos] = '\0';
    }
}

/* Called by MicroPython for time.sleep_ms() / delays */
void mp_hal_delay_ms(mp_uint_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ---------- Timeout timer ---------- */

static void timeout_callback(void *arg)
{
    micropython_vm_timeout_flag = 1;
}

/* ---------- Public API ---------- */

esp_err_t micropython_vm_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    esp_timer_create_args_t timer_args = {
        .callback = timeout_callback,
        .name = "upy_timeout",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_timeout_timer);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return err;
    }

    ESP_LOGI(TAG, "MicroPython VM ready (heap=%dKB, timeout=%dms)",
             MIMI_MICROPYTHON_HEAP_SIZE / 1024, MIMI_MICROPYTHON_TIMEOUT_MS);
    return ESP_OK;
}

esp_err_t micropython_vm_exec(const char *code, char *output, size_t output_size,
                               int timeout_ms)
{
    if (!s_mutex || !code || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Only one script at a time */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        snprintf(output, output_size, "Error: MicroPython VM is busy");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;

    /* Allocate GC heap from PSRAM */
    void *gc_heap = heap_caps_calloc(1, MIMI_MICROPYTHON_HEAP_SIZE, MALLOC_CAP_SPIRAM);
    if (!gc_heap) {
        snprintf(output, output_size, "Error: failed to allocate MicroPython heap (%dKB)",
                 MIMI_MICROPYTHON_HEAP_SIZE / 1024);
        ret = ESP_ERR_NO_MEM;
        goto unlock;
    }

    /* Setup stdout capture */
    s_stdout_buf  = output;
    s_stdout_pos  = 0;
    s_stdout_size = output_size;
    output[0]     = '\0';

    /* Reset timeout flag */
    micropython_vm_timeout_flag = 0;

    /* Init interpreter */
    mp_embed_init(gc_heap, MIMI_MICROPYTHON_HEAP_SIZE);

    /* Start timeout timer */
    if (timeout_ms > 0) {
        esp_timer_start_once(s_timeout_timer, (uint64_t)timeout_ms * 1000);
    }

    /* Execute — exceptions are printed to stdout via mp_hal_stdout_tx_strn */
    mp_embed_exec_str(code);

    /* Cancel timeout */
    esp_timer_stop(s_timeout_timer);
    micropython_vm_timeout_flag = 0;

    /* Teardown interpreter */
    mp_embed_deinit();

    /* Free heap */
    heap_caps_free(gc_heap);

    /* Detach stdout capture */
    s_stdout_buf  = NULL;
    s_stdout_size = 0;

    /* If no output was produced, say so */
    if (s_stdout_pos == 0) {
        snprintf(output, output_size, "(no output — use print() to see results)");
    }

    ESP_LOGI(TAG, "Python exec done (%d bytes output)", (int)s_stdout_pos);

unlock:
    xSemaphoreGive(s_mutex);
    return ret;
}
