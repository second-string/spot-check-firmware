#include "constants.h"

#include "freeRTOS/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "timer.h"

static bool timer_inited = false;
static esp_timer_handle_t timer_handle;

void timer_init(void *timer_expired_callback) {
    esp_timer_create_args_t timer_args = {
            .callback = timer_expired_callback,
            .name = "debounce"
    };

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle));
    timer_inited = true;
 }

void timer_reset() {
    if (!timer_inited) {
        ESP_LOGE(TAG, "Timer not yet inited, call timer_init()");
        return;
    }

    // Don't error check the stop, since we don't care if it fails (that means
    // we tried to stop a non-running timer)
    esp_timer_stop(timer_handle);
    ESP_ERROR_CHECK(esp_timer_start_once(timer_handle, TIMER_PERIOD_MS * 1000));
}
