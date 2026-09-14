#include "sensor.h"
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"

#include "config.h"     // I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO

static const char *TAG = "sensor";

#define I2C_MASTER_NUM         I2C_NUM_0    // I2C peripheral instance to use
#define I2C_MASTER_FREQ_HZ     100000       // I2C bus clock (100 kHz, standard mode)
#define I2C_MASTER_TIMEOUT_MS  1000         // Max wait for a single I2C transaction

#define HTU21D_SENSOR_ADDR     0x40         // 7-bit I2C address of the sensor

// HTU21D commands (no-hold master)
#define TEMP_CMD               0xF3         // trigger temperature measurement
#define HUM_CMD                0xF5         // trigger humidity measurement

#define SOFT_RESET             0xFE         // Soft reset command
#define SOFT_RESET_TIME        15           // 15 ms for it to complete (datasheet)

/* After deep sleep, GPIOs come back from isolation and the I2C pull-ups need
   a moment to charge the bus back up to idle-high before the first transaction */
#define I2C_BUS_SETTLE_MS      5

// Datasheet max conversion time: temp 50 ms, humidity 16 ms. Use 50 ms for both
#define HTU21D_CONV_DELAY_MS   50

static i2c_master_bus_handle_t s_bus;   // handle to the I2C bus
static i2c_master_dev_handle_t s_dev;   // handle to the HTU21D on that bus

// CRC-8 check (polynomial 0x131 = x^8 + x^5 + x^4 + 1) as specified in the HTU21D datasheet
static bool htu21d_crc_ok(uint16_t value, uint8_t crc)
{
    uint32_t remainder = ((uint32_t)value << 8) | crc;
    uint32_t divisor   = 0x988000;      // 0x131 shifted left by 15
    for (int i = 0; i < 16; i++) {
        if (remainder & (1UL << (23 - i))) {
            remainder ^= divisor;
        }
        divisor >>= 1;
    }
    return remainder == 0;
}

// Send a command, wait for conversion, read 3 bytes (MSB, LSB, CRC), return the raw 16-bit value
static esp_err_t htu21d_measure(uint8_t cmd, uint16_t *raw_out)
{
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, &cmd, 1, I2C_MASTER_TIMEOUT_MS),
                        TAG, "cmd 0x%02X failed", cmd);

    vTaskDelay(pdMS_TO_TICKS(HTU21D_CONV_DELAY_MS));

    uint8_t buf[3];     // MSB, LSB, CRC
    ESP_RETURN_ON_ERROR(i2c_master_receive(s_dev, buf, sizeof(buf), I2C_MASTER_TIMEOUT_MS),
                        TAG, "read failed");

    uint16_t raw = ((uint16_t)buf[0] << 8) | buf[1];
    if (!htu21d_crc_ok(raw, buf[2])) {
        ESP_LOGW(TAG, "CRC mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    *raw_out = raw & 0xFFFC;    // clear the two status bits in the LSB
    return ESP_OK;
}

// I2C master and device (HTU21D) configuration
esp_err_t sensor_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_bus), TAG, "bus init failed");

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = HTU21D_SENSOR_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_config, &s_dev), TAG, "add device failed");

    vTaskDelay(pdMS_TO_TICKS(I2C_BUS_SETTLE_MS));   // Wait for I2C pins / pull-ups to settle

    /* Soft reset: restore the user register to defaults and clear any stale state
       Doubles as a presence check — a missing sensor NACKs here and init fails early
       One retry: right after a deep-sleep wake the bus can occasionally NACK the
       very first transaction before the pull-ups have fully settled */
    uint8_t cmd = SOFT_RESET;

    esp_err_t err = i2c_master_transmit(s_dev, &cmd, 1, I2C_MASTER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "soft reset failed once (%s), retrying", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(I2C_BUS_SETTLE_MS));   // Give the bus another moment
        err = i2c_master_transmit(s_dev, &cmd, 1, I2C_MASTER_TIMEOUT_MS);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "soft reset failed");
    vTaskDelay(pdMS_TO_TICKS(SOFT_RESET_TIME));

    ESP_LOGI(TAG, "HTU21D ready (SDA=%d, SCL=%d)", I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO);
    return ESP_OK;
}

/* Read the raw values and convert them to real units
   Either pointer may be NULL — pass NULL to skip that measurement */
esp_err_t sensor_read(float *temperature_c, float *humidity_pct)
{
    uint16_t raw;

    // Conversion formulas (HTU21D datasheet)
    if (temperature_c) {
        ESP_RETURN_ON_ERROR(htu21d_measure(TEMP_CMD, &raw), TAG, "temp read");
        *temperature_c = -46.85f + 175.72f * (float)raw / 65536.0f;
    }

    if (humidity_pct) {
        ESP_RETURN_ON_ERROR(htu21d_measure(HUM_CMD, &raw), TAG, "hum read");
        *humidity_pct = -6.0f + 125.0f * (float)raw / 65536.0f;
    }

    return ESP_OK;
}
