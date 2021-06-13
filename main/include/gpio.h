#ifndef GPIO_H
#define GPIO_H

#include "constants.h"
#include "timer.h"

#define LED_PIN (2)
#define GPIO_BUTTON_PIN (0)

typedef enum {
    BUTTON_STATE_DEFAULT,
    BUTTON_STATE_SINGLE_PRESS,
    BUTTON_STATE_HOLD,
} button_state_t;

volatile uint8_t button_pressed;

void           gpio_init_local(gpio_isr_t button_isr_handler);
button_state_t button_was_released(timer_info_handle debounce_handle, timer_info_handle button_hold_handle);

#endif
