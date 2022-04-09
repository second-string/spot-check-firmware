#include "bq24196.h"
#include "driver/i2c.h"
#include "log.h"

#define TAG "sc-bq24196"

#define BQ24196_SLAVE_ADDR (0x08)
// #define BQ24196_SLAVE_ADDR (0x6B)

// TDOD : remove once we have a function using this (along with void cast in init func)
static void bq24196_write_reg(uint8_t reg, uint8_t byte);

static i2c_handle_t *i2c_handle;

void bq24196_init(i2c_handle_t *handle) {
    (void)bq24196_write_reg;
    i2c_handle = handle;
}

/*
 * Flow for writing a byte to a reg is:
 * - write start bit
 * - write slave addr byte w/ r/^w bit low, wait for ack
 * - write internal slave reg addr, wait for ack
 * - write data byte, wait for ack
 * - write stop bit
 */
static void bq24196_write_reg(uint8_t reg, uint8_t byte) {
    assert(i2c_handle);

    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    assert(cmd_handle);

    ESP_ERROR_CHECK(i2c_master_start(cmd_handle));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd_handle, (BQ24196_SLAVE_ADDR << 1) | I2C_MASTER_WRITE, true));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd_handle, reg, true));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd_handle, byte, false));
    ESP_ERROR_CHECK(i2c_master_stop(cmd_handle));
    // TODO : error check this too
    i2c_master_cmd_begin(i2c_handle->port, cmd_handle, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd_handle);
}

/*
 * Flow for reading a byte from a reg is:
 * - write start bit
 * - write slave addr byte w/ r/^w bit low, wait for ack
 * - write internal slave reg addr, wait for ack
 * - write slave addr byte w/ r/^w bit HIGH, wait for ack
 * - read out byte
 * - write nack
 * - write stop bit
 */
static uint8_t bq24196_read_reg(bq24196_reg_t reg) {
    assert(i2c_handle);

    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    assert(cmd_handle);

    ESP_ERROR_CHECK(i2c_master_start(cmd_handle));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd_handle, (BQ24196_SLAVE_ADDR << 1) | I2C_MASTER_WRITE, true));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd_handle, reg, true));
    ESP_ERROR_CHECK(i2c_master_stop(cmd_handle));
    // TODO : error check this too
    i2c_master_cmd_begin(i2c_handle->port, cmd_handle, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd_handle);

    uint8_t reg_val = 0x00;
    cmd_handle      = i2c_cmd_link_create();
    assert(cmd_handle);

    ESP_ERROR_CHECK(i2c_master_start(cmd_handle));
    ESP_ERROR_CHECK(i2c_master_write_byte(cmd_handle, (BQ24196_SLAVE_ADDR << 1) | I2C_MASTER_READ, true));
    ESP_ERROR_CHECK(i2c_master_read_byte(cmd_handle, &reg_val, false));
    ESP_ERROR_CHECK(i2c_master_stop(cmd_handle));
    // TODO : error check this too
    i2c_master_cmd_begin(i2c_handle->port, cmd_handle, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd_handle);

    return reg_val;
}

uint8_t bq24196_read_status_reg() {
    return bq24196_read_reg(0x08);
}
