#include "constants.h"

#include "freertos/FreeRTOS.h"
#include "cJSON.h"

#include "json.h"

// Must included below constants.h where we overwite the define of LOG_LOCAL_LEVEL
#include "esp_log.h"

cJSON* parse_json(char *server_response) {
    cJSON *json = cJSON_Parse(server_response);
    if (json == NULL) {
        const char *err_ptr = cJSON_GetErrorPtr();
        if (err_ptr) {
            ESP_LOGI(TAG, "JSON parsing err: %s\n", err_ptr);
        }
    }

    return json;
}
