#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cd54hc4094.h"
#include "constants.h"
#include "log.h"

#define TAG SC_TAG_CD54HC4094

static gpio_num_t clk;
static gpio_num_t data;
static gpio_num_t strobe;

void cd54hc4094_init(gpio_num_t clk_pin, gpio_num_t data_pin, gpio_num_t strobe_pin) {
    clk    = clk_pin;
    data   = data_pin;
    strobe = strobe_pin;

    gpio_config_t cfg = {
        .pin_bit_mask = (1LL << clk) | (1LL << data) | (1 << strobe),
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
        .pull_up_en   = false,
        .pull_down_en = false,
    };

    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(clk, 0));
    ESP_ERROR_CHECK(gpio_set_level(data, 0));
    ESP_ERROR_CHECK(gpio_set_level(strobe, 0));

    log_printf(LOG_LEVEL_DEBUG, "CD55HC4094 inited with pins CLK: %u - DATA: %u - STROBE: %u", clk, data, strobe);
}

void cd54hc4094_set_output(uint8_t bits) {
    assert(clk);
    assert(data);
    assert(strobe);

    ESP_ERROR_CHECK(gpio_set_level(clk, 0));
    ESP_ERROR_CHECK(gpio_set_level(strobe, 0));

    for (int8_t i = sizeof(uint8_t) * 8 - 1; i >= 0; i--) {
        log_printf(LOG_LEVEL_DEBUG, "Setting shiftreg pin %u to %s", i, ((bits >> i) & 0x1) ? "HIGH" : "LOW");
        if ((bits >> i) & 0x1) {
            ESP_ERROR_CHECK(gpio_set_level(data, 1));
        } else {
            ESP_ERROR_CHECK(gpio_set_level(data, 0));
        }

        // clk pulse happens too fast after data set, give it time between. Not great for perf, but fine for cli
        // debugging purposes
        vTaskDelay(pdMS_TO_TICKS(1));

        // Latch data in with clk pulse (min time is is 10s to 100s of ns)
        ESP_ERROR_CHECK(gpio_set_level(clk, 1));
        vTaskDelay(pdMS_TO_TICKS(1));
        ESP_ERROR_CHECK(gpio_set_level(clk, 0));
    }

    // Pulse strobe (min time is is 10s to 100s of ns)
    ESP_ERROR_CHECK(gpio_set_level(strobe, 1));
    vTaskDelay(pdMS_TO_TICKS(1));
    ESP_ERROR_CHECK(gpio_set_level(strobe, 0));
}
