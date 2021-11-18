#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

#include "constants.h"
#include "gpio.h"

#include "esp_log.h"

#define TAG "sc-gpio"

#define GPIO_INPUT_PIN_SEL (1 << GPIO_BUTTON_PIN)

typedef enum {
    WAITING_FOR_PRESS,
    DEBOUNCING_PRESS,
    DEBOUNCING_RELEASE,
    WAITING_FOR_RELEASE,
} debounce_state;

static volatile debounce_state current_state;

// Store state of whether or not we've registered a hold so we don't continuously signal it after the first time
static volatile bool button_hold_signalled;

void gpio_init_local(gpio_isr_t button_isr_handler) {
    button_pressed = false;
    current_state  = WAITING_FOR_PRESS;

    // Cheater init for LED output compared to full config for button input below
    gpio_reset_pin(LED_PIN);
    // Needs to be I/O to be able to correctly read level (otherwise always reads zero)
    gpio_set_direction(LED_PIN, GPIO_MODE_INPUT_OUTPUT);

    gpio_config_t input_config;
    input_config.intr_type    = GPIO_INTR_ANYEDGE;
    input_config.mode         = GPIO_MODE_INPUT;
    input_config.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    input_config.pull_down_en = 0;
// The hardware debounce circuit on custom revs doesn't work correctly if the button GPIO is pulled up internally
#ifdef CONFIG_ESP32_DEVBOARD
    input_config.pull_up_en = 1;
#elif defined(CONFIG_SPOT_CHECK_REV_2)
    input_config.pull_up_en = 0;
#else
#error Cannot intialize button GPIO, no dev board HW rev set in menuconfig!
#endif

    gpio_config(&input_config);
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_BUTTON_PIN, button_isr_handler, (void *)GPIO_BUTTON_PIN));
}

button_state_t button_was_released(timer_info_handle debounce_handle, timer_info_handle button_hold_handle) {
    switch (current_state) {
        case WAITING_FOR_PRESS:
            if (button_pressed) {
                // Reset both debounce and hold timers
                timer_reset(debounce_handle, false);
                timer_reset(button_hold_handle, false);
                current_state = DEBOUNCING_PRESS;
            }
            break;
        case DEBOUNCING_PRESS:
            if (button_timer_expired) {
                if (button_pressed) {
                    current_state = WAITING_FOR_RELEASE;
                } else {
                    current_state = WAITING_FOR_PRESS;
                }
            }
            break;
        case WAITING_FOR_RELEASE:
            if (!button_pressed) {
                timer_reset(debounce_handle, false);
                timer_reset(button_hold_handle, false);
                button_hold_signalled = false;
                current_state         = DEBOUNCING_RELEASE;
            } else if (button_hold_timer_expired && !button_hold_signalled) {
                // If we're still pressed but hold timer expired, signal
                button_hold_signalled = true;
                return BUTTON_STATE_HOLD;
            }
            break;
        case DEBOUNCING_RELEASE:
            if (button_timer_expired) {
                if (button_pressed) {
                    current_state = WAITING_FOR_RELEASE;
                } else {
                    current_state = WAITING_FOR_PRESS;
                    return BUTTON_STATE_SINGLE_PRESS;
                }
            }
            break;
    }

    return BUTTON_STATE_DEFAULT;
}
