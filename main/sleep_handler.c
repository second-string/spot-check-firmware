#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "memfault/panics/assert.h"

#include "constants.h"
#include "log.h"
#include "sleep_handler.h"

#define TAG SC_TAG_SLEEP_HANDLER

static EventGroupHandle_t system_idle_event_group;

void sleep_handler_init() {
    system_idle_event_group = xEventGroupCreate();
}

void sleep_handler_start() {
    xEventGroupSetBits(system_idle_event_group, SYSTEM_IDLE_BITS);
}

/*
 * Block (yielding not spinning) until all idle bits set by processes that should not be interrupted by sleeping.
 * Ensures we don't chuck ourselves into deep sleep while in the middle of an operation like saving an image to flash or
 * downloading an OTA image
 */
void sleep_handler_block_until_system_idle() {
    log_printf(LOG_LEVEL_DEBUG, "Blocking until all busy processes set system back to idle");
    xEventGroupWaitBits(system_idle_event_group, SYSTEM_IDLE_BITS, UINT32_MAX, true, portMAX_DELAY);
    log_printf(LOG_LEVEL_DEBUG, "All processes idling, exiting blocking wait");
}

/*
 * Indicate that something is going on that should block going into deep sleep. Must reset bit using
 * sleep_handler_set_idle for system to enter deep sleep.
 */
void sleep_handler_set_busy(uint32_t system_idle_bitmask) {
    // Make sure we're just passing in a single bitmask macro
    MEMFAULT_ASSERT((system_idle_bitmask & (system_idle_bitmask - 1)) == 0);

    xEventGroupClearBits(system_idle_event_group, system_idle_bitmask);
}

/*
 * Indicate that something was blocking going into deep sleep and is now complete. Should be used after a call to
 * sleep_handler_set_busy..
 */
void sleep_handler_set_idle(uint32_t system_idle_bitmask) {
    // Make sure we're just passing in a single bitmask macro
    MEMFAULT_ASSERT((system_idle_bitmask & (system_idle_bitmask - 1)) == 0);

    xEventGroupSetBits(system_idle_event_group, system_idle_bitmask);
}
