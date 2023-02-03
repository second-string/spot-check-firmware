#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include "esp_timer.h"

#include "constants.h"

typedef struct timer_info_t *timer_info_handle;

// Does NOT start timer, must use reset_timer to start count
timer_info_handle timer_local_init(char        *timer_name,
                                   void        *timer_expired_callback,
                                   void        *callback_args,
                                   unsigned int timeout_milliseconds);

// Reset timer and begin counting up to period again.
// Clears interrupt flag as well
void timer_reset(timer_info_handle handle, bool auto_reload);

void timer_change_period(timer_info_handle handle, uint32_t period_ms);

#endif
