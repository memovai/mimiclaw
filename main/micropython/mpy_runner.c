#include "mpy_runner.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "py/stackctrl.h"
#include "py/nlr.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/parse.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "shared/runtime/pyexec.h"

static const char *TAG = "mpy";

static SemaphoreHandle_t s_mpy_lock = NULL;

esp_err_t mpy_runner_init(void)
{
    if (s_mpy_lock) return ESP_OK;
    s_mpy_lock = xSemaphoreCreateMutex();
    return s_mpy_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

static void mpy_setup_vm(void *heap, size_t heap_size)
{
    void *sp = &heap;
    mp_stack_set_top(sp);
    mp_stack_set_limit(MIMI_MPY_TASK_STACK - 1024);

    gc_init(heap, (uint8_t *)heap + heap_size);
    mp_init();
}

static void mpy_teardown_vm(void)
{
    mp_deinit();
}

static esp_err_t mpy_exec_buffer(const char *code, size_t len)
{
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, code, len, 0);
        mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t module_fun = mp_compile(&parse_tree, MP_QSTR__lt_stdin_gt_, false);
        mp_call_function_0(module_fun);
        nlr_pop();
        return ESP_OK;
    }

    // Exception
    mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    return ESP_FAIL;
}

esp_err_t mpy_exec_code(const char *code, size_t len)
{
    if (!code || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_mpy_lock) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_mpy_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    void *heap = heap_caps_malloc(MIMI_MPY_HEAP_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!heap) {
        heap = heap_caps_malloc(MIMI_MPY_HEAP_SIZE, MALLOC_CAP_8BIT);
    }
    if (!heap) {
        xSemaphoreGive(s_mpy_lock);
        return ESP_ERR_NO_MEM;
    }

    mpy_setup_vm(heap, MIMI_MPY_HEAP_SIZE);
    esp_err_t err = mpy_exec_buffer(code, len);
    mpy_teardown_vm();
    heap_caps_free(heap);

    xSemaphoreGive(s_mpy_lock);
    return err;
}

esp_err_t mpy_exec_file(const char *path)
{
    if (!path || path[0] == '\0') return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open script: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t size = (size_t)st.st_size;
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    buf[n] = '\0';

    esp_err_t err = mpy_exec_code(buf, n);
    free(buf);
    return err;
}
