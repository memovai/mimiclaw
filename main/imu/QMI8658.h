#pragma once

#include "I2C_Driver.h"
#include "esp_err.h"

//device address
#define QMI8658_L_SLAVE_ADDRESS                 (0x6B)
#define QMI8658_H_SLAVE_ADDRESS                 (0x6A)

#define QMI8658_WHO_AM_I 0x00 // devide identifier
#define QMI8658_REVISION_ID 0x01
#define QMI8658_CTRL1  0x02 // SPI interface and sensor enable
#define QMI8658_CTRL2  0x03 // Accelerometer settings
#define QMI8658_CTRL3  0x04 // Gyro settings
#define QMI8658_CTRL4  0x05 // reserved (we don't use this)
#define QMI8658_CTRL5  0x06 // Low-pass filter settings
#define QMI8658_CTRL6  0x07 // AttitudeEngine settings (we don't use these)
#define QMI8658_CTRL7  0x08 // Sensor enable
#define QMI8658_CTRL8  0x09 // Motion detection control (not in current lib version)
#define QMI8658_CTRL9  0x0A // Host commands (not in current lib version)

#define QMI8658_CAL1_L  0x0B  // calibration 1 register, lower bits
#define QMI8658_CAL1_H  0x0C  // calibration 1 register, higher bits
#define QMI8658_CAL2_L  0x0D  // calibration 2 register, lower bits
#define QMI8658_CAL2_H  0x0E  // calibration 2 register, higher bits
#define QMI8658_CAL3_L  0x0F  // calibration 3 register, lower bits
#define QMI8658_CAL3_H  0x10  // calibration 3 register, higher bits
#define QMI8658_CAL4_L  0x11  // calibration 4 register, lower bits
#define QMI8658_CAL4_H  0x12  // calibration 4 register, higher bits

#define QMI8658_TEMP_L 0x33 // lower bits of temperature data
#define QMI8658_TEMP_H 0x34 // upper bits of temperature data

#define QMI8658_STATUSINT 0x2D // status + interrupt register

#define QMI8658_AX_L 0x35 // lower bits of x-axis acceleration
#define QMI8658_AX_H 0x36 // upper bits of x-axis acceleration
#define QMI8658_AY_L 0x37 // lower bits of y-axis acceleration
#define QMI8658_AY_H 0x38 // upper bits of y-axis acceleration
#define QMI8658_AZ_L 0x39
#define QMI8658_AZ_H 0x3A
#define QMI8658_GX_L 0x3B // lower bits of x-axis angular velocity
#define QMI8658_GX_H 0x3C // upper bits of x-axis angular velocity
#define QMI8658_GY_L 0x3D
#define QMI8658_GY_H 0x3E
#define QMI8658_GZ_L 0x3F
#define QMI8658_GZ_H 0x40

#define QMI8658_AODR_MASK 0x0F // bits in acc data rate are 1, rest are 0 (CTRL2)
#define QMI8658_GODR_MASK 0x0F // bits in gyro data rate are 1, rest are 0 (CTRL3)
#define QMI8658_ASCALE_MASK 0x70 // bits in acc scale are 1, rest are 0
#define QMI8658_GSCALE_MASK 0x70 // bits in gyro scale are 1, rest are 0
#define QMI8658_ALPF_MASK 0x06 // bits in acc low pass filter setting
#define QMI8658_GLPF_MASK 0x60 // bits in gyro low pass filter setting
#define QMI8658_ASCALE_OFFSET 4 // offset to acc scale bits
#define QMI8658_GSCALE_OFFSET 4 // offset to gyro scale bits
#define QMI8658_ALPF_OFFSET 1 // offset to acc low pass filter bits
#define QMI8658_GLPF_OFFSET 5 // offset to gyro low pass filter bits

#define QMI8658_COMM_TIMEOUT 50 // communication timeout, in ms


// delay between refreshes of sensor data in us
// applies to individual sensor readings while in locking mode
// has no effect in running mode
#define QMI8658_REFRESH_DELAY 2000

// control clock gating (necessary to use data locking)
#define QMI8658_CTRL_CMD_AHB_CLOCK_GATING 0x12


typedef enum {
    acc_odr_norm_8000 = 0x0,
    acc_odr_norm_4000,
    acc_odr_norm_2000,
    acc_odr_norm_1000,
    acc_odr_norm_500,
    acc_odr_norm_250,
    acc_odr_norm_120,
    acc_odr_norm_60,
    acc_odr_norm_30,
    acc_odr_lp_128 = 0xC,
    acc_odr_lp_21,
    acc_odr_lp_11,
    acc_odr_lp_3,
} acc_odr_t;

typedef enum {
    gyro_odr_norm_8000 = 0x0,
    gyro_odr_norm_4000,
    gyro_odr_norm_2000,
    gyro_odr_norm_1000,
    gyro_odr_norm_500,
    gyro_odr_norm_250,
    gyro_odr_norm_120,
    gyro_odr_norm_60,
    gyro_odr_norm_30
} gyro_odr_t;

typedef enum {
    ACC_RANGE_2G = 0x0,
    ACC_RANGE_4G,
    ACC_RANGE_8G,
    ACC_RANGE_16G
} acc_scale_t;

typedef enum {
    GYR_RANGE_16DPS = 0x0,
    GYR_RANGE_32DPS,
    GYR_RANGE_64DPS,
    GYR_RANGE_128DPS,
    GYR_RANGE_256DPS,
    GYR_RANGE_512DPS,
    GYR_RANGE_1024DPS
} gyro_scale_t;

typedef enum {
    LPF_MODE_0 = 0x0,     //2.66% of ODR
    LPF_MODE_1 = 0x2,     //3.63% of ODR
    LPF_MODE_2 = 0x4,     //5.39% of ODR
    LPF_MODE_3 = 0x6     //13.37% of ODR
} lpf_t;

typedef enum {
    sensor_default,  
    sensor_power_down,
    sensor_running, 
    sensor_locking  
} sensor_state_t;

typedef struct __IMUdata {
    float x;
    float y;
    float z;
} IMUdata;

extern IMUdata Accel;
extern IMUdata Gyro;

void QMI8658_Init(void);
void QMI8658_Loop(void);
void QMI8658_transmit(uint8_t addr, uint8_t data);
uint8_t QMI8658_receive(uint8_t addr);
void QMI8658_CTRL9_Write(uint8_t command);
void QMI8658_sensor_update();
void QMI8658_update_if_needed();
void setAccODR(acc_odr_t odr);
void setGyroODR(gyro_odr_t odr);
void setAccScale(acc_scale_t scale);
void setGyroScale(gyro_scale_t scale);
void setAccLPF(lpf_t lpf);
void setGyroLPF(lpf_t lpf);
void setState(sensor_state_t state);
void getRawReadings(int16_t* buf);
float getAccX();
float getAccY();
float getAccZ();
float getGyroX();
float getGyroY();
float getGyroZ();
void getAccelerometer(void);
void getGyroscope(void);
esp_err_t QMI8658_Read_Temperature(float *temp_c);
