#include <string.h>
#include <time.h>

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "memfault/panics/assert.h"
#include "memfault_interface.h"

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
#include "spot_check.h"
#include "timer.h"
#include "wifi.h"

#define TAG SC_TAG_SCHEDULER

#define NUM_DIFFERENTIAL_UPDATES 5
#define NUM_DISCRETE_UPDATES 9

#define OTA_CHECK_INTERVAL_SECONDS (CONFIG_OTA_CHECK_INTERVAL_HOURS * MINS_PER_HOUR * SECS_PER_MIN)
#define NETWORK_CHECK_INTERVAL_SECONDS (30)
#define MFLT_UPLOAD_INTERVAL_SECONDS (30 * SECS_PER_MIN)
#define SCREEN_DIRTY_INTERVAL_SECONDS (30 * SECS_PER_MIN)

#define UPDATE_CONDITIONS_BIT (1 << 0)
#define UPDATE_TIDE_CHART_BIT (1 << 1)
#define UPDATE_SWELL_CHART_BIT (1 << 2)
#define UPDATE_TIME_BIT (1 << 3)
#define UPDATE_SPOT_NAME_BIT (1 << 4)
#define CHECK_OTA_BIT (1 << 5)
#define CHECK_NETWORK_BIT (1 << 6)
#define SEND_MFLT_DATA_BIT (1 << 7)
#define UPDATE_DATE_BIT (1 << 8)
#define MARK_SCREEN_DIRTY_BIT (1 << 9)
#define CUSTOM_SCREEN_UPDATE_BIT (1 << 10)
#define UPDATE_WIND_CHART_BIT (1 << 11)

// Anything that causes a draw to the  screen needs to be added here. This exists so scheduler doesn't re-render screen
// for logical update structs like memfault or ota check
#define BITS_NEEDING_RENDER                                                                           \
    (UPDATE_CONDITIONS_BIT | UPDATE_TIDE_CHART_BIT | UPDATE_SWELL_CHART_BIT | UPDATE_WIND_CHART_BIT | \
     UPDATE_TIME_BIT | UPDATE_SPOT_NAME_BIT | UPDATE_DATE_BIT | CUSTOM_SCREEN_UPDATE_BIT)

/*
 * Exists only to easily index into the discrete/diff update struct arrays in order to perform special handling for
 * specific entries (rather than always string comparing the name)
 */
typedef enum {
    DISCRETE_UPDATE_INDEX_TIME = 0,
    DISCRETE_UPDATE_INDEX_DATE,
    DISCRETE_UPDATE_INDEX_CONDITIONS,
    DISCRETE_UPDATE_INDEX_TIDE_CHART,
    DISCRETE_UPDATE_INDEX_SWELL_CHART_MORNING,
    DISCRETE_UPDATE_INDEX_SWELL_CHART_MIDDAY,
    DISCRETE_UPDATE_INDEX_SWELL_CHART_EVENING,
    DISCRETE_UPDATE_INDEX_SPOT_NAME,
    DISCRETE_UPDATE_INDEX_WIND_CHART,

    DISCRETE_UPDATE_INDEX_COUNT,
} discrete_update_index_t;

typedef enum {
    DIFFERENTIAL_UPDATE_INDEX_OTA = 0,
    DIFFERENTIAL_UPDATE_INDEX_NETWORK_CHECK,
    DIFFERENTIAL_UPDATE_INDEX_MFLT_UPLOAD,
    DIFFERENTIAL_UPDATE_INDEX_DIRTY_SCREEN,
    DIFFERENTIAL_UPDATE_INDEX_CUSTOM_SCREEN_UPDATE,

    DIFFERENTIAL_UPDATE_INDEX_COUNT,
} differential_update_index_t;

typedef struct {
    char              debug_name[21];  // only used for logging/debugging, not necessary
    time_t            update_interval_secs;
    time_t            last_executed_epoch_secs;
    bool              force_next_update;  // mutable flag at runtime to indicate whether this should be run next trigger
    bool              force_on_transition_to_online;  // set at compile time, should not be changed ever
    bool              active;
    spot_check_mode_t active_operating_mode;
    void (*execute)(void);
} differential_update_t;

typedef struct {
    char              debug_name[14];  // only used for logging/debugging, not necessary
    uint8_t           hour;
    uint8_t           minute;
    struct tm         last_executed;
    bool              active;
    spot_check_mode_t active_operating_mode;
    void (*execute)(void);
    bool force_next_update;              // mutable flag at runtime to indicate whether this should be run next trigger
    bool force_on_transition_to_online;  // set at compile time, should not be changed ever
} discrete_update_t;

static TaskHandle_t          scheduler_task_handle;
static scheduler_mode_t      scheduler_mode;
static volatile unsigned int seconds_elapsed;
static conditions_t          last_retrieved_conditions;
static uint32_t              scheduled_bits;

// Execute function cannot be blocking! Will execute from 1 sec timer interrupt callback
static differential_update_t differential_updates[NUM_DIFFERENTIAL_UPDATES] = {
    [DIFFERENTIAL_UPDATE_INDEX_OTA] =
        {
            .debug_name                    = "ota",
            .force_next_update             = false,
            .force_on_transition_to_online = false,
            .update_interval_secs          = OTA_CHECK_INTERVAL_SECONDS,
            .active                        = false,
            .active_operating_mode         = 0xFF,
            .execute                       = scheduler_schedule_ota_check,
        },
    [DIFFERENTIAL_UPDATE_INDEX_NETWORK_CHECK] =
        {
            .debug_name                    = "network_check",
            .force_next_update             = false,
            .force_on_transition_to_online = false,
            .update_interval_secs          = NETWORK_CHECK_INTERVAL_SECONDS,
            .active                        = false,
            .active_operating_mode         = 0xFF,
            .execute                       = scheduler_schedule_network_check,
        },
    [DIFFERENTIAL_UPDATE_INDEX_MFLT_UPLOAD] =
        {
            .debug_name        = "mflt_upload",
            .force_next_update = false,
            .force_on_transition_to_online =
                false,  // do not set this true - it will run immediately on transition from init->online mode at boot,
                        // and if it has a large payload to upload, it will break any subsequent network requests (like
                        // custom screen download) because we can't block on the upload, it just triggers the internal
                        // http client in memfault code
            .update_interval_secs  = MFLT_UPLOAD_INTERVAL_SECONDS,
            .active                = false,
            .active_operating_mode = 0xFF,
            .execute               = scheduler_schedule_mflt_upload,
        },
    [DIFFERENTIAL_UPDATE_INDEX_DIRTY_SCREEN] =
        {
            .debug_name                    = "dirty_screen",
            .force_next_update             = false,
            .force_on_transition_to_online = false,
            .update_interval_secs          = SCREEN_DIRTY_INTERVAL_SECONDS,
            .active                        = false,
            .active_operating_mode         = SPOT_CHECK_MODE_WEATHER,
            .execute                       = scheduler_schedule_screen_dirty,
        },
    [DIFFERENTIAL_UPDATE_INDEX_CUSTOM_SCREEN_UPDATE] =
        {
            .debug_name        = "custom_screen_update",
            .force_next_update = false,
            .force_on_transition_to_online =
                true,                    // mostly need it to force run immediately on init->online transition
            .update_interval_secs  = 0,  // set from config value in scheduler start fun
            .active                = false,
            .active_operating_mode = SPOT_CHECK_MODE_CUSTOM,
            .execute               = scheduler_schedule_custom_screen_update,
        },
};

// Execute function cannot be blocking! Will execute from 1 sec timer interrupt callback
static discrete_update_t discrete_updates[NUM_DISCRETE_UPDATES] = {
    [DISCRETE_UPDATE_INDEX_TIME] =
        {
            .debug_name                    = "time",
            .force_next_update             = false,
            .force_on_transition_to_online = true,
            .hour                          = 0xFF,  // wildcards, should update every minute every hour
            .minute                        = 0xFF,
            .last_executed                 = {0},
            .active                        = false,
            .active_operating_mode         = SPOT_CHECK_MODE_WEATHER,
            .execute                       = scheduler_schedule_time_update,
        },
    [DISCRETE_UPDATE_INDEX_DATE] =
        {
            .debug_name                    = "date",
            .force_next_update             = false,
            .force_on_transition_to_online = true,
            .hour                          = 0,
            .minute                        = 1,
            .last_executed                 = {0},
            .active                        = false,
            .active_operating_mode         = SPOT_CHECK_MODE_WEATHER,
            .execute                       = scheduler_schedule_date_update,
        },
    [DISCRETE_UPDATE_INDEX_CONDITIONS] =
        {
            .debug_name                    = "conditions",
            .force_next_update             = false,
            .force_on_transition_to_online = true,
            .hour                          = 0xFF,  // wildcard, runs on 5th minute of every hours
            .minute                        = 5,
            .last_executed                 = {0},
            .active                        = false,
            .active_operating_mode         = SPOT_CHECK_MODE_WEATHER,
            .execute                       = scheduler_schedule_conditions_update,
        },
    [DISCRETE_UPDATE_INDEX_TIDE_CHART] =
        {
            .debug_name                    = "tide",
            .force_next_update             = false,
            .force_on_transition_to_online = true,
            .hour                          = 3,
            .minute                        = 0,
            .last_executed                 = {0},
            .active                        = false,
            .active_operating_mode         = SPOT_CHECK_MODE_WEATHER,
            .execute                       = scheduler_schedule_tide_chart_update,
        },
    [DISCRETE_UPDATE_INDEX_SWELL_CHART_MORNING] =
        {
            .debug_name                    = "swell_morning",
            .force_next_update             = false,
            .force_on_transition_to_online = true,
            .hour                          = 3,
            .minute                        = 0,
            .last_executed                 = {0},
            .active                        = false,
            .active_operating_mode         = SPOT_CHECK_MODE_WEATHER,
            .execute                       = scheduler_schedule_swell_chart_update,
        },
    [DISCRETE_UPDATE_INDEX_SWELL_CHART_MIDDAY] =
        {
            .debug_name                    = "swell_midday",
            .force_next_update             = false,
            .force_on_transition_to_online = false,
            .hour                          = 12,
            .minute                        = 0,
            .last_executed                 = {0},
            .active                        = false,
            .active_operating_mode         = SPOT_CHECK_MODE_WEATHER,
            .execute                       = scheduler_schedule_swell_chart_update,
        },
    [DISCRETE_UPDATE_INDEX_SWELL_CHART_EVENING] =
        {
            .debug_name                    = "swell_evening",
            .force_next_update             = false,
            .force_on_transition_to_online = false,
            .hour                          = 17,
            .minute                        = 0,
            .last_executed                 = {0},
            .active                        = false,
            .active_operating_mode         = SPOT_CHECK_MODE_WEATHER,
            .execute                       = scheduler_schedule_swell_chart_update,
        },
    [DISCRETE_UPDATE_INDEX_SPOT_NAME] =
        {
            .debug_name = "spot_name",  // use an extra discrete update to hack in spot name renders. Needed for drawing
                                        // spot name on first transition from boot offline mode into online mode
            .force_next_update             = false,
            .force_on_transition_to_online = true,
            .hour                          = 0xEE,  // this hour will obviously never be hit
            .minute                        = 0xEE,  // this minute will obviously never be hit
            .last_executed                 = {0},
            .active                        = false,
            .active_operating_mode         = SPOT_CHECK_MODE_WEATHER,
            .execute                       = scheduler_schedule_spot_name_update,
        },
    [DISCRETE_UPDATE_INDEX_WIND_CHART] =
        {
            .debug_name                    = "wind",
            .force_next_update             = false,
            .force_on_transition_to_online = true,
            .hour                          = 0xFF,  // wildcard, runs on 5th minute of every hours
            .minute                        = 5,
            .last_executed                 = {0},
            .active                        = false,
            .active_operating_mode         = SPOT_CHECK_MODE_WEATHER,
            .execute                       = scheduler_schedule_wind_chart_update,
        },

    // NOTE :: MAKE SURE update_struct_is_chart IS UPDATED IF ANY NEW UPDATE STRUCTS FOR CHARTS ARE ADDED
};

// The update structs that should run in offline mode. Right now it's only one, so this can be typed specifically for
// that enum type.
// TODO :: need a way for online -> offline to keep updating time after we've been online and rendered full homescreen
// once. Can't just add time here though, because if healthcheck or any of the network requests during boot kick into
// offline mode, then time starts rendering even if it isn't correct
const differential_update_index_t offline_mode_update_indexes[] = {
    DIFFERENTIAL_UPDATE_INDEX_NETWORK_CHECK,
};

/*
 * Returns whether or not the two time values (hour or min) match, OR always true if value is 0xFF wildcard (similar
 * to cron's '*')
 */
static inline bool discrete_time_matches(int current, uint8_t check) {
    return (current == check) || (check == 0xFF);
}

/*
 * Similar to the discrete time check, this returns true if the two modes match, or if the structs active_operating_mode
 * is the 0xFF wildcard indicating that it should execute regardless of the operating mode (like memfault for example)
 */
static inline bool active_operating_mode_matches(spot_check_mode_t current_mode,
                                                 spot_check_mode_t active_operating_mode) {
    return (current_mode == active_operating_mode) || (active_operating_mode == 0xFF);
}

/*
 * Returns true if the two tm structs match from day to minutes granularity inclusive. Used to prevent discrete checks
 * from executing for every second of a time matching their min/hour fields, but also allow for the advancement of a day
 * to differentiate between last_executed times so it once again triggers the next day
 */
static inline bool discrete_time_not_yet_executed_today(struct tm now, struct tm last_executed) {
    return now.tm_wday != last_executed.tm_wday || now.tm_hour != last_executed.tm_hour ||
           now.tm_min != last_executed.tm_min;
}

static inline bool active_chart_matches(spot_check_config_t *config, discrete_update_index_t index) {
    screen_img_t screen_img_to_compare;
    switch (index) {
        case DISCRETE_UPDATE_INDEX_TIDE_CHART:
            screen_img_to_compare = SCREEN_IMG_TIDE_CHART;
            break;
        case DISCRETE_UPDATE_INDEX_SWELL_CHART_MORNING:
        case DISCRETE_UPDATE_INDEX_SWELL_CHART_MIDDAY:
        case DISCRETE_UPDATE_INDEX_SWELL_CHART_EVENING:
            screen_img_to_compare = SCREEN_IMG_SWELL_CHART;
            break;
        case DISCRETE_UPDATE_INDEX_WIND_CHART:
            screen_img_to_compare = SCREEN_IMG_WIND_CHART;
            break;
        default:
            MEMFAULT_ASSERT(0);
    }

    return config->active_chart_1 == screen_img_to_compare || config->active_chart_2 == screen_img_to_compare;
}

/*
 * Returns true if the arg index is one of the chart update structs.
 * THIS FUNCTION NEEDS TO BE UPDATED IF A NEW CHART UPDATE STRUCT IS EVER ADDED.
 * I really don't like this implementation, too easy to forget, but the best I can think of for now.
 */
static inline bool update_struct_is_chart(discrete_update_index_t index) {
    return index == DISCRETE_UPDATE_INDEX_TIDE_CHART || index == DISCRETE_UPDATE_INDEX_SWELL_CHART_MORNING ||
           index == DISCRETE_UPDATE_INDEX_SWELL_CHART_MIDDAY || index == DISCRETE_UPDATE_INDEX_SWELL_CHART_EVENING ||
           index == DISCRETE_UPDATE_INDEX_WIND_CHART;
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
                       diff_check->debug_name,
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

        // If (matching time has arrived AND the update has not yet been executed today (necessary for preventing
        // multiple executions in the same minute)) OR force execute flag set, execute.
        if ((discrete_time_matches(now_local.tm_hour, discrete_check->hour) &&
             discrete_time_matches(now_local.tm_min, discrete_check->minute) &&
             discrete_time_not_yet_executed_today(now_local, discrete_check->last_executed)) ||
            discrete_check->force_next_update) {
            log_printf(LOG_LEVEL_DEBUG,
                       "Executing discrete update '%s' (curr hr: %u, curr min: %u, check hr: %u, check min: %u, "
                       "force: %u)",
                       discrete_check->debug_name,
                       now_local.tm_hour,
                       now_local.tm_min,
                       discrete_check->hour,
                       discrete_check->minute,
                       discrete_check->force_next_update);

            discrete_check->execute();
            discrete_check->last_executed     = now_local;
            discrete_check->force_next_update = false;
        }
    }

    // After all structs have scheduled their update bits, kick scheduler. This prevents the race condition of freertos
    // context switching to the scheduler task before the timer task is done scheduling everything
    scheduler_trigger();
}

static void scheduler_task(void *args) {
    // Run polling timer every second that's responsible for triggering any differential or discrete updates that have
    // reached execution time. Timer only calls trigger function, task waits indefinitely on event bits from triggers.
    timer_info_handle scheduler_polling_timer_handle =
        timer_local_init("scheduler-polling", scheduler_polling_timer_callback, NULL, MS_PER_SEC);
    timer_reset(scheduler_polling_timer_handle, true);

    uint32_t update_bits        = 0;
    bool     full_clear         = false;
    bool     scheduler_success  = false;
    bool     force_screen_dirty = false;
    while (1) {
        // Wait forever until a notification received. Clears all bits on exit since we'll handle every set bit in one
        // go
        xTaskNotifyWait(0x0, UINT32_MAX, &update_bits, portMAX_DELAY);

        log_printf(LOG_LEVEL_DEBUG,
                   "scheduler task received task notification of value 0x%02X, updating accordingly",
                   update_bits);

        spot_check_config_t *config = nvs_get_config();
        switch (config->operating_mode) {
            case SPOT_CHECK_MODE_WEATHER:
                // If these three are all set, it means scheduler just got kicked back into online mode. Whether or not
                // this is from a boot, a discon/recon, or a first connection after boot error, do a full erase and
                // redraw
                full_clear = (update_bits & UPDATE_CONDITIONS_BIT) && (update_bits & UPDATE_TIDE_CHART_BIT) &&
                             (update_bits & UPDATE_SWELL_CHART_BIT);
                break;
            case SPOT_CHECK_MODE_CUSTOM:
                // For now we always full clear in custom screen mode if this is a screen image update
                full_clear = update_bits & CUSTOM_SCREEN_UPDATE_BIT;
                break;
            default:
                MEMFAULT_ASSERT(0);
        }

        /***************************************
         * Network update section
         * Gate every network request block with a check for scheduler mode so one failed request will short circuit any
         * remaining ones if their update bits are also set
         **************************************/
        if (update_bits & UPDATE_CONDITIONS_BIT && scheduler_get_mode() != SCHEDULER_MODE_OFFLINE) {
            sleep_handler_set_busy(SYSTEM_IDLE_CONDITIONS_BIT);
            conditions_t new_conditions = {0};
            scheduler_success           = spot_check_download_and_save_conditions(&new_conditions);
            if (scheduler_success) {
                memcpy(&last_retrieved_conditions, &new_conditions, sizeof(conditions_t));
            }
            sleep_handler_set_idle(SYSTEM_IDLE_CONDITIONS_BIT);
        }

        if (update_bits & UPDATE_TIDE_CHART_BIT && scheduler_get_mode() != SCHEDULER_MODE_OFFLINE) {
            sleep_handler_set_busy(SYSTEM_IDLE_TIDE_CHART_BIT);
            screen_img_handler_download_and_save(SCREEN_IMG_TIDE_CHART);
            sleep_handler_set_idle(SYSTEM_IDLE_TIDE_CHART_BIT);
        }

        if (update_bits & UPDATE_SWELL_CHART_BIT && scheduler_get_mode() != SCHEDULER_MODE_OFFLINE) {
            sleep_handler_set_busy(SYSTEM_IDLE_SWELL_CHART_BIT);
            screen_img_handler_download_and_save(SCREEN_IMG_SWELL_CHART);
            sleep_handler_set_idle(SYSTEM_IDLE_SWELL_CHART_BIT);
        }

        if (update_bits & UPDATE_WIND_CHART_BIT && scheduler_get_mode() != SCHEDULER_MODE_OFFLINE) {
            sleep_handler_set_busy(SYSTEM_IDLE_WIND_CHART_BIT);
            screen_img_handler_download_and_save(SCREEN_IMG_WIND_CHART);
            sleep_handler_set_idle(SYSTEM_IDLE_WIND_CHART_BIT);
        }

        // Note: MUST come before OTA bit right now. Special case timer on boot where we delay first memfault and ota
        // run triggers both ota task and memfault upload simultaneously. If order is reversed, async ota task will be
        // running http reqs and memfault upload http req will fail. Will be fixed with refactor / improvement of http
        // req queueing
        if (update_bits & SEND_MFLT_DATA_BIT) {
            // TODO :: brutally blocking right now for big coredump uploads
            // Don't care about return value, all error-handling internal
            (void)memfault_interface_post_data();
        }

        if (update_bits & CHECK_OTA_BIT && scheduler_get_mode() != SCHEDULER_MODE_OFFLINE) {
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

        if (update_bits & CUSTOM_SCREEN_UPDATE_BIT && scheduler_get_mode() != SCHEDULER_MODE_OFFLINE) {
            sleep_handler_set_busy(SYSTEM_IDLE_CUSTOM_SCREEN_BIT);
            screen_img_handler_download_and_save(SCREEN_IMG_CUSTOM_SCREEN);
            sleep_handler_set_idle(SYSTEM_IDLE_CUSTOM_SCREEN_BIT);
        }

        /***************************************
         * Framebuffer update section
         **************************************/
        if (full_clear) {
            log_printf(LOG_LEVEL_DEBUG, "Performing full screen clear from scheduler_task");
            spot_check_full_clear();
        }

        if (update_bits & UPDATE_TIME_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_TIME_BIT);
            if (!full_clear) {
                spot_check_clear_time();
            }

            spot_check_draw_time();
            spot_check_mark_time_dirty();
            sleep_handler_set_idle(SYSTEM_IDLE_TIME_BIT);
        }

        if (update_bits & UPDATE_DATE_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_TIME_BIT);
            if (!full_clear) {
                spot_check_clear_date();
            }

            spot_check_draw_date();
            sleep_handler_set_idle(SYSTEM_IDLE_TIME_BIT);
        }

        if (update_bits & UPDATE_SPOT_NAME_BIT) {
            // Slightly unique case as in it requires no network update, just used as a display update trigger
            sleep_handler_set_busy(SYSTEM_IDLE_CONDITIONS_BIT);

            // TODO :: would be nice to have a 'previous_spot_name' key in config so we could pass to clear function
            // to smart erase with text inverse instead of block erasing max spot name width
            if (!full_clear) {
                spot_check_clear_spot_name();
            }
            spot_check_draw_spot_name(config->spot_name);
            sleep_handler_set_idle(SYSTEM_IDLE_CONDITIONS_BIT);

            // This should only ever run once, so whether it was triggered from initial boot or a new config spot, clear
            // the active flag here until it's manually triggered again.
            discrete_updates[DISCRETE_UPDATE_INDEX_SPOT_NAME].active = false;
        }

        if (update_bits & UPDATE_CONDITIONS_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_CONDITIONS_BIT);
            // TODO :: don't support clearing spot name logic when changing location yet. Need a way to pass more info
            // to this case if we're clearing for a regular update or becase location changed and spot name will need to
            // be cleared too.
            if (!full_clear) {
                spot_check_clear_conditions(true, true, true);
            }
            if (scheduler_success) {
                spot_check_draw_conditions(&last_retrieved_conditions);
            } else {
                spot_check_draw_conditions_error();
            }
            log_printf(LOG_LEVEL_INFO, "scheduler task updated conditions");
            sleep_handler_set_idle(SYSTEM_IDLE_CONDITIONS_BIT);
        }

        if (update_bits & UPDATE_TIDE_CHART_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_TIDE_CHART_BIT);
            if (!full_clear) {
                screen_img_handler_clear_chart(SCREEN_IMG_TIDE_CHART);
            }
            screen_img_handler_draw_chart(SCREEN_IMG_TIDE_CHART);
            log_printf(LOG_LEVEL_INFO, "scheduler task updated tide chart");
            sleep_handler_set_idle(SYSTEM_IDLE_TIDE_CHART_BIT);
        }

        if (update_bits & UPDATE_SWELL_CHART_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_SWELL_CHART_BIT);
            if (!full_clear) {
                screen_img_handler_clear_chart(SCREEN_IMG_SWELL_CHART);
            }
            screen_img_handler_draw_chart(SCREEN_IMG_SWELL_CHART);
            log_printf(LOG_LEVEL_INFO, "scheduler task updated swell chart");
            sleep_handler_set_idle(SYSTEM_IDLE_SWELL_CHART_BIT);
        }

        if (update_bits & UPDATE_WIND_CHART_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_WIND_CHART_BIT);
            if (!full_clear) {
                screen_img_handler_clear_chart(SCREEN_IMG_WIND_CHART);
            }
            screen_img_handler_draw_chart(SCREEN_IMG_WIND_CHART);
            log_printf(LOG_LEVEL_INFO, "scheduler task updated wind chart");
            sleep_handler_set_idle(SYSTEM_IDLE_WIND_CHART_BIT);
        }

        if (update_bits & MARK_SCREEN_DIRTY_BIT) {
            // Manually mark the full framebuffer dirty to prevent long-term gray-in happening on longer-static areas of
            // the screen (aka everything but the time & conditions)
            force_screen_dirty = true;
            log_printf(LOG_LEVEL_INFO,
                       "Flag to force mark framebuffer dirty received in scheduler, inverting framebuffer to re-render "
                       "full screen");
        }

        if (update_bits & CUSTOM_SCREEN_UPDATE_BIT) {
            sleep_handler_set_busy(SYSTEM_IDLE_CUSTOM_SCREEN_BIT);
            if (!full_clear) {
                screen_img_handler_clear_screen_img(SCREEN_IMG_CUSTOM_SCREEN);
            }
            screen_img_handler_draw_screen_img(SCREEN_IMG_CUSTOM_SCREEN);
            log_printf(LOG_LEVEL_INFO, "scheduler task updated custom screen");
            sleep_handler_set_idle(SYSTEM_IDLE_CUSTOM_SCREEN_BIT);
        }

        /***************************************
         * Render section
         **************************************/
        if (update_bits & BITS_NEEDING_RENDER) {
            // If either the force dirty flag is set or ANY bits requiring a screen render besides time are set, mark
            // entire framebuffer as dirty
            if (force_screen_dirty || (update_bits & ~UPDATE_TIME_BIT)) {
                force_screen_dirty = false;
                spot_check_mark_all_lines_dirty();
            }

            spot_check_render();
        }
    }
}

/*
 * Trigger the task notification for all bits that have accumulated from calls to scheduler_schedule_* functions.
 * Grouping them and triggering one time prevents non-deterministic race conditions with the FreeRTOS scheduler as more
 * and more bits are set. This was causing inconsistent screen renders on boot where there's a bunch of stuff scheduled
 * at once
 */
void scheduler_trigger() {
    // Don't trigger if nothing's set, this would result in triggering full task every second at end of update struct
    // timer func
    if (scheduled_bits == 0x0) {
        return;
    }

    uint32_t saved_scheduled_bits = scheduled_bits;

    // TODO :: this should be wrapped in a lock to avoid the race condition of calling notify, another task setting a
    // bit, and then the scheduler task taking over and not having that set bit in it's bits. Then we'd erase
    // scheduled_bits below and it would be lost
    xTaskNotify(scheduler_task_handle, scheduled_bits, eSetBits);
    scheduled_bits = 0x0;

    // After important work is done, trying to minimize time between notify call and setting bits = 0
    log_printf(LOG_LEVEL_DEBUG, "Triggered scheduler task with bits 0x%08X", saved_scheduled_bits);
}

void scheduler_schedule_network_check() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (network check)", CHECK_NETWORK_BIT);
    scheduled_bits |= CHECK_NETWORK_BIT;
}

void scheduler_schedule_time_update() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (time)", UPDATE_TIME_BIT);
    scheduled_bits |= UPDATE_TIME_BIT;
}

void scheduler_schedule_date_update() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (date)", UPDATE_DATE_BIT);
    scheduled_bits |= UPDATE_DATE_BIT;
}

void scheduler_schedule_spot_name_update() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (spot name)", UPDATE_SPOT_NAME_BIT);
    scheduled_bits |= UPDATE_SPOT_NAME_BIT;
}

void scheduler_schedule_conditions_update() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (conditions)", UPDATE_CONDITIONS_BIT);
    scheduled_bits |= UPDATE_CONDITIONS_BIT;
}

void scheduler_schedule_tide_chart_update() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (tide chart)", UPDATE_TIDE_CHART_BIT);
    scheduled_bits |= UPDATE_TIDE_CHART_BIT;
}

void scheduler_schedule_swell_chart_update() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (swell chart)", UPDATE_SWELL_CHART_BIT);
    scheduled_bits |= UPDATE_SWELL_CHART_BIT;
}

void scheduler_schedule_wind_chart_update() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (wind chart)", UPDATE_WIND_CHART_BIT);
    scheduled_bits |= UPDATE_WIND_CHART_BIT;
}

void scheduler_schedule_both_charts_update() {
    uint32_t             chart_bits = 0x0;
    spot_check_config_t *config     = nvs_get_config();

    if (config->active_chart_1 == SCREEN_IMG_TIDE_CHART || config->active_chart_2 == SCREEN_IMG_TIDE_CHART) {
        chart_bits |= UPDATE_TIDE_CHART_BIT;
    }

    if (config->active_chart_1 == SCREEN_IMG_SWELL_CHART || config->active_chart_2 == SCREEN_IMG_SWELL_CHART) {
        chart_bits |= UPDATE_SWELL_CHART_BIT;
    }

    if (config->active_chart_1 == SCREEN_IMG_WIND_CHART || config->active_chart_2 == SCREEN_IMG_WIND_CHART) {
        chart_bits |= UPDATE_WIND_CHART_BIT;
    }

    log_printf(LOG_LEVEL_DEBUG, "Scheduling bits 0x%08X (chart 1 and 2)", chart_bits);
    scheduled_bits |= chart_bits;
}

void scheduler_schedule_ota_check() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (ota)", CHECK_OTA_BIT);
    scheduled_bits |= CHECK_OTA_BIT;
}

void scheduler_schedule_mflt_upload() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (memfault)", SEND_MFLT_DATA_BIT);
    scheduled_bits |= SEND_MFLT_DATA_BIT;
}

void scheduler_schedule_screen_dirty() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (mark screen dirty)", MARK_SCREEN_DIRTY_BIT);
    scheduled_bits |= MARK_SCREEN_DIRTY_BIT;
}

void scheduler_schedule_custom_screen_update() {
    log_printf(LOG_LEVEL_DEBUG, "Scheduling bit 0x%08X (custom screen update)", CUSTOM_SCREEN_UPDATE_BIT);
    scheduled_bits |= CUSTOM_SCREEN_UPDATE_BIT;
}

scheduler_mode_t scheduler_get_mode() {
    return scheduler_mode;
}

void scheduler_set_offline_mode() {
    // Once we're in offline mode, every healthcheck will re-execute this function as requests still fail. Prevent
    // unnecessary looping and updating of the structs
    if (scheduler_mode == SCHEDULER_MODE_OFFLINE) {
        return;
    }

    bool activate = false;
    for (int i = 0; i < NUM_DIFFERENTIAL_UPDATES; i++) {
        for (int j = 0; j < sizeof(offline_mode_update_indexes) / sizeof(differential_update_index_t); j++) {
            if (offline_mode_update_indexes[j] == i) {
                activate = true;
                break;
            }
        }

        if (activate) {
            differential_updates[i].active = true;
            log_printf(LOG_LEVEL_DEBUG, "Activated update struct '%s'", differential_updates[i].debug_name);
        } else {
            differential_updates[i].active = false;
            log_printf(LOG_LEVEL_DEBUG, "Deactivated update struct '%s'", differential_updates[i].debug_name);
        }
        activate = false;
    }

    for (int i = 0; i < NUM_DISCRETE_UPDATES; i++) {
        // NOTE :: currently there's only one offline activated struct and it's differential. If that changes this needs
        // to be updated for (int j = 0; j < sizeof(offline_mode_update_names) / sizeof(char *); j++) {
        //     if (strcmp(offline_mode_update_names[j], discrete_updates[i].name) == 0) {
        activate = true;
        break;
        // }
        // }

        if (activate) {
            discrete_updates[i].active = true;
            log_printf(LOG_LEVEL_DEBUG, "Activated update struct '%s'", discrete_updates[i].debug_name);
        } else {
            discrete_updates[i].active = false;
            log_printf(LOG_LEVEL_DEBUG, "Deactivated update struct '%s'", discrete_updates[i].debug_name);
        }
        activate = false;
    }

    scheduler_mode = SCHEDULER_MODE_OFFLINE;
}

/*
 * Disable everything except for time update. We're running OTA and that means no network requests will work, and
 * ideally then we reboot normally. If OTA fails we have bigger problems
 */
void scheduler_set_ota_mode() {
    log_printf(LOG_LEVEL_WARN, "%s called", __func__);

    if (scheduler_mode == SCHEDULER_MODE_OTA) {
        return;
    }

    for (int i = 0; i < NUM_DIFFERENTIAL_UPDATES; i++) {
        differential_updates[i].active = false;
        log_printf(LOG_LEVEL_DEBUG, "Deactivated update struct '%s'", differential_updates[i].debug_name);
    }

    for (int i = 0; i < NUM_DISCRETE_UPDATES; i++) {
        // Don't disable time or date
        if (i != DISCRETE_UPDATE_INDEX_TIME && i != DISCRETE_UPDATE_INDEX_DATE) {
            discrete_updates[i].active = false;
            log_printf(LOG_LEVEL_DEBUG, "Deactivated update struct '%s'", discrete_updates[i].debug_name);
        }
    }

    scheduler_mode = SCHEDULER_MODE_OTA;
}

void scheduler_set_online_mode() {
    log_printf(LOG_LEVEL_WARN, "%s called", __func__);

    if (scheduler_mode == SCHEDULER_MODE_ONLINE) {
        return;
    }

    bool respect_force_flags = true;
    if (scheduler_mode == SCHEDULER_MODE_OTA) {
        // If we're returning to online from OTA, don't allow any update structs to force an update. We know that OTA is
        // a short blip, so this prevents running all the forces for discrete/diff structs after popping into OTA for
        // max a minute or so.
        respect_force_flags = false;
    } else {
        // Who knows what error or random state screen was in from init/offline mode. Full clear, show fetching
        // conditions, and kick everything off.
        spot_check_full_clear();
        spot_check_draw_fetching_data_text();
        spot_check_render();
        respect_force_flags = true;
    }

    struct tm now_local;
    sntp_time_get_local_time(&now_local);
    time_t now_epoch_secs = mktime(&now_local);

    spot_check_config_t *config = nvs_get_config();
    for (int i = 0; i < NUM_DIFFERENTIAL_UPDATES; i++) {
        // Only have one update struct that shouldn't run in online mode so just hardcode it
        if (i == DIFFERENTIAL_UPDATE_INDEX_NETWORK_CHECK) {
            differential_updates[i].active = false;
            log_printf(LOG_LEVEL_DEBUG, "Deactivated diff update struct '%s'", differential_updates[i].debug_name);
        } else {
            // Only activate the struct if it matches with the currently active operating mode
            differential_updates[i].active =
                active_operating_mode_matches(config->operating_mode, differential_updates[i].active_operating_mode);
            differential_updates[i].force_next_update = differential_updates[i].force_on_transition_to_online;

            // Only reset the base diff time if we're coming from offline or init - from OTA we want to seamlessly
            // transition back to same state. Edge case bug here if device keeps losing and regaining connection before
            // the full period of each diff update has happened at least once.
            if (respect_force_flags) {
                differential_updates[i].last_executed_epoch_secs = now_epoch_secs;
            }

            log_printf(LOG_LEVEL_DEBUG,
                       "%s diff update struct '%s'",
                       differential_updates[i].active ? "Activated" : "Did not active",
                       differential_updates[i].debug_name);
        }
    }

    bool activate_struct        = false;
    bool operating_mode_matches = false;
    for (int i = 0; i < NUM_DISCRETE_UPDATES; i++) {
        // No matter the struct type (chart or otherwise), never activate if operating mode doesn't match
        operating_mode_matches =
            active_operating_mode_matches(config->operating_mode, discrete_updates[i].active_operating_mode);

        // If op mode matches and this struct is for a chart, also check against the active chart values in the config
        if (operating_mode_matches && update_struct_is_chart(i)) {
            activate_struct = active_chart_matches(config, i);
        } else {
            activate_struct = operating_mode_matches;
        }

        discrete_updates[i].active = activate_struct;

        // Don't force specific discrete updates on activation. Otherwise we'd trigger things like all three swell
        // updates back to back (and an unnecessary time update but that's a bit less intrusive). Also only worry about
        // this if the struct was activated in the previous lines, otherwise pointless (1sec callback bails immediately
        // if not active) and log messages looks funny
        discrete_updates[i].force_next_update =
            respect_force_flags && activate_struct && discrete_updates[i].force_on_transition_to_online;
        log_printf(LOG_LEVEL_DEBUG,
                   "%s %s discrete update struct '%s'",
                   discrete_updates[i].active ? "Activated" : "Did not activate",
                   discrete_updates[i].force_next_update ? "and forced" : "but did not force",
                   discrete_updates[i].debug_name);
    }

    scheduler_mode = SCHEDULER_MODE_ONLINE;
}

UBaseType_t scheduler_task_get_stack_high_water() {
    MEMFAULT_ASSERT(scheduler_task_handle);
    return uxTaskGetStackHighWaterMark(scheduler_task_handle);
}

void scheduler_task_init() {
    scheduler_mode = SCHEDULER_MODE_INIT;
    scheduled_bits = 0x0;
}

void scheduler_task_start() {
    // Print out all update structs and set them all to inactive. main.c init function responsible for setting scheduler
    // into offline or online mode no matter what, otherwise scheduler will never run.
    log_printf(LOG_LEVEL_DEBUG, "List of all time differential updates:");
    differential_update_t *diff_check = NULL;
    for (int i = 0; i < NUM_DIFFERENTIAL_UPDATES; i++) {
        diff_check         = &differential_updates[i];
        diff_check->active = false;
        log_printf(LOG_LEVEL_DEBUG,
                   "'%s' executing every %.0f seconds",
                   diff_check->debug_name,
                   diff_check->update_interval_secs);
    }

    log_printf(LOG_LEVEL_DEBUG, "List of all discrete updates:");
    discrete_update_t *discrete_check = NULL;
    for (int i = 0; i < NUM_DISCRETE_UPDATES; i++) {
        discrete_check         = &discrete_updates[i];
        discrete_check->active = false;
        log_printf(LOG_LEVEL_DEBUG,
                   "'%s' executing at %u:%02u",
                   discrete_check->debug_name,
                   discrete_check->hour,
                   discrete_check->minute);
    }

    // If we're running in custom mode, set the update_interval_secs field of the custom screen update diff
    // struct since it's dynamic from the config and not a preprocessor macro like all others. Has to be done in
    // _start not _init becuase NVS doesn't load config into memory until its own _start function
    spot_check_config_t *config = nvs_get_config();
    if (config->operating_mode == SPOT_CHECK_MODE_CUSTOM) {
        differential_update_t *custom_diff_update =
            &differential_updates[DIFFERENTIAL_UPDATE_INDEX_CUSTOM_SCREEN_UPDATE];

        custom_diff_update->update_interval_secs = config->custom_update_interval_secs;
        log_printf(LOG_LEVEL_DEBUG,
                   "Updated custom screen update diff stuct update_interval_secs to %.0f (%lu)",
                   custom_diff_update->update_interval_secs,
                   config->custom_update_interval_secs);
    }

    xTaskCreate(&scheduler_task,
                "scheduler-update",
                SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES * 4,
                NULL,
                tskIDLE_PRIORITY,
                &scheduler_task_handle);
}
