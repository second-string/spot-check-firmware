#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "scheduler_task.h"

#include "constants.h"
#include "gpio.h"
#include "http_client.h"
#include "log.h"
#include "nvs.h"
#include "ota_task.h"
#include "screen_img_handler.h"
#include "sleep_handler.h"
#include "sntp_time.h"
#include "timer.h"
#include "wifi.h"

#define TAG SC_TAG_SCHEDULER

#define NUM_DIFFERENTIAL_UPDATES 3
#define NUM_DISCRETE_UPDATES 4

#define TIME_UPDATE_INTERVAL_SECONDS (1 * SECS_PER_MIN)
#define CONDITIONS_UPDATE_INTERVAL_SECONDS (20 * SECS_PER_MIN)
#define CHARTS_UPDATE_INTERVAL_SECONDS (MINS_PER_HOUR * SECS_PER_MIN)
#define OTA_CHECK_INTERVAL_SECONDS (CONFIG_OTA_CHECK_INTERVAL_HOURS * MINS_PER_HOUR * SECS_PER_MIN)

#define UPDATE_CONDITIONS_BIT (1 << 0)
#define UPDATE_TIDE_CHART_BIT (1 << 1)
#define UPDATE_SWELL_CHART_BIT (1 << 2)
#define UPDATE_TIME_BIT (1 << 3)
#define UPDATE_SPOT_NAME_BIT (1 << 4)
#define CHECK_OTA_BIT (1 << 5)

typedef struct {
    char   name[11];
    time_t update_interval_secs;
    time_t last_executed_epoch_secs;
    bool   force_next_update;
    void (*execute)(void);
} differential_update_t;

typedef struct {
    char    name[14];
    uint8_t hour;
    uint8_t minute;
    bool    executed_already;
    void (*execute)(void);
    bool force_next_update;
} discrete_update_t;

static void scheduler_trigger_ota_check();

static TaskHandle_t scheduler_task_handle;

static volatile unsigned int seconds_elapsed;
static conditions_t          last_retrieved_conditions;

// Execute function cannot be blocking! Will execute from 1 sec timer interrupt callback
static differential_update_t differential_updates[NUM_DIFFERENTIAL_UPDATES] = {
    {
        .name                 = "time",
        .force_next_update    = true,
        .update_interval_secs = TIME_UPDATE_INTERVAL_SECONDS,
        .execute              = scheduler_trigger_time_update,
    },
    {
        .name                 = "conditions",
        .force_next_update    = true,
        .update_interval_secs = CONDITIONS_UPDATE_INTERVAL_SECONDS,
        .execute              = scheduler_trigger_conditions_update,
    },
    {
        .name                 = "ota",
        .force_next_update    = true,
        .update_interval_secs = OTA_CHECK_INTERVAL_SECONDS,
        .execute              = scheduler_trigger_ota_check,
    },
};

static discrete_update_t discrete_updates[NUM_DISCRETE_UPDATES] = {
    {
        .name              = "tide",
        .force_next_update = true,
        .hour              = 3,
        .minute            = 0,
        .executed_already  = false,
        .execute           = scheduler_trigger_tide_chart_update,
    },
    {
        .name              = "swell_morning",
        .force_next_update = true,
        .hour              = 12,
        .minute            = 0,
        .executed_already  = false,
        .execute           = scheduler_trigger_swell_chart_update,
    },
    {
        .name              = "swell_midday",
        .force_next_update = true,
        .hour              = 21,
        .minute            = 0,
        .executed_already  = false,
        .execute           = scheduler_trigger_swell_chart_update,
    },
    {
        .name              = "swell_evening",
        .force_next_update = true,
        .hour              = 17,
        .minute            = 0,
        .executed_already  = false,
        .execute           = scheduler_trigger_swell_chart_update,
    },
};

/*
 * Returns whether or not the two time values (hour or min) match, OR always true if value is 0xFF wildcard (similar to
 * cron's '*')
 */
static inline bool discrete_time_matches(int current, uint8_t check) {
    return (current == check) || (check == 0xFF);
}

/*
 * Polling function that runs every 1 second. Responsible for checking all differential/discrete time update structs
 * and if any have reached their elapsed time, execute and update them. No execute functions for the update structs
 * should be blocking - they should all either call trigger functions for main task to handle or execute very quickly
 * and return.
 */
static void scheduler_polling_timer_callback(void *timer_args) {
    struct tm now_local;
    sntp_time_get_local_time(&now_local);
    time_t now_epoch_secs = mktime(&now_local);

    // If time differential has passed OR force execute flag set, execute and bring up to date.
    differential_update_t *diff_check = NULL;
    for (int i = 0; i < (NUM_DIFFERENTIAL_UPDATES - 1); i++) {
        diff_check = &differential_updates[i];

        if (((now_epoch_secs - diff_check->last_executed_epoch_secs) > diff_check->update_interval_secs) ||
            diff_check->force_next_update) {
            // Printing time_t is fucked, have to convert to double with difftime
            log_printf(LOG_LEVEL_DEBUG,
                       "Executing polling diff update '%s' (last: %.0f, now: %.0f, intvl: %.0f, force: %u)",
                       diff_check->name,
                       difftime(diff_check->last_executed_epoch_secs, 0),
                       difftime(now_epoch_secs, 0),
                       difftime(diff_check->update_interval_secs, 0),
                       diff_check->force_next_update);

            diff_check->execute();
            diff_check->last_executed_epoch_secs = now_epoch_secs;
            diff_check->force_next_update        = false;
        }
    }

    // If matching time has arrived OR force execute flag set, execute.
    discrete_update_t *discrete_check = NULL;
    for (int i = 0; i < (NUM_DISCRETE_UPDATES - 1); i++) {
        discrete_check = &discrete_updates[i];

        if ((discrete_time_matches(now_local.tm_hour, discrete_check->hour) &&
             discrete_time_matches(now_local.tm_min, discrete_check->minute)) ||
            discrete_check->force_next_update) {
            if (!discrete_check->executed_already) {
                log_printf(LOG_LEVEL_DEBUG,
                           "Executing discrete update '%s' (curr hr: %u, curr min: %u, check hr: %u, check min: %u, "
                           "force: %u)",
                           discrete_check->name,
                           now_local.tm_hour,
                           now_local.tm_min,
                           discrete_check->hour,
                           discrete_check->minute,
                           discrete_check->force_next_update);

                discrete_check->execute();
                discrete_check->force_next_update = false;
                discrete_check->executed_already  = true;
            }
        } else if (discrete_check->executed_already) {
            discrete_check->executed_already = false;
        }
    }
}

static void scheduler_task(void *args) {
    log_printf(LOG_LEVEL_DEBUG, "List of all time differential updates:");
    differential_update_t *diff_check = NULL;
    for (int i = 0; i < (NUM_DIFFERENTIAL_UPDATES - 1); i++) {
        diff_check = &differential_updates[i];
        log_printf(LOG_LEVEL_DEBUG,
                   "'%s' executing every %.0f seconds",
                   diff_check->name,
                   diff_check->update_interval_secs);
    }

    log_printf(LOG_LEVEL_DEBUG, "List of all discrete updates:");
    discrete_update_t *discrete_check = NULL;
    for (int i = 0; i < (NUM_DISCRETE_UPDATES - 1); i++) {
        discrete_check = &discrete_updates[i];
        log_printf(LOG_LEVEL_DEBUG,
                   "'%s' executing at %u:%02u",
                   discrete_check->name,
                   discrete_check->hour,
                   discrete_check->minute);
    }

    // Run polling timer every second that's responsible for triggering any differential or discrete updates that have
    // reached execution time. Timer only calls trigger function, task waits indefinitely on event bits from triggers.
    timer_info_handle scheduler_polling_timer_handle =
        timer_init("scheduler-polling", scheduler_polling_timer_callback, NULL, MS_PER_SEC);
    timer_reset(scheduler_polling_timer_handle, true);

    // Wait forever until connected
    wifi_block_until_connected();

    uint32_t update_bits       = 0;
    bool     full_clear        = false;
    bool     scheduler_success = false;
    while (1) {
        // Wait forever until a notification received. Clears all bits on exit since we'll handle every set bit in one
        // go
        xTaskNotifyWait(0x0, UINT32_MAX, &update_bits, portMAX_DELAY);

        log_printf(LOG_LEVEL_DEBUG,
                   "scheduler task received task notification of value 0x%02X, updating accordingly",
                   update_bits);

        // If we're doing all of them, it means this is the first time we're refreshing after boot, and it should wait
        // and do a full clear before redrawing everything. Otherwise it's very piecemeal and slow
        full_clear = (update_bits & UPDATE_SPOT_NAME_BIT) && (update_bits & UPDATE_SPOT_NAME_BIT) &&
                     (update_bits & UPDATE_CONDITIONS_BIT) && (update_bits & UPDATE_TIDE_CHART_BIT) &&
                     (update_bits & UPDATE_SWELL_CHART_BIT);

        /***************************************
         * Network update section
         **************************************/
        if (update_bits & UPDATE_CONDITIONS_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_CONDITIONS_BIT);
            conditions_t new_conditions = {0};
            bool         success = scheduler_success = screen_img_handler_download_and_save_conditions(&new_conditions);
            if (success) {
                memcpy(&last_retrieved_conditions, &new_conditions, sizeof(conditions_t));
            }
            sleep_handler_set_idle(SYSTEM_IDLE_CONDITIONS_BIT);
        }

        if (update_bits & UPDATE_TIDE_CHART_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_TIDE_CHART_BIT);
            screen_img_handler_download_and_save(SCREEN_IMG_TIDE_CHART);
            sleep_handler_set_idle(SYSTEM_IDLE_TIDE_CHART_BIT);
        }

        if (update_bits & UPDATE_SWELL_CHART_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_SWELL_CHART_BIT);
            screen_img_handler_download_and_save(SCREEN_IMG_SWELL_CHART);
            sleep_handler_set_idle(SYSTEM_IDLE_SWELL_CHART_BIT);
        }

        if (update_bits & CHECK_OTA_BIT) {
            // Just kicks off the task non-blocking so this won't actually disrupt anything with rest of conditions
            // update loop
            ota_task_start();
        }

        /***************************************
         * Framebuffer update section
         **************************************/
        if (full_clear) {
            log_printf(LOG_LEVEL_DEBUG,
                       "Performing full screen clear from scheduler_task since every piece was updated");
            screen_img_handler_full_clear();
        }

        if (update_bits & UPDATE_TIME_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_TIME_BIT);
            if (!full_clear) {
                screen_img_handler_clear_time();
                screen_img_handler_clear_date(false);
            }
            screen_img_handler_draw_time();
            screen_img_handler_draw_date();
            log_printf(LOG_LEVEL_INFO, "scheduler task updated time");
            sleep_handler_set_idle(SYSTEM_IDLE_TIME_BIT);
        }

        if (update_bits & UPDATE_SPOT_NAME_BIT) {
            // Slightly unique case as in it requires no network update, just used as a display update trigger
            sleep_handler_set_busy(SYSTEM_IDLE_CONDITIONS_BIT);
            spot_check_config *config = nvs_get_config();

            // TODO :: would be nice to have a 'previous_spot_name' key in config so we could pass to clear function
            // to smart erase with text inverse instead of block erasing max spot name width
            if (!full_clear) {
                screen_img_handler_clear_spot_name();
            }
            screen_img_handler_draw_spot_name(config->spot_name);
            sleep_handler_set_idle(SYSTEM_IDLE_CONDITIONS_BIT);
        }

        if (update_bits & UPDATE_CONDITIONS_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_CONDITIONS_BIT);
            // TODO :: don't support clearing spot name logic when changing location yet. Need a way to pass more info
            // to this case if we're clearing for a regular update or becase location changed and spot name will need to
            // be cleared too.
            if (!full_clear) {
                screen_img_handler_clear_conditions(true, true, true);
            }
            if (scheduler_success) {
                screen_img_handler_draw_conditions(&last_retrieved_conditions);
            } else {
                screen_img_handler_draw_conditions_error();
            }
            log_printf(LOG_LEVEL_INFO, "scheduler task updated conditions");
            sleep_handler_set_idle(SYSTEM_IDLE_CONDITIONS_BIT);
        }

        if (update_bits & UPDATE_TIDE_CHART_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_TIDE_CHART_BIT);
            if (!full_clear) {
                screen_img_handler_clear_screen_img(SCREEN_IMG_TIDE_CHART);
            }
            screen_img_handler_draw_screen_img(SCREEN_IMG_TIDE_CHART);
            log_printf(LOG_LEVEL_INFO, "scheduler task updated tide chart");
            sleep_handler_set_idle(SYSTEM_IDLE_TIDE_CHART_BIT);
        }

        if (update_bits & UPDATE_SWELL_CHART_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_SWELL_CHART_BIT);
            if (!full_clear) {
                screen_img_handler_clear_screen_img(SCREEN_IMG_SWELL_CHART);
            }
            screen_img_handler_draw_screen_img(SCREEN_IMG_SWELL_CHART);
            log_printf(LOG_LEVEL_INFO, "scheduler task updated swell chart");
            sleep_handler_set_idle(SYSTEM_IDLE_SWELL_CHART_BIT);
        }

        /***************************************
         * Render section
         **************************************/
        if (update_bits) {
            // If any other bits besides time are set, mark full screen as dirty so it refreshes all faded pixels
            if (update_bits & ~(UPDATE_TIME_BIT)) {
                screen_img_handler_mark_all_lines_dirty();
            }

            screen_img_handler_render(__func__, __LINE__);
        }
    }
}

static void scheduler_trigger_ota_check() {
    xTaskNotify(scheduler_task_handle, CHECK_OTA_BIT, eSetBits);
}

void scheduler_trigger_time_update() {
    xTaskNotify(scheduler_task_handle, UPDATE_TIME_BIT, eSetBits);
}

void scheduler_trigger_spot_name_update() {
    xTaskNotify(scheduler_task_handle, UPDATE_SPOT_NAME_BIT, eSetBits);
}

void scheduler_trigger_conditions_update() {
    xTaskNotify(scheduler_task_handle, UPDATE_CONDITIONS_BIT, eSetBits);
}

void scheduler_trigger_tide_chart_update() {
    xTaskNotify(scheduler_task_handle, UPDATE_TIDE_CHART_BIT, eSetBits);
}

void scheduler_trigger_swell_chart_update() {
    xTaskNotify(scheduler_task_handle, UPDATE_SWELL_CHART_BIT, eSetBits);
}

void scheduler_trigger_both_charts_update() {
    xTaskNotify(scheduler_task_handle, UPDATE_SWELL_CHART_BIT | UPDATE_TIDE_CHART_BIT, eSetBits);
}

void scheduler_task_init() {
}

void scheduler_task_start() {
    xTaskCreate(&scheduler_task,
                "scheduler-update",
                SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 4,
                NULL,
                tskIDLE_PRIORITY,
                &scheduler_task_handle);
}
