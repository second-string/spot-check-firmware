#pragma once

#include "i2c.h"

#define BQ24196_I2C_PORT I2C_NUM_0
#define BQ24196_I2C_SDA_PIN GPIO_NUM_13
#define BQ24196_I2C_SCL_PIN GPIO_NUM_14

typedef enum {
    BQ24196_REG_STATUS = 0x08,

    BQ24196_REG_COUNT,
} bq24196_reg_t;

void    bq24196_init(i2c_handle_t *i2c_handle);
uint8_t bq24196_read_status_reg();
void    bq24196_write_fake_reg();
