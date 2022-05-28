#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

#include "constants.h"
#include "gpio.h"

#include "log.h"

#define TAG "sc-gpio"

#define GPIO_INPUT_PIN_SEL (1 << GPIO_BUTTON_PIN)
#define BUTTON_TIMER_PERIOD_MS (20)

typedef enum {
    WAITING_FOR_PRESS,
    DEBOUNCING_PRESS,
    DEBOUNCING_RELEASE,
    WAITING_FOR_RELEASE,
} debounce_state;

static volatile debounce_state current_state;
static timer_info_handle       debounce_handle;

static void button_isr_handler(void *arg) {
    if (current_state == WAITING_FOR_PRESS) {
        timer_reset(debounce_handle, false);
        current_state = DEBOUNCING_PRESS;
    } else if (current_state == WAITING_FOR_RELEASE) {
        current_state = DEBOUNCING_RELEASE;
        timer_reset(debounce_handle, false);
    }
}

static void button_timer_expired_callback(void *timer_args) {
    uint8_t level = gpio_get_level(GPIO_BUTTON_PIN);

    if (current_state == DEBOUNCING_PRESS) {
        if (level) {
            // TODO :: Signal button event bits / mutex / semaphore
            log_printf(TAG, LOG_LEVEL_INFO, "Successfully debounced button press");
            current_state = WAITING_FOR_RELEASE;
        } else {
            // Failed debounce check, back to waiting for press
            log_printf(TAG, LOG_LEVEL_INFO, "Failed press debounce, returning to waiting for press");
            current_state = WAITING_FOR_PRESS;
        }
    } else if (current_state == DEBOUNCING_RELEASE) {
        if (!level) {
            // TODO :: Signal button event bits / mutex / semaphore
            log_printf(TAG, LOG_LEVEL_INFO, "Successfully debounced button release");
            current_state = WAITING_FOR_PRESS;
        } else {
            // Failed debounce check, back to waiting for release
            log_printf(TAG, LOG_LEVEL_INFO, "Failed release debounce, returning to waiting for release");
            current_state = WAITING_FOR_RELEASE;
        }
    } else {
        // Timer should never be running unless we're actively debouncing
        configASSERT(0);
    }
}

void gpio_init() {
    current_state = WAITING_FOR_PRESS;

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
#if defined(CONFIG_ESP32_DEVBOARD)
    input_config.pull_up_en = 1;
#elif defined(CONFIG_SPOT_CHECK_REV_2) || defined(CONFIG_SPOT_CHECK_REV_3)
    input_config.pull_up_en = 0;
#else
#error Cannot intialize button GPIO, no dev board HW rev set in menuconfig!
#endif

    debounce_handle = timer_init("debounce", button_timer_expired_callback, NULL, BUTTON_TIMER_PERIOD_MS);

    gpio_config(&input_config);
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_BUTTON_PIN, button_isr_handler, (void *)GPIO_BUTTON_PIN));
}
