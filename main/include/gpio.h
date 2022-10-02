#ifndef GPIO_H
#define GPIO_H

#include "constants.h"
#include "driver/gpio.h"
#include "timer.h"

#define LED_PIN (2)

#if defined(CONFIG_ESP32_DEVBOARD) || defined(CONFIG_SPOT_CHECK_REV_3_1)
#define GPIO_BUTTON_PIN (0)
#elif defined(CONFIG_SPOT_CHECK_REV_2)
#define GPIO_BUTTON_PIN (27)
#else
#error Cannot set button GPIO pin, no dev board HW rev set in menuconfig!
#endif

void gpio_init();

#endif
