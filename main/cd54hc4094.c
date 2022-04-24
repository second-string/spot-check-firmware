#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cd54hc4094.h"

static gpio_num_t clk;
static gpio_num_t data;
static gpio_num_t strobe;

void cd54hc4094_init(gpio_num_t clk_pin, gpio_num_t data_pin, gpio_num_t strobe_pin) {
    clk    = clk_pin;
    data   = data_pin;
    strobe = strobe_pin;

    gpio_config_t cfg = {
        .pin_bit_mask = clk | data | strobe,
        .mode         = GPIO_MODE_OUTPUT,
    };

    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(clk, 0));
    ESP_ERROR_CHECK(gpio_set_level(data, 0));
    ESP_ERROR_CHECK(gpio_set_level(strobe, 0));
}

void cd54hc4094_set_output(uint8_t bits) {
    for (uint8_t i = 0; i < sizeof(uint8_t); i++) {
        if ((bits >> i) & 0x1) {
            ESP_ERROR_CHECK(gpio_set_level(data, 1));
        } else {
            ESP_ERROR_CHECK(gpio_set_level(data, 0));
        }
    }

    // Pulse strobe (min time is is 10s to 100s of ns)
    ESP_ERROR_CHECK(gpio_set_level(strobe, 0));
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_ERROR_CHECK(gpio_set_level(strobe, 0));
}
