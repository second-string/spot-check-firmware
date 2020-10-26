#ifndef GPIO_H
#define GPIO_H

#include "constants.h"

#define LED_PIN (2)
#define GPIO_BUTTON_PIN (0)

volatile uint8_t button_pressed;

void gpio_init_local(gpio_isr_t button_isr_handler);
bool button_was_released();

#endif
