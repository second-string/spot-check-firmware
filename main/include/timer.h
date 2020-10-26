#ifndef TIMER_H
#define TIMER_H

#include "constants.h"

#define RELOAD_ON_EXPIRATION true

#define TIMER_PERIOD_MS (100)

// Used for debouncing
volatile bool timer_expired;

// Does NOT start timer, must use reset_timer to start count
void timer_init(void *timer_expired_callback);

// Reset timer and begin counting up to period again.
// Clears interrupt flag as well
void timer_reset();

#endif
