#ifndef GPIO_H
#define GPIO_H

#include "constants.h"
#include "driver/gpio.h"
#include "timer.h"

#define LED_PIN (2)

#ifdef CONFIG_ESP32_DEVBOARD
#define GPIO_BUTTON_PIN (0)
#elif defined(CONFIG_SPOT_CHECK_REV_2)
#define GPIO_BUTTON_PIN (27)
#else
#error Cannot set button GPIO pin, no dev board HW rev set in menuconfig!
#endif

typedef enum {
    BUTTON_STATE_DEFAULT,
    BUTTON_STATE_SINGLE_PRESS,
    BUTTON_STATE_HOLD,
} button_state_t;

extern volatile bool button_pressed;

void           gpio_init_local(gpio_isr_t button_isr_handler);
button_state_t gpio_debounce(timer_info_handle debounce_handle, timer_info_handle button_hold_handle);

#endif
