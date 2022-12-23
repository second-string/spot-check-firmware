#include "constants.h"

#include "cJSON.h"
#include "freertos/FreeRTOS.h"

#include "constants.h"
#include "json.h"

// Must included below constants.h where we overwite the define of LOG_LOCAL_LEVEL
#include "log.h"

#define TAG SC_TAG_JSON

cJSON *parse_json(char *server_response) {
    cJSON *json = cJSON_Parse(server_response);
    if (json == NULL) {
        const char *err_ptr = cJSON_GetErrorPtr();
        if (err_ptr) {
            log_printf(LOG_LEVEL_INFO, "JSON parsing err: %s\n", err_ptr);
        }
    }

    return json;
}
