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

#define TAG SC_TAG_NVS

static nvs_handle_t handle = 0;

// Allocate backing field buffers for our settings
static char _spot_name[MAX_LENGTH_SPOT_NAME_PARAM + 1]             = {0};
static char _spot_uid[MAX_LENGTH_SPOT_UID_PARAM + 1]               = {0};
static char _spot_lat[MAX_LENGTH_SPOT_LAT_PARAM + 1]               = {0};
static char _spot_lon[MAX_LENGTH_SPOT_LON_PARAM + 1]               = {0};
static char _tz_str[MAX_LENGTH_TZ_STR_PARAM + 1]                   = {0};
static char _tz_display_name[MAX_LENGTH_TZ_DISPLAY_NAME_PARAM + 1] = {0};

static spot_check_config current_config;

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

bool nvs_get_uint32(char *key, uint32_t *val) {
    bool retval = false;
    MEMFAULT_ASSERT(handle);

    esp_err_t err = nvs_get_u32(handle, key, val);
    switch (err) {
        case ESP_OK:
            retval = true;
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            log_printf(LOG_LEVEL_INFO, "The NVS value for key '%s' is not initialized yet", key);
            *val = 0;
            break;
        default:
            log_printf(LOG_LEVEL_ERROR, "Error (%s) reading value for key '%s' from NVS", esp_err_to_name(err), key);
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

bool nvs_get_int8(char *key, int8_t *val) {
    bool retval = false;
    MEMFAULT_ASSERT(handle);

    esp_err_t err = nvs_get_i8(handle, key, val);
    switch (err) {
        case ESP_OK:
            retval = true;
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            log_printf(LOG_LEVEL_INFO, "The NVS value for key '%s' is not initialized yet", key);
            *val = 0;
            break;
        default:
            log_printf(LOG_LEVEL_ERROR, "Error (%s) reading value for key '%s' from NVS", esp_err_to_name(err), key);
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

void nvs_save_config(spot_check_config *config) {
    if (handle == 0) {
        log_printf(LOG_LEVEL_ERROR, "Attempting to save to NVS before calling nvs_init(), not saving values");
        return;
    }

    // Kick conditions & both charts update if we have a new spot. The logic in scheduler interprets these three update
    // bits as a full clear, so also include the time trigger so there isn't a minute-long gap of no time
    if (current_config.spot_lat != config->spot_lat || current_config.spot_lon != config->spot_lon) {
        scheduler_trigger_time_update();
        scheduler_trigger_spot_name_update();
        scheduler_trigger_conditions_update();
        scheduler_trigger_both_charts_update();
    }

    ESP_ERROR_CHECK(nvs_set_str(handle, "spot_name", config->spot_name));
    ESP_ERROR_CHECK(nvs_set_str(handle, "spot_uid", config->spot_uid));
    ESP_ERROR_CHECK(nvs_set_str(handle, "spot_lat", config->spot_lat));
    ESP_ERROR_CHECK(nvs_set_str(handle, "spot_lon", config->spot_lon));
    ESP_ERROR_CHECK(nvs_set_str(handle, "tz_str", config->tz_str));
    ESP_ERROR_CHECK(nvs_set_str(handle, "tz_display_name", config->tz_display_name));
    ESP_ERROR_CHECK(nvs_set_str(handle, "tz_display_name", config->tz_display_name));

    ESP_ERROR_CHECK(nvs_commit(handle));
}

spot_check_config *nvs_get_config() {
    if (handle == 0) {
        log_printf(LOG_LEVEL_ERROR, "Attempting to retrieve from NVS before calling nvs_init(), returning null ptr");
        return NULL;
    }

    size_t max_length_spot_name_param       = MAX_LENGTH_SPOT_NAME_PARAM;
    size_t max_length_spot_uid_param        = MAX_LENGTH_SPOT_UID_PARAM;
    size_t max_length_spot_lat_param        = MAX_LENGTH_SPOT_LAT_PARAM;
    size_t max_length_spot_lon_param        = MAX_LENGTH_SPOT_LON_PARAM;
    size_t max_length_tz_str_param          = MAX_LENGTH_TZ_STR_PARAM;
    size_t max_length_tz_display_name_param = MAX_LENGTH_TZ_DISPLAY_NAME_PARAM;

    esp_err_t err = nvs_get_str(handle, "spot_name", _spot_name, &max_length_spot_name_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            log_printf(LOG_LEVEL_DEBUG, "The value is not initialized yet, defaulting to wedge!");
            strcpy(_spot_name, "Wedge");
            break;
        default:
            log_printf(LOG_LEVEL_DEBUG, "Error (%s) reading spot_name from flash!", esp_err_to_name(err));
    }

    err = nvs_get_str(handle, "spot_lat", _spot_lat, &max_length_spot_lat_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            log_printf(LOG_LEVEL_DEBUG, "The value is not initialized yet, defaulting to 33.5930302087!");
            strcpy(_spot_lat, "33.5930302087");
            break;
        default:
            log_printf(LOG_LEVEL_DEBUG, "Error (%s) reading spot_lat from flash!", esp_err_to_name(err));
    }

    err = nvs_get_str(handle, "spot_lon", _spot_lon, &max_length_spot_lon_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            log_printf(LOG_LEVEL_DEBUG, "The value is not initialized yet, defaulting to -117.8819918632!");
            strcpy(_spot_lon, "-117.8819918632");
            break;
        default:
            log_printf(LOG_LEVEL_DEBUG, "Error (%s) reading spot_lon from flash!", esp_err_to_name(err));
    }

    err = nvs_get_str(handle, "spot_uid", _spot_uid, &max_length_spot_uid_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:;
            char *wedge_uid = "5842041f4e65fad6a770882b";
            log_printf(LOG_LEVEL_DEBUG, "The value is not initialized yet, defaulting to wedge's uid (%s)", wedge_uid);
            strcpy(_spot_uid, wedge_uid);
            break;
        default:
            log_printf(LOG_LEVEL_DEBUG, "Error (%s) reading spot_uid from flash!", esp_err_to_name(err));
    }

    err = nvs_get_str(handle, "tz_str", _tz_str, &max_length_tz_str_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:;
            char *tz_str = "CET-1CEST,M3.4.0/2,M10.4.0/2";
            log_printf(LOG_LEVEL_DEBUG,
                       "The value is not initialized yet, defaulting to default Berlin tz  '%s'",
                       tz_str);
            strcpy(_tz_str, tz_str);
            break;
        default:
            log_printf(LOG_LEVEL_DEBUG, "Error (%s) reading tz_str from flash!", esp_err_to_name(err));
    }

    err = nvs_get_str(handle, "tz_display_name", _tz_display_name, &max_length_tz_display_name_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:;
            char *display_name = "Europe/Berlin";
            log_printf(LOG_LEVEL_DEBUG,
                       "The value is not initialized yet, defaulting to default Berlin tz  '%s'",
                       display_name);
            strcpy(_tz_display_name, display_name);
            break;
        default:
            log_printf(LOG_LEVEL_DEBUG, "Error (%s) reading tz_display_name from flash!", esp_err_to_name(err));
    }

    current_config.spot_name       = _spot_name;
    current_config.spot_uid        = _spot_uid;
    current_config.spot_lat        = _spot_lat;
    current_config.spot_lon        = _spot_lon;
    current_config.tz_str          = _tz_str;
    current_config.tz_display_name = _tz_display_name;

    return &current_config;
}

esp_err_t nvs_full_erase() {
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        log_printf(LOG_LEVEL_ERROR, "Failed to erase NVS flash! %s", esp_err_to_name(err));
    }

    return err;
}
