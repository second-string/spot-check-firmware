#include "constants.h"
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"

#include "nvs.h"
#include "http_server.h"

static nvs_handle_t handle = 0;

// Allocate backing field buffers for our settings
static char _number_of_days[MAX_LENGTH_NUMBER_OF_DAYS_PARAM + 1] = { 0 };
static char _spot_name[MAX_LENGTH_SPOT_NAME_PARAM + 1] = { 0 };
static char _spot_uid[MAX_LENGTH_SPOT_UID_PARAM + 1] = { 0 };
static char _forecast_types[MAX_NUM_FORECAST_TYPES][MAX_LENGTH_FORECAST_TYPE_PARAM + 1] = { 0 };

static spot_check_config current_config;

void nvs_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &h));
    handle = h;

    ESP_LOGI(TAG, "NVS successfully inited and opened");
}

void nvs_save_config(spot_check_config *config) {
    if (handle == 0) {
        ESP_LOGE(TAG, "Attempting to save to NVS before calling nvs_init(), not saving values");
        return;
    }

    ESP_ERROR_CHECK(nvs_set_str(handle, "number_of_days", config->number_of_days));
    ESP_ERROR_CHECK(nvs_set_str(handle, "spot_name", config->spot_name));
    ESP_ERROR_CHECK(nvs_set_str(handle, "spot_uid", config->spot_uid));

    // TODO :: BT this v and then test full flow of defaults, POST saves, re-pulls correctly, the next POST overwriting everything
    // Loops for MAX macro, but once we're past forecast_type_count we need to delete remaining keys
    int i;
    char forecast_type_key[40];
    esp_err_t err;
    for (i = 0; i < MAX_NUM_FORECAST_TYPES; i++) {
        sprintf(forecast_type_key, "forecast_type_%d", i);

        // If we're onto empty types, delete any of the further forecast_type keys that we might have saved previously
        if (*config->forecast_types[i] == 0) {
            err = nvs_erase_key(handle, forecast_type_key);
        } else {
            err = nvs_set_str(handle, forecast_type_key, config->forecast_types[i]);
        }

        switch (err) {
            case ESP_OK:
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                break;
            default :
                printf("Error (%s) saving forecast_type to flash with key %s!\n", esp_err_to_name(err), forecast_type_key);
        }
    }

    ESP_ERROR_CHECK(nvs_commit(handle));
}

spot_check_config *nvs_get_config() {
    if (handle == 0) {
        ESP_LOGE(TAG, "Attempting to retrive from NVS before calling nvs_init(), returning null ptr");
        return NULL;
    }

    size_t max_length_number_of_days_param = MAX_LENGTH_NUMBER_OF_DAYS_PARAM;
    size_t max_length_spot_name_param = MAX_LENGTH_SPOT_NAME_PARAM;
    size_t max_length_spot_uid_param = MAX_LENGTH_SPOT_UID_PARAM;
    size_t max_length_forecast_type_param = MAX_LENGTH_FORECAST_TYPE_PARAM;

    esp_err_t err = nvs_get_str(handle, "number_of_days", _number_of_days, &max_length_number_of_days_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            printf("The value is not initialized yet, defaulting to 1\n");
            strcpy(_number_of_days, "1");
            break;
        default :
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
        default :
            printf("Error (%s) reading spot_name from flash!\n", esp_err_to_name(err));
    }

    err = nvs_get_str(handle, "spot_uid", _spot_uid, &max_length_spot_uid_param);
    switch (err) {
        case ESP_OK:
            break;
        case ESP_ERR_NVS_NOT_FOUND: ;
            char *wedge_uid = "5842041f4e65fad6a770882b";
            printf("The value is not initialized yet, defaulting to wedge's uid (%s)\n", wedge_uid);
            strcpy(_spot_uid, wedge_uid);
            break;
        default :
            printf("Error (%s) reading spot_uid from flash!\n", esp_err_to_name(err));
    }

    int i;
    char forecast_type_key[40];
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
            default :
                printf("Error (%s) reading forecast_type from flash with key %s!\n", esp_err_to_name(err), forecast_type_key);
        }
    }

    if (num_forecast_types == 0) {
        ESP_LOGI(TAG, "Read zero forecast types out of flash, defaulting to ['swell']");
        strcpy(_forecast_types[0], "swell");
        num_forecast_types = 1;
    }

    current_config.number_of_days = _number_of_days;
    current_config.spot_name = _spot_name;
    current_config.spot_uid = _spot_uid;
    for (i = 0; i < num_forecast_types; i++) {
        current_config.forecast_types[i] = _forecast_types[i];
    }
    current_config.forecast_type_count = num_forecast_types;

    return &current_config;
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
        index = 0;
        next_type = types[index];
    }

    index++;
    return next_type;
}

