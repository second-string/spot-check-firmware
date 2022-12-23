#include "esp_err.h"

#include "constants.h"
#include "i2c.h"
#include "log.h"

#define TAG SC_TAG_I2C

#define I2C_MASTER_TX_BUF_DISABLE 0 /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0 /*!< I2C master doesn't need buffer */

void i2c_init(i2c_port_t port, gpio_num_t sda_pin, gpio_num_t scl_pin, i2c_handle_t *handle) {
    handle->port                    = port;
    handle->config.mode             = I2C_MODE_MASTER;
    handle->config.sda_io_num       = sda_pin;
    handle->config.scl_io_num       = scl_pin;
    handle->config.sda_pullup_en    = true;  // SDK recommends externals even with these turned on, 10k internals
    handle->config.scl_pullup_en    = true;
    handle->config.master.clk_speed = 115200;
    handle->config.clk_flags        = 0;

    esp_err_t err = i2c_param_config(handle->port, &handle->config);
    if (err != ESP_OK) {
        log_printf(LOG_LEVEL_DEBUG, "Error! %u", err);
        assert(false);
    }
}

void i2c_start(i2c_handle_t *handle) {
    // Current start func only suports master (zerod rx/tx buffer lengths)
    log_printf(LOG_LEVEL_DEBUG,
               "instantiating with sda: %u and scl: %u",
               handle->config.sda_io_num,
               handle->config.scl_io_num);
    esp_err_t err =
        i2c_driver_install(handle->port, handle->config.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
    if (err != ESP_OK) {
        log_printf(LOG_LEVEL_DEBUG, "Error! %u", err);
        assert(false);
    }
}
