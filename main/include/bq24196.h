#pragma once

#include "i2c.h"

#define BQ24196_I2C_PORT I2C_NUM_0
#define BQ24196_I2C_SDA_PIN GPIO_NUM_13
#define BQ24196_I2C_SCL_PIN GPIO_NUM_14

typedef enum {
    BQ24196_REG_INPUT_SRC_CTRL      = 0x00,
    BQ24196_REG_POWER_ON_CONFIG     = 0x01,
    BQ24196_REG_CHARGE_VOLTAGE_CTRL = 0x04,
    BQ24196_REG_CHARGE_TERM         = 0x05,
    BQ24196_REG_MISC_CTRL           = 0x07,
    BQ24196_REG_STATUS              = 0x08,
    BQ24196_REG_FAULT               = 0x09,
    BQ24196_REG_PART_INFO           = 0xA,

    BQ24196_REG_COUNT,
} bq24196_reg_t;

void      bq24196_init(i2c_handle_t *i2c_handle);
void      bq24196_start();
esp_err_t bq24196_read_reg(bq24196_reg_t reg, uint8_t *value);
uint8_t   bq24196_read_charge_term_reg();
uint8_t   bq24196_read_status_reg();
uint8_t   bq24196_read_fault_reg();
esp_err_t bq24196_disable_charging();
esp_err_t bq24196_disable_watchdog();
