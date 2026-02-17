// 檔案路徑：main/tools/tool_camera.h
#ifndef TOOL_CAMERA_H
#define TOOL_CAMERA_H

#include "esp_err.h"
#include "esp_camera.h"
#include <stddef.h>

/**
 * @brief implementation of taking photo. Call by agent loop
 */
esp_err_t tool_take_photo_execute(const char *input_json, char *output, size_t output_size);

/**
 * @brief retreive the last photo pointer call by mimi.c
 */
camera_fb_t* tool_camera_get_last_fb(void);

/**
 * @brief relase buffer
 */
void tool_camera_clear_last_fb(void);

#endif // TOOL_CAMERA_H