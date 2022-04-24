#pragma once

#include "driver/gpio.h"

#define CD54HC4094_3V3_EN_BIT (1 << 0)
#define CD54HC4094_P15V_EN_BIT (1 << 0)
#define CD54HC4094_N15V_EN_BIT (1 << 0)
#define CD54HC4094_N20V_EN_BIT (1 << 0)
#define CD54HC4094_DISP_SPV_BIT (1 << 0)
#define CD54HC4094_P22V_EN_BIT (1 << 0)
#define CD54HC4094_DISP_GMODE_BIT (1 << 0)
#define CD54HC4094_DISP_OE_BIT (1 << 0)

void cd54hc4094_init(gpio_num_t clk_pin, gpio_num_t data_pin, gpio_num_t strobe_pin);
void cd54hc4094_set_output(uint8_t bits);
