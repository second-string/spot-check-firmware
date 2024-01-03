#pragma once

// NOTE: Don't forget to add any new bits to the full SYSTEM_IDLE_BITS mask below!
#define SYSTEM_IDLE_TIME_BIT (1 << 0)
#define SYSTEM_IDLE_CONDITIONS_BIT (1 << 1)
#define SYSTEM_IDLE_TIDE_CHART_BIT (1 << 2)
#define SYSTEM_IDLE_SWELL_CHART_BIT (1 << 3)
#define SYSTEM_IDLE_OTA_BIT (1 << 4)
#define SYSTEM_IDLE_CLI_BIT (1 << 5)
#define SYSTEM_IDLE_CUSTOM_SCREEN_BIT (1 << 6)
#define SYSTEM_IDLE_BITS                                                                                            \
    (SYSTEM_IDLE_TIME_BIT | SYSTEM_IDLE_CONDITIONS_BIT | SYSTEM_IDLE_TIDE_CHART_BIT | SYSTEM_IDLE_SWELL_CHART_BIT | \
     SYSTEM_IDLE_OTA_BIT | SYSTEM_IDLE_CLI_BIT | SYSTEM_IDLE_CUSTOM_SCREEN_BIT)

void sleep_handler_init();
void sleep_handler_start();
void sleep_handler_block_until_system_idle();
void sleep_handler_set_busy(uint32_t system_idle_bitmask);
void sleep_handler_set_idle(uint32_t system_idle_bitmask);
