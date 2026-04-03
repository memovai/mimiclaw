#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/// WiFi 已拿到 IP，可继续访问外部 API
#define WIFI_CONNECTED_BIT  BIT0

/// WiFi 达到最大重试次数，当前连接流程判定失败
#define WIFI_FAIL_BIT       BIT1

/**
 * @brief 初始化 WiFi STA 驱动和事件处理器
 *
 * 该函数只完成驱动级初始化，不会立刻开始连接路由器。
 * 必须在 wifi_manager_start() 之前调用。
 *
 * @return ESP_OK          初始化成功
 * @return ESP_ERR_NO_MEM  事件组创建失败
 *
 * @note 依赖默认事件循环已创建
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 启动 STA 模式并使用配置头中的账号密码发起连接
 *
 * 连接参数直接来自 mvp_config.h。真正的首次连接动作会在
 * WIFI_EVENT_STA_START 事件进入回调后触发。
 *
 * @return ESP_OK               启动成功
 * @return ESP_ERR_INVALID_ARG  SSID 为空
 *
 * @note 调用前必须先执行 wifi_manager_init()
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief 阻塞等待 WiFi 连接结果
 *
 * 当设备拿到 IP 时返回成功；如果在等待期内超时，或连接过程
 * 已被事件回调判定为失败，则返回超时错误。
 *
 * @param[in] timeout_ms 等待超时，单位毫秒；传 UINT32_MAX 表示无限等待
 *
 * @return ESP_OK          已成功连接
 * @return ESP_ERR_TIMEOUT 超时或连接失败
 */
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms);

/**
 * @brief 查询当前是否处于已连接状态
 *
 * @return true  已拿到 IP
 * @return false 未连接或已断线
 */
bool wifi_manager_is_connected(void);

/**
 * @brief 获取当前保存的 IPv4 字符串
 *
 * 连接成功前返回默认值 "0.0.0.0"。
 *
 * @return 内部静态字符串指针
 *
 * @warning 返回值指向模块内部缓冲区，调用者不要修改
 */
const char *wifi_manager_get_ip(void);
