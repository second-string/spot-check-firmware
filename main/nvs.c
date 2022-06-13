#include <string.h>
#include "constants.h"

#include "freertos/FreeRTOS.h"
#include "log.h"
#include "nvs_flash.h"

#include "http_server.h"
#include "nvs.h"

#define TAG "sc-nvs"

bool new_location_set;

static nvs_handle_t handle = 0;

// Allocate backing field buffers for our settings
static char _number_of_days[MAX_LENGTH_NUMBER_OF_DAYS_PARAM + 1]                        = {0};
static char _spot_name[MAX_LENGTH_SPOT_NAME_PARAM + 1]                                  = {0};
static char _spot_uid[MAX_LENGTH_SPOT_UID_PARAM + 1]                                    = {0};
static char _spot_lat[MAX_LENGTH_SPOT_LAT_PARAM + 1]                                    = {0};
static char _spot_lon[MAX_LENGTH_SPOT_LON_PARAM + 1]                                    = {0};
static char _forecast_types[MAX_NUM_FORECAST_TYPES][MAX_LENGTH_FORECAST_TYPE_PARAM + 1] = {0};

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

    new_location_set = false;

    log_printf(TAG, LOG_LEVEL_INFO, "NVS successfully inited and opened");
}

bool nvs_get_uint32(char *key, uint32_t *val) {
    bool retval = false;
    configASSERT(handle);

    esp_err_t err = nvs_get_u32(handle, key, val);
    switch (err) {
        case ESP_OK:
            retval = true;
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            log_printf(TAG, LOG_LEVEL_INFO, "The NVS value for key '%s' is not initialized yet", key);
            *val = 0;
            break;
        default:
            log_printf(TAG,
                       LOG_LEVEL_ERROR,
                       "Error (%s) reading value for key '%s' from NVS\n",
                       esp_err_to_name(err),
                       key);
    }

    return retval;
}

bool nvs_set_uint32(char *key, uint32_t val) {
    bool retval = false;
    configASSERT(handle);

    esp_err_t err = nvs_set_u32(handle, key, val);
    if (err == ESP_OK) {
        retval = true;
    } else {
        log_printf(TAG,
                   LOG_LEVEL_ERROR,
                   "Error (%s) setting uint32 value '%u' for key '%s' in NVS",
                   esp_err_to_name(err),
                   val,
                   key);
    }

    return retval;
}

void nvs_save_config(spot_check_config *config) {
    if (handle == 0) {
        log_printf(TAG, LOG_LEVEL_ERROR, "Attempting to save to NVS before calling nvs_init(), not saving values");
        return;
    }

    // Kick weather update if we have a new spot. 1 second timer monitors this bool while task loop runs once
    // every 5 seconds. Max time between save and new weather is 6 seconds
    if (current_config.spot_lat != config->spot_lat || current_config.spot_lon != config->spot_lon) {
        new_location_set = true;
    }

    ESP_ERROR_CHECK(nvs_set_str(handle, "number_of_days", config->number_of_days));
    ESP_ERROR_CHECK(nvs_set_str(handle, "spot_name", config->spot_name));
    ESP_ERROR_CHECK(nvs_set_str(handle, "spot_uid", config->spot_uid));
    ESP_ERROR_CHECK(nvs_set_str(handle, "spot_lat", config->spot_lat));
    ESP_ERROR_CHECK(nvs_set_str(handle, "spot_lon", config->spot_lon));

    int       i;
    char      forecast_type_key[40];
    esp_err_t err;
    for (i = 0; i < MAX_NUM_FORECAST_TYPES; i++) {
        sprintf(forecast_type_key, "forecast_type_%d", i);

        // If we're onto empty types or nullptrs, delete any of the further forecast_type keys that we might have saved
        // previously
        if (config->forecast_types[i] == 0 || *config->forecast_types[i] == 0) {
            err = nvs_erase_key(handle, forecast_type_key);
        } else {
            err = nvs_set_str(handle, forecast_type_key, config->forecast_types[i]);
        }

        switch (err) {
            case ESP_OK:
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                break;
            default:
                printf("Error (%s) saving forecast_type to flash with key %s!\n",
                       esp_err_to_name(err),
                       forecast_type_key);
        }
    }

    ESP_ERROR_CHECK(nvs_commit(handle));
}

spot_check_config *nvs_get_config() {
    if (handle == 0) {
        log_printf(TAG,
                   LOG_LEVEL_ERROR,
                   "Attempting to retrieve from NVS before calling nvs_init(), returning null ptr");
        return NULL;
    }

    size_t max_length_number_of_days_param = MAX_LENGTH_NUMBER_OF_DAYS_PARAM;
    size_t max_length_spot_name_param      = MAX_LENGTH_SPOT_NAME_PARAM;
    size_t max_length_spot_uid_param       = MAX_LENGTH_SPOT_UID_PARAM;
    size_t max_length_spot_lat_param       = MAX_LENGTH_SPOT_LAT_PARAM;
    size_t max_length_spot_lon_param       = MAX_LENGTH_SPOT_LON_PARAM;
    size_t max_length_forecast_type_param  = MAX_LENGTH_FORECAST_TYPE_PARAM;

    esp_err_t err = nvs_get_str(handle, "number_of_days", _number_of_days, &max_length_number_of_days_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            printf("The value is not initialized yet, defaulting to 1\n");
            strcpy(_number_of_days, "1");
            break;
        default:
            printf("Error (%s) reading number_of_days from flash!\n", esp_err_to_name(err));
    }

    err = nvs_get_str(handle, "spot_name", _spot_name, &max_length_spot_name_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            printf("The value is not initialized yet, defaulting to wedge!\n");
            strcpy(_spot_name, "Wedge");
            break;
        default:
            printf("Error (%s) reading spot_name from flash!\n", esp_err_to_name(err));
    }

    err = nvs_get_str(handle, "spot_lat", _spot_lat, &max_length_spot_lat_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            printf("The value is not initialized yet, defaulting to 33.5930302087!\n");
            strcpy(_spot_lat, "33.5930302087");
            break;
        default:
            printf("Error (%s) reading spot_lat from flash!\n", esp_err_to_name(err));
    }

    err = nvs_get_str(handle, "spot_lon", _spot_lon, &max_length_spot_lon_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            printf("The value is not initialized yet, defaulting to -117.8819918632!\n");
            strcpy(_spot_lon, "-117.8819918632");
            break;
        default:
            printf("Error (%s) reading spot_lon from flash!\n", esp_err_to_name(err));
    }

    err = nvs_get_str(handle, "spot_uid", _spot_uid, &max_length_spot_uid_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:;
            char *wedge_uid = "5842041f4e65fad6a770882b";
            printf("The value is not initialized yet, defaulting to wedge's uid (%s)\n", wedge_uid);
            strcpy(_spot_uid, wedge_uid);
            break;
        default:
            printf("Error (%s) reading spot_uid from flash!\n", esp_err_to_name(err));
    }

    int          i;
    char         forecast_type_key[40];
    unsigned int num_forecast_types = 0;
    for (i = 0; i < MAX_NUM_FORECAST_TYPES; i++) {
        sprintf(forecast_type_key, "forecast_type_%d", i);
        err = nvs_get_str(handle, forecast_type_key, _forecast_types[i], &max_length_forecast_type_param);
        switch (err) {
            case ESP_OK:
                // Only increment count for non-null types (if we had previously saved a key and then deleted the value)
                if (*_forecast_types[i] != 0) {
                    num_forecast_types++;
                }
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                break;
            default:
                printf("Error (%s) reading forecast_type from flash with key %s!\n",
                       esp_err_to_name(err),
                       forecast_type_key);
        }
    }

    if (num_forecast_types == 0) {
        log_printf(TAG, LOG_LEVEL_INFO, "Read zero forecast types out of flash, defaulting to ['swell']");
        strcpy(_forecast_types[0], "swell");
        num_forecast_types = 1;
    }

    current_config.number_of_days = _number_of_days;
    current_config.spot_name      = _spot_name;
    current_config.spot_uid       = _spot_uid;
    current_config.spot_lat       = _spot_lat;
    current_config.spot_lon       = _spot_lon;
    for (i = 0; i < num_forecast_types; i++) {
        current_config.forecast_types[i] = _forecast_types[i];
    }
    current_config.forecast_type_count = num_forecast_types;

    return &current_config;
}

esp_err_t nvs_full_erase() {
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        log_printf(TAG, LOG_LEVEL_ERROR, "Failed to erase NVS flash! %s", esp_err_to_name(err));
    }

    return err;
}

char *get_next_forecast_type(char **types) {
    static unsigned int index = 0;
    if (index > MAX_NUM_FORECAST_TYPES - 1) {
        index = 0;
    }

    char *next_type = types[index];

    // Wrap if we get a null since we probably haven't filled all 5 param slots
    // We check the pointer itself in case we saved new config with less forecast_types
    // than before, so reset to 0 index.
    if (next_type == 0 || *next_type == 0) {
        index     = 0;
        next_type = types[index];
    }

    index++;
    return next_type;
}

void nvs_zero_forecast_types(int starting_from, char **forecast_types) {
    for (; starting_from < MAX_NUM_FORECAST_TYPES; starting_from++) {
        forecast_types[starting_from] = NULL;
    }
}
