#include "constants.h"

#include <sys/param.h>
#include <esp_system.h>
#include <esp_http_server.h>
#include <esp_log.h>

#include "http_server.h"
#include "json.h"
#include "nvs.h"


static httpd_handle_t server_handle;

// Allocate backing field buffers for our settings
static char _number_of_days[MAX_LENGTH_NUMBER_OF_DAYS_PARAM + 1] = { 0 };
static char _spot_name[MAX_LENGTH_SPOT_NAME_PARAM + 1] = { 0 };
static char _spot_uid[MAX_LENGTH_SPOT_UID_PARAM + 1] = { 0 };
static char _forecast_types[MAX_NUM_FORECAST_TYPES][MAX_LENGTH_FORECAST_TYPE_PARAM + 1] = { 0 };

static esp_err_t health_get_handler(httpd_req_t *req);
static esp_err_t configure_post_handler(httpd_req_t *req);
static esp_err_t current_config_get_handler(httpd_req_t *req);

static const httpd_uri_t health_uri = {
    .uri       = "/health",
    .method    = HTTP_GET,
    .handler   = health_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t configure_uri = {
    .uri       = "/configure",
    .method    = HTTP_POST,
    .handler   = configure_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t current_config_uri = {
    .uri       = "/current_configuration",
    .method    = HTTP_GET,
    .handler   = current_config_get_handler,
    .user_ctx  = NULL
};

static esp_err_t health_get_handler(httpd_req_t *req) {
    httpd_resp_send(req, "Surviving not thriving", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t configure_post_handler(httpd_req_t *req) {
    const int rx_buf_size = 200;
    char buf[rx_buf_size];

    if (req->content_len > rx_buf_size) {
        ESP_LOGE(TAG, "Payload is too big (%d bytes), bailing out", req->content_len);
        httpd_resp_send(req, "err", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int bytes_received = httpd_req_recv(req, buf, MIN(req->content_len, rx_buf_size));
    if (bytes_received <= 0) {
        if (bytes_received == HTTPD_SOCK_ERR_TIMEOUT) {
            /* Retry receiving if timeout occurred */
            // continue;
            ESP_LOGE(TAG, "Received timeout, bailing out (could retry though)");
            httpd_resp_send(req, "err", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        return ESP_FAIL;
    }

    /* Log data received */
    ESP_LOGI(TAG, "=========== RECEIVED DATA ==========");
    ESP_LOGI(TAG, "%.*s", bytes_received, buf);
    ESP_LOGI(TAG, "====================================");

    // Should really be parsing this with prealloced buf. If user sends fat payload
    // we'll crash from heap overflow
    cJSON *payload = parse_json(buf);
    if (payload == NULL) {
        ESP_LOGE(TAG, "Couldn't parse json (TODO return the right code");
        httpd_resp_send(req, "err", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    cJSON *json_number_of_days = cJSON_GetObjectItem(payload, "number_of_days");
    if (cJSON_IsString(json_number_of_days)) {
        char *temp_number_of_days = cJSON_GetStringValue(json_number_of_days);
        if (strlen(temp_number_of_days) > MAX_LENGTH_NUMBER_OF_DAYS_PARAM) {
            ESP_LOGI(TAG, "Received number > %d digits, invalid. Defaulting to 2", MAX_LENGTH_NUMBER_OF_DAYS_PARAM);
            strcpy(_number_of_days, "2");
        } else {
            strcpy(_number_of_days, temp_number_of_days);
        }
    } else {
        ESP_LOGI(TAG, "Unable to parse number_of_days param, defaulting to 2");
        strcpy(_number_of_days, "2");
    }

    cJSON *json_spot_name = cJSON_GetObjectItem(payload, "spot_name");
    if (cJSON_IsString(json_spot_name)) {
        char *temp_spot_name = cJSON_GetStringValue(json_spot_name);
        if (strlen(temp_spot_name) > MAX_LENGTH_SPOT_NAME_PARAM) {
            ESP_LOGI(TAG, "Received spot_name > %d chars, invalid. Defaulting to wedge", MAX_LENGTH_SPOT_NAME_PARAM);
            strcpy(_spot_name, "wedge");
        } else {
            strcpy(_spot_name, temp_spot_name);
        }
    } else {
        ESP_LOGI(TAG, "Unable to parse spot_name param, defaulting to 'wedge'");
        strcpy(_spot_name, "wedge");
    }

    cJSON *json_spot_uid = cJSON_GetObjectItem(payload, "spot_uid");
    char *wedge_uid = "5842041f4e65fad6a770882b";
    if (cJSON_IsString(json_spot_uid)) {
        char *temp_spot_uid = cJSON_GetStringValue(json_spot_uid);
        if (strlen(temp_spot_uid) > MAX_LENGTH_SPOT_UID_PARAM) {
            char *wedge_uid = "";
            ESP_LOGI(TAG, "Received spot_uid > %d chars, invalid. Defaulting to wedge uid (%s)", MAX_LENGTH_SPOT_UID_PARAM, wedge_uid);
            strcpy(_spot_uid, wedge_uid);
        } else {
            strcpy(_spot_uid, temp_spot_uid);
        }
    } else {
        ESP_LOGI(TAG, "Unable to parse spot_uid param, defaulting to wedge uid (%s)", wedge_uid);
        strcpy(_spot_uid, wedge_uid);
    }

    bool one_valid_forecast_type = false;
    cJSON *json_forecast_types = cJSON_GetObjectItem(payload, "forecast_types");
    if (cJSON_IsArray(json_forecast_types)) {
        int index = 0;
        cJSON *json_forecast_type = NULL;
        cJSON_ArrayForEach(json_forecast_type, json_forecast_types) {
            char *temp_forecast_type = cJSON_GetStringValue(json_forecast_type);
            if (strlen(temp_forecast_type) > MAX_LENGTH_FORECAST_TYPE_PARAM) {
                ESP_LOGI(TAG, "Received forecast type > %d chars, invalid. Defaulting to empty", MAX_LENGTH_FORECAST_TYPE_PARAM);
            } else {
                strcpy(_forecast_types[index], temp_forecast_type);
                one_valid_forecast_type = true;
            }

            index++;
        }

        if (!one_valid_forecast_type) {
            ESP_LOGI(TAG, "Didn't get a single valid forecast type, defaulting to a single 'tides'");
            strcpy(_forecast_types[0], "tides");
        }
    } else {
        ESP_LOGI(TAG, "Unable to parse forecast_types param, defaulting to [\"tides\"]");
        strcpy(_forecast_types[0], "tides");
    }

    spot_check_config config = {
        .number_of_days = _number_of_days,
        .spot_name = _spot_name,
        .spot_uid = _spot_uid
    };
    int i;
    unsigned int num_forecast_types = 0;
    for (i = 0; i < MAX_NUM_FORECAST_TYPES; i++) {
        if (*_forecast_types[0] != 0) {
            config.forecast_types[i] = _forecast_types[i];
            num_forecast_types++;
        }
    }
    config.forecast_type_count = num_forecast_types;

    nvs_save_config(&config);

    // End response
    cJSON_Delete(payload);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t current_config_get_handler(httpd_req_t *req) {
    spot_check_config *current_config = nvs_get_config();
    const char **forecast_types_ptr = (const char **)current_config->forecast_types;

    cJSON *root = cJSON_CreateObject();
    cJSON *num_days_json = cJSON_CreateString(current_config->number_of_days);
    cJSON *spot_name_json = cJSON_CreateString(current_config->spot_name);
    cJSON *spot_uid_json = cJSON_CreateString(current_config->spot_uid);
    cJSON *forecast_types_json = cJSON_CreateStringArray(forecast_types_ptr, current_config->forecast_type_count);
    cJSON_AddItemToObject(root, "number_of_days", num_days_json);
    cJSON_AddItemToObject(root, "spot_name", spot_name_json);
    cJSON_AddItemToObject(root, "spot_uid", spot_uid_json);
    cJSON_AddItemToObject(root, "forecast_types", forecast_types_json);

    char *response_json = cJSON_Print(root);
    httpd_resp_send(req, response_json, HTTPD_RESP_USE_STRLEN);

    cJSON_Delete(root);
    cJSON_free(response_json);
    return ESP_OK;
}

void http_server_start() {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    ESP_LOGI(TAG, "Starting server on port: '%d'", 80);
    esp_err_t err;
    if ((err = httpd_start(&server, &config)) != ESP_OK) {
        ESP_LOGI(TAG, "Error starting webserver (%s), trying one more time", esp_err_to_name(err));
        ESP_ERROR_CHECK(httpd_start(&server, &config));
    }

    httpd_register_uri_handler(server, &configure_uri);
    httpd_register_uri_handler(server, &health_uri);
    httpd_register_uri_handler(server, &current_config_uri);

    server_handle = server;
}
