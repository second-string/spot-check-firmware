#include <string.h>
#include <time.h>

#include "esp_wifi.h"
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
#define NUM_DISCRETE_UPDATES 6

#define OTA_CHECK_INTERVAL_SECONDS (CONFIG_OTA_CHECK_INTERVAL_HOURS * MINS_PER_HOUR * SECS_PER_MIN)
#define NETWORK_CHECK_INTERVAL_SECONDS (30)

#define UPDATE_CONDITIONS_BIT (1 << 0)
#define UPDATE_TIDE_CHART_BIT (1 << 1)
#define UPDATE_SWELL_CHART_BIT (1 << 2)
#define UPDATE_TIME_BIT (1 << 3)
#define UPDATE_SPOT_NAME_BIT (1 << 4)
#define CHECK_OTA_BIT (1 << 5)
#define CHECK_NETWORK_BIT (1 << 6)

typedef struct {
    char   name[14];
    time_t update_interval_secs;
    time_t last_executed_epoch_secs;
    bool   force_next_update;
    bool   active;
    void (*execute)(void);
} differential_update_t;

typedef struct {
    char    name[14];
    uint8_t hour;
    uint8_t minute;
    uint8_t hour_last_executed;
    uint8_t minute_last_executed;
    bool    active;
    void (*execute)(void);
    bool force_next_update;
    bool force_on_transition_to_online;
} discrete_update_t;

static void scheduler_trigger_ota_check();
static void scheduler_trigger_network_check();

static TaskHandle_t          scheduler_task_handle;
static scheduler_mode_t      mode;
static volatile unsigned int seconds_elapsed;
static conditions_t          last_retrieved_conditions;

// Separate static pointer outside of regular list so we can manually deactivate it once it runs once
static differential_update_t *spot_name_differential_update;

// Execute function cannot be blocking! Will execute from 1 sec timer interrupt callback
static differential_update_t differential_updates[NUM_DIFFERENTIAL_UPDATES] = {
    {
        .name                 = "ota",
        .force_next_update    = false,
        .update_interval_secs = OTA_CHECK_INTERVAL_SECONDS,
        .active               = false,
        .execute              = scheduler_trigger_ota_check,
    },
    {
        .name                 = "network_check",
        .force_next_update    = false,
        .update_interval_secs = NETWORK_CHECK_INTERVAL_SECONDS,
        .active               = false,
        .execute              = scheduler_trigger_network_check,
    },
    {
        .name = "spot_name",  // use an extra diff update to hack in spot name renders. Needed for drawing spot name on
                              // first transition from boot offline mode into online mode
        .force_next_update    = false,
        .update_interval_secs = UINT32_MAX,  // ~136 years, will never be invoked outside of being forced
        .active               = false,
        .execute              = scheduler_trigger_spot_name_update,
    },
};

// Execute function cannot be blocking! Will execute from 1 sec timer interrupt callback
static discrete_update_t discrete_updates[NUM_DISCRETE_UPDATES] = {
    {
        .name                          = "time",
        .force_next_update             = false,
        .force_on_transition_to_online = false,
        .hour                          = 0xFF,  // wildcards, should update every minute every hour
        .minute                        = 0xFF,
        .hour_last_executed            = 0,
        .minute_last_executed          = 0,
        .active                        = false,
        .execute                       = scheduler_trigger_time_update,
    },
    {
        .name                          = "conditions",
        .force_next_update             = false,
        .force_on_transition_to_online = true,
        .hour                          = 0xFF,  // wildcard, runs on 5th minute of every hours
        .minute                        = 5,
        .hour_last_executed            = 0,
        .minute_last_executed          = 0,
        .active                        = false,
        .execute                       = scheduler_trigger_conditions_update,
    },
    {
        .name                          = "tide",
        .force_next_update             = false,
        .force_on_transition_to_online = true,
        .hour                          = 3,
        .minute                        = 0,
        .hour_last_executed            = 0,
        .minute_last_executed          = 0,
        .active                        = false,
        .execute                       = scheduler_trigger_tide_chart_update,
    },
    {
        .name                          = "swell_morning",
        .force_next_update             = false,
        .force_on_transition_to_online = true,
        .hour                          = 12,
        .minute                        = 0,
        .hour_last_executed            = 0,
        .minute_last_executed          = 0,
        .active                        = false,
        .execute                       = scheduler_trigger_swell_chart_update,
    },
    {
        .name                          = "swell_midday",
        .force_next_update             = false,
        .force_on_transition_to_online = false,
        .hour                          = 21,
        .minute                        = 0,
        .hour_last_executed            = 0,
        .minute_last_executed          = 0,
        .active                        = false,
        .execute                       = scheduler_trigger_swell_chart_update,
    },
    {
        .name                          = "swell_evening",
        .force_next_update             = true,
        .force_on_transition_to_online = false,
        .hour                          = 17,
        .minute                        = 0,
        .hour_last_executed            = 0,
        .minute_last_executed          = 0,
        .active                        = false,
        .execute                       = scheduler_trigger_swell_chart_update,
    },
};

// The update structs that should run in offline mode
const char *const offline_mode_update_names[] = {
    "time",
    "network_check",
};

/*
 * Returns whether or not the two time values (hour or min) match, OR always true if value is 0xFF wildcard (similar
 * to cron's '*')
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

    differential_update_t *diff_check = NULL;
    for (int i = 0; i < NUM_DIFFERENTIAL_UPDATES; i++) {
        diff_check = &differential_updates[i];
        if (!diff_check->active) {
            continue;
        }

        // If time differential has passed OR force execute flag set, execute and bring up to date.
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

    discrete_update_t *discrete_check = NULL;
    for (int i = 0; i < NUM_DISCRETE_UPDATES; i++) {
        discrete_check = &discrete_updates[i];
        if (!discrete_check->active) {
            continue;
        }

        // If (matching time has arrived AND the update has not yet been executed this minute) OR force execute flag
        // set, execute.
        if ((discrete_time_matches(now_local.tm_hour, discrete_check->hour) &&
             discrete_time_matches(now_local.tm_min, discrete_check->minute) &&
             (now_local.tm_hour != discrete_check->hour_last_executed ||
              now_local.tm_min != discrete_check->minute_last_executed)) ||
            discrete_check->force_next_update) {
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
            discrete_check->hour_last_executed   = now_local.tm_hour;
            discrete_check->minute_last_executed = now_local.tm_min;
            discrete_check->force_next_update    = false;
        }
    }
}

static void scheduler_task(void *args) {
    // Run polling timer every second that's responsible for triggering any differential or discrete updates that have
    // reached execution time. Timer only calls trigger function, task waits indefinitely on event bits from triggers.
    timer_info_handle scheduler_polling_timer_handle =
        timer_init("scheduler-polling", scheduler_polling_timer_callback, NULL, MS_PER_SEC);
    timer_reset(scheduler_polling_timer_handle, true);

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

        // If these three are all set, it means scheduler just got kicked back into online mode. Whether or not this is
        // from a boot, a discon/recon, or a first connection after boot error, do a full erase and redraw
        full_clear = (update_bits & UPDATE_CONDITIONS_BIT) && (update_bits & UPDATE_TIDE_CHART_BIT) &&
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

        if (update_bits & CHECK_NETWORK_BIT) {
            // Note: there can usually be a spurious extra network poll call when we come online for the first time,
            // because sntp will update from the successful http req, The huge delta between epoch and current time will
            // immediately trigger the network poll update struct, but it doesn't affect anything. Only occurs in case
            // of no stored sntp time AND not having internet available at first boot, not a common case.
            if (wifi_is_connected_to_network()) {
                log_printf(LOG_LEVEL_DEBUG, "Execing http internet healtcheck from network poll in offline mode");
                if (http_client_check_internet()) {
                    scheduler_set_online_mode();
                }
            } else {
                // Don't have to set anything - this kicks off another round of internal connecting with event loop. If
                // successful, it will set the event bits and the next time scheduler enterst this check network block
                // it will execute the above internet check
                log_printf(LOG_LEVEL_DEBUG, "Execing esp_wifi_connect from network poll in offline mode");
                esp_wifi_connect();
            }
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

            // We could be in this block from a manual direct trigger call or the async diff update struct. Either way,
            // make sure the diff struct is now deactivated so we don't run it anymore.
            if (spot_name_differential_update != NULL) {
                spot_name_differential_update->active = false;
            }
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

static void scheduler_trigger_network_check() {
    xTaskNotify(scheduler_task_handle, CHECK_NETWORK_BIT, eSetBits);
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

scheduler_mode_t scheduler_get_mode() {
    return mode;
}

void scheduler_set_offline_mode() {
    if (mode == SCHEDULER_MODE_OFFLINE) {
        return;
    }

    bool activate = false;
    for (int i = 0; i < NUM_DIFFERENTIAL_UPDATES; i++) {
        for (int j = 0; j < sizeof(offline_mode_update_names) / sizeof(char *); j++) {
            if (strcmp(offline_mode_update_names[j], differential_updates[i].name) == 0) {
                activate = true;
                break;
            }
        }

        if (activate) {
            differential_updates[i].active = true;
            log_printf(LOG_LEVEL_DEBUG, "Activated update struct '%s'", differential_updates[i].name);
        } else {
            differential_updates[i].active = false;
            log_printf(LOG_LEVEL_DEBUG, "Deactivated update struct '%s'", differential_updates[i].name);
        }
        activate = false;
    }

    for (int i = 0; i < NUM_DISCRETE_UPDATES; i++) {
        for (int j = 0; j < sizeof(offline_mode_update_names) / sizeof(char *); j++) {
            if (strcmp(offline_mode_update_names[j], discrete_updates[i].name) == 0) {
                activate = true;
                break;
            }
        }

        if (activate) {
            discrete_updates[i].active = true;
            log_printf(LOG_LEVEL_DEBUG, "Activated update struct '%s'", discrete_updates[i].name);
        } else {
            discrete_updates[i].active = false;
            log_printf(LOG_LEVEL_DEBUG, "Deactivated update struct '%s'", discrete_updates[i].name);
        }
        activate = false;
    }

    mode = SCHEDULER_MODE_OFFLINE;
}

void scheduler_set_online_mode() {
    if (mode == SCHEDULER_MODE_ONLINE) {
        return;
    }

    // Only have one update struct that shouldn't run in online mode so just hardcode it
    char *offline_only_update_name = "network_check";

    struct tm now_local;
    sntp_time_get_local_time(&now_local);
    time_t now_epoch_secs = mktime(&now_local);

    for (int i = 0; i < NUM_DIFFERENTIAL_UPDATES; i++) {
        if (strcmp(offline_only_update_name, differential_updates[i].name) == 0) {
            differential_updates[i].active = false;
            log_printf(LOG_LEVEL_DEBUG, "Deactivated diff update struct '%s'", differential_updates[i].name);
        } else {
            // Set 'last execd' time to now. Diff updates are fundamentally low prio, so we always want to reset timer
            // to when we come online. Edge case bug here if device keeps losing and regaining connection before the
            // full period of each diff update has happened at least once. If we didn't do this, every one of these
            // would trigger when coming online for the first time because of the massive time delta. That results in
            // OTA triggering and fucking with the other network requests since it's not gated by lock.
            differential_updates[i].active                   = true;
            differential_updates[i].force_next_update        = false;
            differential_updates[i].last_executed_epoch_secs = now_epoch_secs;
            log_printf(LOG_LEVEL_DEBUG, "Activated diff update struct '%s'", differential_updates[i].name);
        }
    }

    for (int i = 0; i < NUM_DISCRETE_UPDATES; i++) {
        if (strcmp(offline_only_update_name, discrete_updates[i].name) == 0) {
            discrete_updates[i].active = false;
            log_printf(LOG_LEVEL_DEBUG, "Deactivated discrete update struct '%s'", discrete_updates[i].name);
        } else {
            discrete_updates[i].active = true;

            // Don't force specific discrete updates on activation. Otherwise we'd trigger things like all three swell
            // updates back to back (and an unnecessary time update but that's a bit less intrusive)
            discrete_updates[i].force_next_update = discrete_updates[i].force_on_transition_to_online;
            log_printf(LOG_LEVEL_DEBUG,
                       "Activated %s discrete update struct '%s'",
                       discrete_updates[i].force_on_transition_to_online ? "and forced" : "but did not force",
                       discrete_updates[i].name);
        }
    }

    mode = SCHEDULER_MODE_ONLINE;
}

void scheduler_task_init() {
    mode = SCHEDULER_MODE_INIT;
}

void scheduler_task_start() {
    // Print out all update structs and set them all to inactive. This lets the main.c boot process run unfettered
    // without the scheduler in offline mode testing a healthcheck before the main code is ready for it an mucking
    // everything up. main.c init function responsible for setting scheduler into offline or online mode no matter what,
    // otherwise scheduler will never run.
    log_printf(LOG_LEVEL_DEBUG, "List of all time differential updates:");
    differential_update_t *diff_check = NULL;
    for (int i = 0; i < NUM_DIFFERENTIAL_UPDATES; i++) {
        diff_check         = &differential_updates[i];
        diff_check->active = false;
        log_printf(LOG_LEVEL_DEBUG,
                   "'%s' executing every %.0f seconds",
                   diff_check->name,
                   diff_check->update_interval_secs);

        // Set the unique spot name update pointer on init to deactive the update struct after running once after boot
        if (strcmp("spot_name", diff_check->name) == 0) {
            spot_name_differential_update = diff_check;
        }
    }

    log_printf(LOG_LEVEL_DEBUG, "List of all discrete updates:");
    discrete_update_t *discrete_check = NULL;
    for (int i = 0; i < NUM_DISCRETE_UPDATES; i++) {
        discrete_check         = &discrete_updates[i];
        discrete_check->active = false;
        log_printf(LOG_LEVEL_DEBUG,
                   "'%s' executing at %u:%02u",
                   discrete_check->name,
                   discrete_check->hour,
                   discrete_check->minute);
    }

    xTaskCreate(&scheduler_task,
                "scheduler-update",
                SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 4,
                NULL,
                tskIDLE_PRIORITY,
                &scheduler_task_handle);
}
