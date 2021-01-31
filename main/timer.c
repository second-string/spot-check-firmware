#include "constants.h"

#include "esp_log.h"
#include "freeRTOS/FreeRTOS.h"

#include "timer.h"

typedef struct timer_info_t {
    esp_timer_handle_t timer_handle;
    unsigned int       timeout_microseconds;
    void *             callback;
} timer_info_t;

static timer_info_t timer_infos[2];
static unsigned int next_timer_info_idx = 0;

timer_info_handle timer_init(char *timer_name, void *timer_expired_callback, unsigned int timeout_microseconds) {
    timer_info_t *next_info = &timer_infos[next_timer_info_idx];
    next_timer_info_idx++;
    next_info->callback             = timer_expired_callback;
    next_info->timeout_microseconds = timeout_microseconds;

    esp_timer_create_args_t timer_args = {.callback = next_info->callback, .name = timer_name};

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &next_info->timer_handle));

    return next_info;
}

void timer_reset(timer_info_handle handle, bool auto_reload) {
    // Don't error check the stop, since we don't care if it fails (that means
    // we tried to stop a non-running timer)
    esp_timer_stop(handle->timer_handle);
    if (auto_reload) {
        ESP_ERROR_CHECK(esp_timer_start_periodic(handle->timer_handle, handle->timeout_microseconds));
    } else {
        ESP_ERROR_CHECK(esp_timer_start_once(handle->timer_handle, handle->timeout_microseconds));
    }
}
