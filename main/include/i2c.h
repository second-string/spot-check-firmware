#pragma once

#include "driver/gpio.h"
#include "driver/i2c.h"

typedef struct {
    i2c_port_t   port;
    i2c_config_t config;
} i2c_handle_t;

void i2c_init(i2c_port_t port, gpio_num_t sda_pin, gpio_num_t scl_pin, i2c_handle_t *handle);
void i2c_start(i2c_handle_t *handle);
