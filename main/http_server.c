#include "constants.h"

#include <esp_http_server.h>
#include <esp_system.h>
#include <log.h>
#include <sys/param.h>

#include "memfault/panics/assert.h"

#include "constants.h"
#include "http_client.h"
#include "http_server.h"
#include "json.h"
#include "nvs.h"
#include "scheduler_task.h"
#include "screen_img_handler.h"
#include "sntp_time.h"
#include "spot_check.h"

#define TAG SC_TAG_HTTP_SERVER

static httpd_handle_t server_handle = NULL;

static esp_err_t health_get_handler(httpd_req_t *req);
static esp_err_t configure_post_handler(httpd_req_t *req);
static esp_err_t current_config_get_handler(httpd_req_t *req);
static esp_err_t clear_nvs_post_handler(httpd_req_t *req);
static esp_err_t set_time_post_handler(httpd_req_t *req);

static const httpd_uri_t health_uri = {.uri      = "/health",
                                       .method   = HTTP_GET,
                                       .handler  = health_get_handler,
                                       .user_ctx = NULL};

static const httpd_uri_t configure_uri = {.uri      = "/configure",
                                          .method   = HTTP_POST,
                                          .handler  = configure_post_handler,
                                          .user_ctx = NULL};

static const httpd_uri_t current_config_uri = {.uri      = "/current_configuration",
                                               .method   = HTTP_GET,
                                               .handler  = current_config_get_handler,
                                               .user_ctx = NULL};

static const httpd_uri_t clear_nvs_uri = {.uri      = "/clear_nvs",
                                          .method   = HTTP_POST,
                                          .handler  = clear_nvs_post_handler,
                                          .user_ctx = NULL};

static const httpd_uri_t set_time_uri = {.uri      = "/set_time",
                                         .method   = HTTP_POST,
                                         .handler  = set_time_post_handler,
                                         .user_ctx = NULL};

/*
 * Caller responsible for deleting malloced cJSON payload with cJSON_Delete!
 */
static bool http_server_parse_post_body(httpd_req_t *req, cJSON **payload) {
    const int rx_buf_size = 300;
    char      buf[rx_buf_size];

    if (req->content_len > rx_buf_size) {
        log_printf(LOG_LEVEL_ERROR, "Payload is too big (%d bytes), bailing out", req->content_len);
        httpd_resp_send(req, "err", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    int bytes_received = httpd_req_recv(req, buf, MIN(req->content_len, rx_buf_size));
    if (bytes_received <= 0) {
        if (bytes_received == HTTPD_SOCK_ERR_TIMEOUT) {
            /* Retry receiving if timeout occurred */
            // continue;
            log_printf(LOG_LEVEL_ERROR, "Received timeout, bailing out (could retry though)");
            httpd_resp_send(req, "err", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        return false;
    }

    /* Log data received */
    log_printf(LOG_LEVEL_INFO, "=========== RECEIVED DATA ==========");
    log_printf(LOG_LEVEL_INFO, "%.*s", bytes_received, buf);
    log_printf(LOG_LEVEL_INFO, "====================================");

    // Should really be parsing this with prealloced buf. If user sends fat payload
    // we'll crash from heap overflow
    *payload = parse_json(buf);
    if (payload == NULL) {
        log_printf(LOG_LEVEL_ERROR, "Couldn't parse json (TODO return the right code");
        httpd_resp_send(req, "err", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    return true;
}

/*
 * Generic function for parsing out a string value from a json key with built-in error handling and fallback value. It
 * does not COPY the value into the field_to_set arg, but assigns the original arg pointer (by the deref of ptr to a
 * ptr) to the available/alloced string. This avoids an extra buffer declaration, because the value to set (either
 * what's in the json object or the fallback string) is already declared and will persist. Caller must ensure that the
 * json object's string and the fallback must remain alloced until whatever has used their values is done with them
 * (i.e. config object is saved)!
 */
static void http_server_parse_json_string(cJSON *payload,
                                          char  *json_key,
                                          char **field_to_set,
                                          size_t max_field_length,
                                          char  *fallback) {
    cJSON *json_obj = cJSON_GetObjectItem(payload, json_key);
    if (cJSON_IsString(json_obj)) {
        char *rx_strvalue = cJSON_GetStringValue(json_obj);

        if (strlen(rx_strvalue) > max_field_length) {
            log_printf(LOG_LEVEL_INFO,
                       "Received value '%s' > %d chars, invalid. Defaulting to '%s'",
                       rx_strvalue,
                       max_field_length,
                       fallback);
            *field_to_set = fallback;
        } else {
            // rx_strvalue is the char pointer held within the cjson object, so it will be a valid pointer until json
            // object is freed and is not tied to the local scope here
            *field_to_set = rx_strvalue;
        }
    } else {
        log_printf(LOG_LEVEL_WARN, "Unable to parse param, defaulting to '%s'", fallback);
        *field_to_set = fallback;
    }
}

static esp_err_t health_get_handler(httpd_req_t *req) {
    httpd_resp_send(req, "Surviving not thriving", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t configure_post_handler(httpd_req_t *req) {
    cJSON *payload;
    MEMFAULT_ASSERT(http_server_parse_post_body(req, &payload));
    vTaskDelay(pdMS_TO_TICKS(400));

    // Load all our values into here to save to nvs. No need to alloc
    // any more memory than the json takes because nvs will use those
    // pointers to write directly to flash
    spot_check_config_t config                  = {0};
    char               *default_tz_str          = "CET-1CEST,M3.4.0/2,M10.4.0/2";
    char               *default_tz_display_name = "Europe/Berlin";
    char               *default_mode            = (char *)spot_check_mode_to_string(SPOT_CHECK_MODE_WEATHER);

    // Parse default fields first which are always included, then parse specific fields based on rxd mode
    http_server_parse_json_string(payload, "tz_str", &config.tz_str, MAX_LENGTH_TZ_STR_PARAM, default_tz_str);
    http_server_parse_json_string(payload,
                                  "tz_display_name",
                                  &config.tz_display_name,
                                  MAX_LENGTH_TZ_DISPLAY_NAME_PARAM,
                                  default_tz_display_name);
    char *temp_mode_str;
    http_server_parse_json_string(payload,
                                  "operating_mode",
                                  &temp_mode_str,
                                  MAX_LENGTH_OPERATING_MODE_PARAM,
                                  default_mode);
    config.operating_mode = spot_check_string_to_mode(temp_mode_str);

    switch (config.operating_mode) {
        case SPOT_CHECK_MODE_WEATHER: {
            char *default_spot_name    = "The Wedge";
            char *default_spot_lat     = "33.5930302087";
            char *default_spot_lon     = "-117.8819918632";
            char *default_spot_uid     = "5842041f4e65fad6a770882b";
            char *default_active_chart = "tide";
            char *temp_active_chart;

            http_server_parse_json_string(payload,
                                          "spot_name",
                                          &config.spot_name,
                                          MAX_LENGTH_SPOT_NAME_PARAM,
                                          default_spot_name);
            http_server_parse_json_string(payload,
                                          "spot_lat",
                                          &config.spot_lat,
                                          MAX_LENGTH_SPOT_LAT_PARAM,
                                          default_spot_lat);
            http_server_parse_json_string(payload,
                                          "spot_lon",
                                          &config.spot_lon,
                                          MAX_LENGTH_SPOT_LON_PARAM,
                                          default_spot_lon);
            http_server_parse_json_string(payload,
                                          "spot_uid",
                                          &config.spot_uid,
                                          MAX_LENGTH_SPOT_UID_PARAM,
                                          default_spot_uid);
            http_server_parse_json_string(payload,
                                          "active_chart_1",
                                          &temp_active_chart,
                                          MAX_LENGTH_ACTIVE_CHART_PARAM,
                                          default_active_chart);
            MEMFAULT_ASSERT(nvs_chart_string_to_enum(temp_active_chart, &config.active_chart_1));

            http_server_parse_json_string(payload,
                                          "active_chart_2",
                                          &temp_active_chart,
                                          MAX_LENGTH_ACTIVE_CHART_PARAM,
                                          default_active_chart);
            MEMFAULT_ASSERT(nvs_chart_string_to_enum(temp_active_chart, &config.active_chart_2));
            break;
        }
        case SPOT_CHECK_MODE_CUSTOM: {
            // Example default image already on server, default update interval 1 hour
            char *default_custom_screen_url           = URL_BASE "custom_screen_test_image";
            char *default_custom_update_interval_secs = "3600";
            http_server_parse_json_string(payload,
                                          "custom_screen_url",
                                          &config.custom_screen_url,
                                          MAX_LENGTH_CUSTOM_SCREEN_URL_PARAM,
                                          default_custom_screen_url);
            log_printf(LOG_LEVEL_WARN, "config custm val: %s", config.custom_screen_url);

            char *temp_interval_str;
            http_server_parse_json_string(payload,
                                          "custom_update_interval_secs",
                                          &temp_interval_str,
                                          MAX_LENGTH_CUSTOM_UPDATE_INTERVAL_SECS_PARAM,
                                          default_custom_update_interval_secs);
            uint32_t temp_interval = strtoul(temp_interval_str, NULL, 10);
            if (temp_interval < 900) {
                log_printf(LOG_LEVEL_WARN,
                           "Attempt to set custom_update_interval_secs to a value too low (%u) - defaulting to 900 "
                           "secs (15 min)",
                           temp_interval);
                temp_interval = 900;
            }

            config.custom_update_interval_secs = temp_interval;
            break;
        }
        default:
            log_printf(LOG_LEVEL_ERROR,
                       "spot_check_mode_t enum value '%u' not supported in %s",
                       config.operating_mode,
                       __func__);
    }

    // Release client before we do time-intensive stuff with flash
    cJSON_Delete(payload);
    httpd_resp_send(req, NULL, 0);

    nvs_save_config(&config);

    // TODO : previously we were setting the tz str and forcing a time redraw through scheduler, but that wouldn't
    // properly re render everything else if the spot changed right? For now, reboot in all cases to make things super
    // simple, but in the future this could be smarter to only reboot on mode change or gracefully handle all cases
    //
    // TODO :: schedule restart so we finish logging and properly close res conn, don't do it here
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    while (1) {
    }

    return ESP_OK;
}

static esp_err_t current_config_get_handler(httpd_req_t *req) {
    spot_check_config_t *current_config = nvs_get_config();

    cJSON *root                        = cJSON_CreateObject();
    cJSON *spot_name_json              = cJSON_CreateString(current_config->spot_name);
    cJSON *spot_lat_json               = cJSON_CreateString(current_config->spot_lat);
    cJSON *spot_lon_json               = cJSON_CreateString(current_config->spot_lon);
    cJSON *spot_uid_json               = cJSON_CreateString(current_config->spot_uid);
    cJSON *tz_str_json                 = cJSON_CreateString(current_config->tz_str);
    cJSON *tz_display_name_json        = cJSON_CreateString(current_config->tz_display_name);
    cJSON *operating_mode              = cJSON_CreateString(spot_check_mode_to_string(current_config->operating_mode));
    cJSON *custom_screen_url           = cJSON_CreateString(current_config->custom_screen_url);
    cJSON *custom_update_interval_secs = cJSON_CreateNumber(current_config->custom_update_interval_secs);

    char temp_chart_str[10];
    nvs_chart_enum_to_string(current_config->active_chart_1, temp_chart_str);
    cJSON *active_chart_1 = cJSON_CreateString(temp_chart_str);
    nvs_chart_enum_to_string(current_config->active_chart_2, temp_chart_str);
    cJSON *active_chart_2 = cJSON_CreateString(temp_chart_str);

    cJSON_AddItemToObject(root, "spot_name", spot_name_json);
    cJSON_AddItemToObject(root, "spot_lat", spot_lat_json);
    cJSON_AddItemToObject(root, "spot_lon", spot_lon_json);
    cJSON_AddItemToObject(root, "spot_uid", spot_uid_json);
    cJSON_AddItemToObject(root, "tz_str", tz_str_json);
    cJSON_AddItemToObject(root, "tz_display_name", tz_display_name_json);
    cJSON_AddItemToObject(root, "operating_mode", operating_mode);
    cJSON_AddItemToObject(root, "custom_screen_url", custom_screen_url);
    cJSON_AddItemToObject(root, "custom_update_interval_secs", custom_update_interval_secs);
    cJSON_AddItemToObject(root, "active_chart_1", active_chart_1);
    cJSON_AddItemToObject(root, "active_chart_2", active_chart_2);

    char *response_json = cJSON_Print(root);
    httpd_resp_send(req, response_json, HTTPD_RESP_USE_STRLEN);
    log_printf(LOG_LEVEL_DEBUG, "HTTP server response: %s", response_json);

    cJSON_Delete(root);
    cJSON_free(response_json);
    return ESP_OK;
}

static esp_err_t clear_nvs_post_handler(httpd_req_t *req) {
    int  query_buf_len = 30;
    char query_buf[query_buf_len];
    int  actual_query_len = httpd_req_get_url_query_len(req) + 1;
    if (actual_query_len > query_buf_len) {
        log_printf(LOG_LEVEL_INFO,
                   "Query str too long for buffer (%d long, can only fit %d)",
                   actual_query_len,
                   query_buf_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid query string");
        return ESP_OK;
    }

    if (httpd_req_get_url_query_str(req, query_buf, actual_query_len) == ESP_OK) {
        char value[15];
        if (httpd_query_key_value(query_buf, "key", value, sizeof(value)) == ESP_OK) {
            if (strcmp(value, "sekrit") == 0) {
                // Clearing nvs takes a long time, send req before kicking off
                httpd_resp_send(req,
                                "Successfully received cmd to clear NVS, clearing and will reboot after",
                                HTTPD_RESP_USE_STRLEN);
                ESP_ERROR_CHECK(nvs_full_erase());
                esp_restart();
            } else {
                log_printf(LOG_LEVEL_INFO, "Received incorrect key for erasing flash: %s", value);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid query string");
            }
        }
    } else {
        log_printf(LOG_LEVEL_INFO, "Failed to get query string");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get query string");
    }

    return ESP_OK;
}

static esp_err_t set_time_post_handler(httpd_req_t *req) {
    cJSON *payload;
    MEMFAULT_ASSERT(http_server_parse_post_body(req, &payload));

    // TODO :: this appears to only work with GMT time, then SNTP internally converts it to local time based on internal
    // tz_str already set
    cJSON *json_epoch_secs = cJSON_GetObjectItem(payload, "epoch_secs");
    if (cJSON_IsNumber(json_epoch_secs)) {
        // Set time and de-init sntp to keep user's manual time set
        uint32_t epoch_secs = cJSON_GetNumberValue(json_epoch_secs);
        sntp_set_time(epoch_secs);
        sntp_time_stop();
    } else {
        log_printf(LOG_LEVEL_INFO, "Unable to parse epoch_secs param, not changing time");
    }

    // End response
    cJSON_Delete(payload);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void http_server_start() {
    if (server_handle) {
        log_printf(LOG_LEVEL_WARN, "http_server already started and http_server_start called, ignoring and bailing");
        return;
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    log_printf(LOG_LEVEL_INFO, "Starting server on port: '%d'", 80);
    esp_err_t err;
    if ((err = httpd_start(&server, &config)) != ESP_OK) {
        log_printf(LOG_LEVEL_INFO, "Error starting webserver (%s), trying one more time", esp_err_to_name(err));
        err = httpd_start(&server, &config);
        if (err != ESP_OK) {
            log_printf(LOG_LEVEL_INFO,
                       "Error starting webserver (%s) for the second time, rebooting...",
                       esp_err_to_name(err));
            esp_restart();
        }
    }

    httpd_register_uri_handler(server, &configure_uri);
    httpd_register_uri_handler(server, &health_uri);
    httpd_register_uri_handler(server, &current_config_uri);
    httpd_register_uri_handler(server, &clear_nvs_uri);
    httpd_register_uri_handler(server, &set_time_uri);

    server_handle = server;
}

void http_server_stop() {
    if (server_handle == NULL) {
        log_printf(LOG_LEVEL_WARN, "http_server not running and http_server_stop called, ignoring.");
        return;
    }

    ESP_ERROR_CHECK(httpd_stop(server_handle));
    server_handle = NULL;
}
