#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include "esp_timer.h"

#include "constants.h"

#define RELOAD_ON_EXPIRATION true

#define BUTTON_TIMER_PERIOD_MS (40)
#define BUTTON_HOLD_TIMER_PERIOD_MS (3000)
#define CONDITIONS_UPDATE_INTERVAL_MINUTES (3)
#define ONE_SECOND_TIMER_MS (1000)

typedef struct timer_info_t *timer_info_handle;

// Used for debouncing
volatile bool button_timer_expired;
//
// Used for button hold timer
volatile bool button_hold_timer_expired;

// Does NOT start timer, must use reset_timer to start count
timer_info_handle timer_init(char *timer_name, void *timer_expired_callback, unsigned int timeout_microseconds);

// Reset timer and begin counting up to period again.
// Clears interrupt flag as well
void timer_reset(timer_info_handle handle, bool auto_reload);

#endif
