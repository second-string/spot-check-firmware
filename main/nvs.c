#include <string.h>
#include "constants.h"

#include "freertos/FreeRTOS.h"
#include "log.h"
#include "memfault/panics/assert.h"
#include "nvs_flash.h"

#include "constants.h"
#include "http_server.h"
#include "nvs.h"
#include "scheduler_task.h"
#include "screen_img_handler.h"

#define TAG SC_TAG_NVS

static const char *const chart_strings_by_enum[] = {
    [SCREEN_IMG_TIDE_CHART]  = "tide",
    [SCREEN_IMG_SWELL_CHART] = "swell",
    [SCREEN_IMG_WIND_CHART]  = "wind",
};

static nvs_handle_t handle = 0;

// Allocate backing field buffers for our settings
static char _spot_name[MAX_LENGTH_SPOT_NAME_PARAM + 1]                 = {0};
static char _spot_uid[MAX_LENGTH_SPOT_UID_PARAM + 1]                   = {0};
static char _spot_lat[MAX_LENGTH_SPOT_LAT_PARAM + 1]                   = {0};
static char _spot_lon[MAX_LENGTH_SPOT_LON_PARAM + 1]                   = {0};
static char _tz_str[MAX_LENGTH_TZ_STR_PARAM + 1]                       = {0};
static char _tz_display_name[MAX_LENGTH_TZ_DISPLAY_NAME_PARAM + 1]     = {0};
static char _operating_mode[MAX_LENGTH_OPERATING_MODE_PARAM + 1]       = {0};
static char _custom_screen_url[MAX_LENGTH_CUSTOM_SCREEN_URL_PARAM + 1] = {0};

static spot_check_config_t current_config;

/*
 * Loads the config vales in NVS into the in-mem representation for easy access
 */
static spot_check_config_t *nvs_load_config() {
    if (handle == 0) {
        log_printf(LOG_LEVEL_ERROR, "Attempting to retrieve from NVS before calling nvs_init(), returning null ptr");
        return NULL;
    }

    size_t max_bytes_to_write = MAX_LENGTH_SPOT_NAME_PARAM;
    nvs_get_string("spot_name", _spot_name, &max_bytes_to_write, "Wedge");

    max_bytes_to_write = MAX_LENGTH_SPOT_LAT_PARAM;
    nvs_get_string("spot_lat", _spot_lat, &max_bytes_to_write, "33.5930302087");

    max_bytes_to_write = MAX_LENGTH_SPOT_LON_PARAM;
    nvs_get_string("spot_lon", _spot_lon, &max_bytes_to_write, "-117.8819918632");

    max_bytes_to_write = MAX_LENGTH_SPOT_UID_PARAM;
    nvs_get_string("spot_uid", _spot_uid, &max_bytes_to_write, "5842041f4e65fad6a770882b");

    max_bytes_to_write = MAX_LENGTH_TZ_STR_PARAM;
    nvs_get_string("tz_str", _tz_str, &max_bytes_to_write, "CET-1CEST,M3.5.0/2,M10.5.0/2");

    max_bytes_to_write = MAX_LENGTH_TZ_DISPLAY_NAME_PARAM;
    nvs_get_string("tz_display_name", _tz_display_name, &max_bytes_to_write, "Europe/Berlin");

    max_bytes_to_write = MAX_LENGTH_OPERATING_MODE_PARAM;
    nvs_get_string("operating_mode", _operating_mode, &max_bytes_to_write, "weather");

    max_bytes_to_write = MAX_LENGTH_CUSTOM_SCREEN_URL_PARAM;
    nvs_get_string("custom_scrn_url",
                   _custom_screen_url,
                   &max_bytes_to_write,
                   "https://spotcheck.brianteam.com/custom_screen_test_image");

    uint32_t temp_custom_update_interval_secs = 0;
    nvs_get_uint32("custom_ui_secs", &temp_custom_update_interval_secs, 900);

    max_bytes_to_write = MAX_LENGTH_ACTIVE_CHART_PARAM;
    char temp_chart_str[10];
    nvs_get_string("chart_1", temp_chart_str, &max_bytes_to_write, "tide");

    screen_img_t active_chart_1;
    if (!nvs_chart_string_to_enum(temp_chart_str, &active_chart_1)) {
        log_printf(LOG_LEVEL_ERROR,
                   "Error parsing chart str '%s' to enum, falling back to tide chart enum",
                   temp_chart_str);
        active_chart_1 = SCREEN_IMG_TIDE_CHART;
    }

    max_bytes_to_write = MAX_LENGTH_ACTIVE_CHART_PARAM;
    nvs_get_string("chart_2", temp_chart_str, &max_bytes_to_write, "swell");

    screen_img_t active_chart_2;
    if (!nvs_chart_string_to_enum(temp_chart_str, &active_chart_2)) {
        log_printf(LOG_LEVEL_ERROR,
                   "Error parsing chart str '%s' to enum, falling back to swell chart enum",
                   temp_chart_str);
        active_chart_2 = SCREEN_IMG_SWELL_CHART;
    }

    current_config.spot_name                   = _spot_name;
    current_config.spot_uid                    = _spot_uid;
    current_config.spot_lat                    = _spot_lat;
    current_config.spot_lon                    = _spot_lon;
    current_config.tz_str                      = _tz_str;
    current_config.tz_display_name             = _tz_display_name;
    current_config.operating_mode              = spot_check_string_to_mode(_operating_mode);
    current_config.custom_screen_url           = _custom_screen_url;
    current_config.custom_update_interval_secs = temp_custom_update_interval_secs;
    current_config.active_chart_1              = active_chart_1;
    current_config.active_chart_2              = active_chart_2;

    nvs_print_config(LOG_LEVEL_DEBUG);

    return &current_config;
}

void nvs_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_full_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &h));
    handle = h;

    log_printf(LOG_LEVEL_INFO, "NVS successfully inited and opened");
}

void nvs_start() {
    nvs_load_config();
}

bool nvs_get_uint32(char *key, uint32_t *val, uint32_t fallback) {
    bool retval = false;
    MEMFAULT_ASSERT(handle);

    esp_err_t err = nvs_get_u32(handle, key, val);
    switch (err) {
        case ESP_OK:
            retval = true;
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            log_printf(LOG_LEVEL_INFO,
                       "The NVS value for key '%s' is not initialized yet, returning fallback value %lu",
                       key,
                       fallback);
            *val = fallback;
            break;
        default:
            log_printf(LOG_LEVEL_ERROR,
                       "Error (%s) reading value for key '%s' from NVS, returning fallback value %lu",
                       esp_err_to_name(err),
                       key,
                       fallback);
            *val = fallback;
    }

    return retval;
}

bool nvs_set_uint8(char *key, int8_t val) {
    bool retval = false;
    MEMFAULT_ASSERT(handle);

    esp_err_t err = nvs_set_i8(handle, key, val);
    if (err == ESP_OK) {
        retval = true;
    } else {
        log_printf(LOG_LEVEL_ERROR,
                   "Error (%s) setting int8 value '%u' for key '%s' in NVS",
                   esp_err_to_name(err),
                   val,
                   key);
    }

    return retval;
}

bool nvs_get_int8(char *key, int8_t *val, int8_t fallback) {
    bool retval = false;
    MEMFAULT_ASSERT(handle);

    esp_err_t err = nvs_get_i8(handle, key, val);
    switch (err) {
        case ESP_OK:
            retval = true;
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            log_printf(LOG_LEVEL_INFO,
                       "The NVS value for key '%s' is not initialized yet, returning fallback value %d",
                       key,
                       fallback);
            *val = fallback;
            break;
        default:
            log_printf(LOG_LEVEL_ERROR,
                       "Error (%s) reading value for key '%s' from NVS, returning fallback value %d",
                       esp_err_to_name(err),
                       key,
                       fallback);
            *val = fallback;
    }

    return retval;
}

bool nvs_set_uint32(char *key, uint32_t val) {
    bool retval = false;
    MEMFAULT_ASSERT(handle);

    esp_err_t err = nvs_set_u32(handle, key, val);
    if (err == ESP_OK) {
        retval = true;
    } else {
        log_printf(LOG_LEVEL_ERROR,
                   "Error (%s) setting uint32 value '%u' for key '%s' in NVS",
                   esp_err_to_name(err),
                   val,
                   key);
    }

    return retval;
}

bool nvs_get_string(char *key, char *val, size_t *val_size, char *fallback) {
    bool retval = false;
    MEMFAULT_ASSERT(handle);

    esp_err_t err = nvs_get_str(handle, key, val, val_size);
    switch (err) {
        case ESP_OK:
            retval = true;
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            log_printf(LOG_LEVEL_INFO,
                       "The NVS value for key '%s' is not initialized yet, returing fallback value '%s'",
                       key,
                       fallback);
            strcpy(val, fallback);
            break;
        default:
            log_printf(LOG_LEVEL_ERROR,
                       "Error (%s) reading value for key '%s' from NVS, returing fallback value '%s'",
                       esp_err_to_name(err),
                       key,
                       fallback);
            strcpy(val, fallback);
    }

    return retval;
}

bool nvs_set_string(char *key, char *val) {
    bool retval = false;
    MEMFAULT_ASSERT(handle);

    // If config is only partially populated (depending on the mode) just skip over the unset keys
    if (!val) {
        log_printf(LOG_LEVEL_INFO, "Value string provided for key '%s' is null, not setting any value in NVS", key);
        return true;
    }

    esp_err_t err = nvs_set_str(handle, key, val);
    if (err == ESP_OK) {
        retval = true;
    } else {
        log_printf(LOG_LEVEL_ERROR,
                   "Error (%s) setting strng value '%s' for key '%s' in NVS",
                   esp_err_to_name(err),
                   val,
                   key);
    }

    return retval;
}

spot_check_config_t *nvs_get_config() {
    MEMFAULT_ASSERT(handle);

    return &current_config;
}

/*
 * Helper func to print out all the key/vals in current config
 */
void nvs_print_config(log_level_t level) {
    // call to log_printf needs compile-time eval of params so we have to do it this ugly way
    switch (level) {
        case LOG_LEVEL_INFO:
            log_printf(LOG_LEVEL_INFO, "CURRENT IN-MEM SPOT CHECK CONFIG");
            log_printf(LOG_LEVEL_INFO, "spot_name: %s", current_config.spot_name);
            log_printf(LOG_LEVEL_INFO, "spot_uid: %s", current_config.spot_uid);
            log_printf(LOG_LEVEL_INFO, "spot_lat: %s", current_config.spot_lat);
            log_printf(LOG_LEVEL_INFO, "spot_lon: %s", current_config.spot_lon);
            log_printf(LOG_LEVEL_INFO, "tz_str: %s", current_config.tz_str);
            log_printf(LOG_LEVEL_INFO, "tz_display_name: %s", current_config.tz_display_name);
            log_printf(LOG_LEVEL_INFO, "operating_mode: %s", spot_check_mode_to_string(current_config.operating_mode));
            log_printf(LOG_LEVEL_INFO, "custom_scrn_url: %s", current_config.custom_screen_url);
            log_printf(LOG_LEVEL_INFO, "custom_ui_secs: %lu", current_config.custom_update_interval_secs);
            log_printf(LOG_LEVEL_INFO, "active_chart_1: %u", current_config.active_chart_1);
            log_printf(LOG_LEVEL_INFO, "active_chart_2: %u", current_config.active_chart_2);
            break;
        case LOG_LEVEL_DEBUG:
            log_printf(LOG_LEVEL_DEBUG, "CURRENT IN-MEM SPOT CHECK CONFIG");
            log_printf(LOG_LEVEL_DEBUG, "spot_name: %s", current_config.spot_name);
            log_printf(LOG_LEVEL_DEBUG, "spot_uid: %s", current_config.spot_uid);
            log_printf(LOG_LEVEL_DEBUG, "spot_lat: %s", current_config.spot_lat);
            log_printf(LOG_LEVEL_DEBUG, "spot_lon: %s", current_config.spot_lon);
            log_printf(LOG_LEVEL_DEBUG, "tz_str: %s", current_config.tz_str);
            log_printf(LOG_LEVEL_DEBUG, "tz_display_name: %s", current_config.tz_display_name);
            log_printf(LOG_LEVEL_DEBUG, "operating_mode: %s", spot_check_mode_to_string(current_config.operating_mode));
            log_printf(LOG_LEVEL_DEBUG, "custom_scrn_url: %s", current_config.custom_screen_url);
            log_printf(LOG_LEVEL_DEBUG, "custom_ui_secs: %lu", current_config.custom_update_interval_secs);
            log_printf(LOG_LEVEL_DEBUG, "active_chart_1: %u", current_config.active_chart_1);
            log_printf(LOG_LEVEL_DEBUG, "active_chart_2: %u", current_config.active_chart_2);
            break;
        default:
            MEMFAULT_ASSERT(0);
    }
}

void nvs_save_config(spot_check_config_t *config) {
    if (handle == 0) {
        log_printf(LOG_LEVEL_ERROR, "Attempting to save to NVS before calling nvs_init(), not saving values");
        return;
    }

    // Kick conditions & both charts update if we have a new spot. The logic in scheduler interprets these three update
    // bits as a full clear, so also include the time trigger so there isn't a minute-long gap of no time
    if (current_config.spot_lat != config->spot_lat || current_config.spot_lon != config->spot_lon) {
        scheduler_schedule_time_update();
        scheduler_schedule_spot_name_update();
        scheduler_schedule_conditions_update();
        scheduler_schedule_both_charts_update();
        scheduler_trigger();
    }

    MEMFAULT_ASSERT(nvs_set_string("spot_name", config->spot_name));
    MEMFAULT_ASSERT(nvs_set_string("spot_uid", config->spot_uid));
    MEMFAULT_ASSERT(nvs_set_string("spot_lat", config->spot_lat));
    MEMFAULT_ASSERT(nvs_set_string("spot_lon", config->spot_lon));
    MEMFAULT_ASSERT(nvs_set_string("tz_str", config->tz_str));
    MEMFAULT_ASSERT(nvs_set_string("tz_display_name", config->tz_display_name));
    MEMFAULT_ASSERT(nvs_set_string("operating_mode", (char *)spot_check_mode_to_string(config->operating_mode)));
    MEMFAULT_ASSERT(nvs_set_string("custom_scrn_url", config->custom_screen_url));
    MEMFAULT_ASSERT(nvs_set_uint32("custom_ui_secs", config->custom_update_interval_secs));
    MEMFAULT_ASSERT(nvs_set_string("chart_1", (char *)chart_strings_by_enum[config->active_chart_1]));
    MEMFAULT_ASSERT(nvs_set_string("chart_2", (char *)chart_strings_by_enum[config->active_chart_2]));

    ESP_ERROR_CHECK(nvs_commit(handle));

    // Need to reload back into mem. Could theoretically manually set these to avoid the second flash time hit, but
    // safer to use the existing logic
    nvs_load_config();
}

esp_err_t nvs_full_erase() {
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        log_printf(LOG_LEVEL_ERROR, "Failed to erase NVS flash! %s", esp_err_to_name(err));
    }

    return err;
}

/*
 * Convert string val received by fw from config POST to the enum value used in the actual in-mem config
 */
bool nvs_chart_string_to_enum(char *chart_str_in, screen_img_t *enum_out) {
    bool success = false;
    for (uint8_t i = 0; i < sizeof(chart_strings_by_enum) / sizeof(char *); i++) {
        if (strcmp(chart_strings_by_enum[i], chart_str_in) == 0) {
            *enum_out = i;
            success   = true;
            break;
        }
    }

    return success;
}

/*
 * Expose access to the enum/string array to http_server
 */
void nvs_chart_enum_to_string(screen_img_t enum_in, char *chart_str_out) {
    MEMFAULT_ASSERT(enum_in != SCREEN_IMG_COUNT);

    strcpy(chart_str_out, chart_strings_by_enum[enum_in]);
}
