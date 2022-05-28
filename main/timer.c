#include "constants.h"

#include "freeRTOS/FreeRTOS.h"
#include "log.h"

#include "timer.h"

#define TAG "sc-timer"

typedef struct timer_info_t {
    esp_timer_handle_t timer_handle;
    unsigned int       timeout_milliseconds;
    void              *callback;
} timer_info_t;

// Used for button debounce, conditions re-fetch, and display splash screen callback
static timer_info_t timer_infos[3];
static unsigned int next_timer_info_idx = 0;

timer_info_handle timer_init(char        *timer_name,
                             void        *timer_expired_callback,
                             void        *callback_args,
                             unsigned int timeout_milliseconds) {
    configASSERT(next_timer_info_idx < sizeof(timer_infos) / sizeof(timer_info_t));

    timer_info_t *next_info = &timer_infos[next_timer_info_idx];
    next_timer_info_idx++;
    next_info->callback             = timer_expired_callback;
    next_info->timeout_milliseconds = timeout_milliseconds;

    esp_timer_create_args_t timer_args = {.callback = next_info->callback, .arg = callback_args, .name = timer_name};

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &next_info->timer_handle));

    return next_info;
}

void timer_change_period(timer_info_handle handle, uint32_t period_ms) {
    handle->timeout_milliseconds = period_ms;
}

void timer_reset(timer_info_handle handle, bool auto_reload) {
    // Don't error check the stop, since we don't care if it fails (that means
    // we tried to stop a non-running timer)
    esp_timer_stop(handle->timer_handle);

    // ESP timers run on microseconds for some dumb reason, so scall MS params accordingly
    if (auto_reload) {
        log_printf(TAG, LOG_LEVEL_DEBUG, "Starting repeating timer with period %ums", handle->timeout_milliseconds);
        ESP_ERROR_CHECK(esp_timer_start_periodic(handle->timer_handle, handle->timeout_milliseconds * 1000));
    } else {
        log_printf(TAG, LOG_LEVEL_DEBUG, "Starting one-shot timer with period %ums", handle->timeout_milliseconds);
        ESP_ERROR_CHECK(esp_timer_start_once(handle->timer_handle, handle->timeout_milliseconds * 1000));
    }
}
