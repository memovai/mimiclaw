#include "sensors/env_sensor.h"

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu/I2C_Driver.h"

static const char *TAG = "env_sensor";

#define AHT20_I2C_ADDR          0x38
#define AHT20_CMD_STATUS        0x71
#define AHT20_CMD_INIT_0        0xBE
#define AHT20_CMD_INIT_1        0x08
#define AHT20_CMD_INIT_2        0x00
#define AHT20_CMD_MEASURE_0     0xAC
#define AHT20_CMD_MEASURE_1     0x33
#define AHT20_CMD_MEASURE_2     0x00
#define AHT20_STATUS_BUSY_MASK  0x80
#define AHT20_STATUS_CAL_MASK   0x08

static bool s_inited = false;

static esp_err_t aht20_read_status(uint8_t *status)
{
    uint8_t cmd = AHT20_CMD_STATUS;
    return i2c_master_write_read_device(
        I2C_MASTER_NUM, AHT20_I2C_ADDR,
        &cmd, 1, status, 1,
        pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
}

esp_err_t env_sensor_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    uint8_t status = 0;
    esp_err_t err = aht20_read_status(&status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 status read failed: %s", esp_err_to_name(err));
        return err;
    }

    if ((status & AHT20_STATUS_CAL_MASK) == 0) {
        uint8_t init_cmd[3] = {AHT20_CMD_INIT_0, AHT20_CMD_INIT_1, AHT20_CMD_INIT_2};
        err = i2c_master_write_to_device(
            I2C_MASTER_NUM, AHT20_I2C_ADDR,
            init_cmd, sizeof(init_cmd),
            pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AHT20 init command failed: %s", esp_err_to_name(err));
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_inited = true;
    ESP_LOGI(TAG, "AHT20 ready on I2C addr 0x%02X", AHT20_I2C_ADDR);
    return ESP_OK;
}

esp_err_t env_sensor_read(float *temperature_c, float *humidity_pct)
{
    if (!temperature_c || !humidity_pct) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = env_sensor_init();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t measure_cmd[3] = {AHT20_CMD_MEASURE_0, AHT20_CMD_MEASURE_1, AHT20_CMD_MEASURE_2};
    err = i2c_master_write_to_device(
        I2C_MASTER_NUM, AHT20_I2C_ADDR,
        measure_cmd, sizeof(measure_cmd),
        pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t data[6] = {0};
    int retries = 5;
    do {
        vTaskDelay(pdMS_TO_TICKS(20));
        err = i2c_master_read_from_device(
            I2C_MASTER_NUM, AHT20_I2C_ADDR,
            data, sizeof(data),
            pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
        if (err != ESP_OK) {
            return err;
        }
        if ((data[0] & AHT20_STATUS_BUSY_MASK) == 0) {
            break;
        }
    } while (--retries > 0);

    if (data[0] & AHT20_STATUS_BUSY_MASK) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t hum_raw = ((uint32_t)data[1] << 12) |
                       ((uint32_t)data[2] << 4) |
                       ((uint32_t)data[3] >> 4);
    uint32_t temp_raw = (((uint32_t)data[3] & 0x0F) << 16) |
                        ((uint32_t)data[4] << 8) |
                        (uint32_t)data[5];

    *humidity_pct = ((float)hum_raw * 100.0f) / 1048576.0f;
    *temperature_c = ((float)temp_raw * 200.0f) / 1048576.0f - 50.0f;

    return ESP_OK;
}
