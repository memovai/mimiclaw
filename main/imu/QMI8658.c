#include "QMI8658.h"

IMUdata Accel;
IMUdata Gyro;

uint8_t Device_addr ; // default for SD0/SA0 low, 0x6A if high
acc_scale_t acc_scale = ACC_RANGE_4G;
gyro_scale_t gyro_scale = GYR_RANGE_64DPS;
acc_odr_t acc_odr = acc_odr_norm_8000;
gyro_odr_t gyro_odr = gyro_odr_norm_8000;
sensor_state_t sensor_state = sensor_default;
lpf_t acc_lpf;

float accelScales, gyroScales;
float accelScales = 0;
uint8_t readings[12];
uint32_t reading_timestamp_us; // timestamp in arduino micros() time
/**
 * Inialize Wire and send default configs
 * @param addr I2C address of sensor, typically 0x6A or 0x6B
 */
void QMI8658_Init(void)
{
    uint8_t buf[1];
    Device_addr = QMI8658_L_SLAVE_ADDRESS;     
    I2C_Read(Device_addr, QMI8658_REVISION_ID, buf, 1);
    printf("QMI8658 Device ID: %x\r\n",buf[0]);    // Get chip id
    setState(sensor_running);             

    setAccScale(acc_scale);            
    setAccODR(acc_odr);                    
    setAccLPF(LPF_MODE_0);                  
    switch (acc_scale) {                
        // Possible accelerometer scales (and their register bit settings) are:
        // 2 Gs (00), 4 Gs (01), 8 Gs (10), and 16 Gs  (11).
        // Here's a bit of an algorith to calculate DPS/(ADC tick) based on that
        // 2-bit value:
        case ACC_RANGE_2G:  accelScales = 2.0 / 32768.0; break;
        case ACC_RANGE_4G:  accelScales = 4.0 / 32768.0; break;
        case ACC_RANGE_8G:  accelScales = 8.0 / 32768.0; break;
        case ACC_RANGE_16G: accelScales = 16.0 / 32768.0; break;
    }

    setGyroScale(gyro_scale);              
    setGyroODR(gyro_odr);                       
    setGyroLPF(LPF_MODE_3);                
    switch (gyro_scale) {                  
        // Possible gyro scales (and their register bit settings) are:
        // 250 DPS (00), 500 DPS (01), 1000 DPS (10), and 2000 DPS  (11).
        // Here's a bit of an algorith to calculate DPS/(ADC tick) based on that
        // 2-bit value:
        case GYR_RANGE_16DPS: gyroScales = 16.0 / 32768.0; break;
        case GYR_RANGE_32DPS: gyroScales = 32.0 / 32768.0; break;
        case GYR_RANGE_64DPS: gyroScales = 64.0 / 32768.0; break;
        case GYR_RANGE_128DPS: gyroScales = 128.0 / 32768.0; break;
        case GYR_RANGE_256DPS: gyroScales = 256.0 / 32768.0; break;
        case GYR_RANGE_512DPS: gyroScales = 512.0 / 32768.0; break;
        case GYR_RANGE_1024DPS: gyroScales = 1024.0 / 32768.0; break;
    }
}
void QMI8658_Loop(void)
{
  getAccelerometer();
}

/**
 * Transmit one uint8_t of data to QMI8658.
 * @param addr address of data to be written
 * @param data the data to be written
 */
void QMI8658_transmit(uint8_t addr, uint8_t data)
{
    I2C_Write(Device_addr, addr, &data, 1);
}

/**
 * Receive one uint8_t of data from QMI8658.
 * @param addr address of data to be read
 * @return the uint8_t of data that was read
 */
uint8_t QMI8658_receive(uint8_t addr)
{
    uint8_t retval;
    I2C_Read(Device_addr, addr, &retval, 1);
    return retval;
}

/**
 * Writes data to CTRL9 (command register) and waits for ACK.
 * @param command the command to be executed
 */
void QMI8658_CTRL9_Write(uint8_t command)
{
    // transmit command
    QMI8658_transmit(QMI8658_CTRL9, command);

    // wait for command to be done
    while (((QMI8658_receive(QMI8658_STATUSINT)) & 0x80) == 0x00);
}

/**
 * Set output data rate (ODR) of accelerometer.
 * @param odr acc_odr_t variable representing new data rate
 */
void setAccODR(acc_odr_t odr)
{
    if (sensor_state != sensor_default)                     // If the device is not in the default state
    {
        uint8_t ctrl2 = QMI8658_receive(QMI8658_CTRL2);
        ctrl2 &= ~QMI8658_AODR_MASK;                        // clear previous setting
        ctrl2 |= odr;                                       // OR in new setting
        QMI8658_transmit(QMI8658_CTRL2, ctrl2);
    }
    acc_odr = odr;
}

/**
 * Set output data rate (ODR) of gyro.
 * @param odr gyro_odr_t variable representing new data rate
 */
void setGyroODR(gyro_odr_t odr)
{
    if (sensor_state != sensor_default)
    {
    uint8_t ctrl3 = QMI8658_receive(QMI8658_CTRL3);
    ctrl3 &= ~QMI8658_GODR_MASK; // clear previous setting
    ctrl3 |= odr; // OR in new setting
    QMI8658_transmit(QMI8658_CTRL3, ctrl3);
    }
    gyro_odr = odr;
}

/**
 * Set scale of accelerometer output.
 * @param scale acc_scale_t variable representing new scale
 */
void setAccScale(acc_scale_t scale)
{
    if (sensor_state != sensor_default)
    {
    uint8_t ctrl2 = QMI8658_receive(QMI8658_CTRL2);
    ctrl2 &= ~QMI8658_ASCALE_MASK; // clear previous setting
    ctrl2 |= scale << QMI8658_ASCALE_OFFSET; // OR in new setting
    QMI8658_transmit(QMI8658_CTRL2, ctrl2);
    }
    acc_scale = scale;
}

/**
 * Set scale of gyro output.
 * @param scale gyro_scale_t variable representing new scale
 */
void setGyroScale(gyro_scale_t scale)
{
    if (sensor_state != sensor_default)
    {
    uint8_t ctrl3 = QMI8658_receive(QMI8658_CTRL3);
    ctrl3 &= ~QMI8658_GSCALE_MASK; // clear previous setting
    ctrl3 |= scale << QMI8658_GSCALE_OFFSET; // OR in new setting
    QMI8658_transmit(QMI8658_CTRL3, ctrl3);
    }
    gyro_scale = scale;
}

/**
 * Set new low-pass filter value for accelerometer
 * @param lp lpf_t variable representing new low-pass filter value
 */
void setAccLPF(lpf_t lpf)
{
    if (sensor_state != sensor_default)
    {
    uint8_t ctrl5 = QMI8658_receive(QMI8658_CTRL5);
    ctrl5 &= !QMI8658_ALPF_MASK;
    ctrl5 |= lpf << QMI8658_ALPF_OFFSET;
    ctrl5 |= 0x01; // turn on acc low pass filter
    QMI8658_transmit(QMI8658_CTRL5, ctrl5);
    }
    acc_lpf = lpf;
}

/**
 * Set new low-pass filter value for gyro
 * @param lp lpf_t variable representing new low-pass filter value
 */
void setGyroLPF(lpf_t lpf)
{
    if (sensor_state != sensor_default)
    {
    uint8_t ctrl5 = QMI8658_receive(QMI8658_CTRL5);
    ctrl5 &= !QMI8658_GLPF_MASK;
    ctrl5 |= lpf << QMI8658_GLPF_OFFSET;
    ctrl5 |= 0x10; // turn on gyro low pass filter
    QMI8658_transmit(QMI8658_CTRL5, ctrl5);
    }
}

/**
 * Set new state of QMI8658.
 * @param state new state to transition to
 */
void setState(sensor_state_t state)
{
    uint8_t ctrl1;
    switch (state)
    {
    case sensor_running:
        ctrl1 = QMI8658_receive(QMI8658_CTRL1);
        // enable 2MHz oscillator
        ctrl1 &= 0xFE;
        // enable auto address increment for fast block reads
        ctrl1 |= 0x40;
        QMI8658_transmit(QMI8658_CTRL1, ctrl1);

        // enable high speed internal clock,
        // acc and gyro in full mode, and
        // disable syncSample mode
        QMI8658_transmit(QMI8658_CTRL7, 0x43);

        // disable AttitudeEngine Motion On Demand
        QMI8658_transmit(QMI8658_CTRL6, 0x00);
        break;
    case sensor_power_down:
        // disable high speed internal clock,
        // acc and gyro powered down
        QMI8658_transmit(QMI8658_CTRL7, 0x00);

        ctrl1 = QMI8658_receive(QMI8658_CTRL1);
        // disable 2MHz oscillator
        ctrl1|= 0x01;
        QMI8658_transmit(QMI8658_CTRL1, ctrl1);
        break;
    case sensor_locking:
        ctrl1 = QMI8658_receive(QMI8658_CTRL1);
        // enable 2MHz oscillator
        ctrl1 &= 0xFE;
        // enable auto address increment for fast block reads
        ctrl1 |= 0x40;
        QMI8658_transmit(QMI8658_CTRL1, ctrl1);

        // enable high speed internal clock,
        // acc and gyro in full mode, and
        // enable syncSample mode
        QMI8658_transmit(QMI8658_CTRL7, 0x83);

        // disable AttitudeEngine Motion On Demand
        QMI8658_transmit(QMI8658_CTRL6, 0x00);

        // disable internal AHB clock gating:
        QMI8658_transmit(QMI8658_CAL1_L, 0x01);
        QMI8658_CTRL9_Write(0x12);
        // re-enable clock gating
        QMI8658_transmit(QMI8658_CAL1_L, 0x00);
        QMI8658_CTRL9_Write(0x12);
        break;
    default:
        break;
    }
    sensor_state = state;
}


void getAccelerometer(void)
{

    uint8_t buf[6];
    I2C_Read(Device_addr, QMI8658_AX_L, buf, 6);
    Accel.x = (float)((int16_t)((buf[1]<<8) | (buf[0])));
    Accel.y = (float)((int16_t)((buf[3]<<8) | (buf[2])));
    Accel.z = (float)((int16_t)((buf[5]<<8) | (buf[4])));
    Accel.x = Accel.x * accelScales;
    Accel.y = Accel.y * accelScales;
    Accel.z = Accel.z * accelScales;

}
void getGyroscope(void)
{
    uint8_t buf[6];
    I2C_Read(Device_addr, QMI8658_GX_L, buf, 6);
    Gyro.x = (float)((int16_t)((buf[1]<<8) | (buf[0])));
    Gyro.y = (float)((int16_t)((buf[3]<<8) | (buf[2])));
    Gyro.z = (float)((int16_t)((buf[5]<<8) | (buf[4])));
    Gyro.x = Gyro.x * gyroScales;
    Gyro.y = Gyro.y * gyroScales;
    Gyro.z = Gyro.z * gyroScales;
}

esp_err_t QMI8658_Read_Temperature(float *temp_c)
{
    if (!temp_c) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[2] = {0};
    esp_err_t err = I2C_Read(Device_addr, QMI8658_TEMP_L, buf, 2);
    if (err != ESP_OK) {
        return err;
    }

    int16_t raw = (int16_t)(((uint16_t)buf[1] << 8) | buf[0]);
    *temp_c = ((float)raw / 256.0f) + 25.0f;
    return ESP_OK;
}
















