#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_http_client.h"
#include "esp_err.h"

#include "constants.h"
#include "http_client.h"

// Must included below constants.h where we overwite the define of LOG_LOCAL_LEVEL
#include "esp_log.h"

#define MAX_QUERY_PARAM_LENGTH 15
#define MAX_READ_BUFFER_SIZE 4096

static esp_http_client_handle_t client;

bool http_client_inited = false;

/* Technically unnecessary, should be stubbed out for non-debug build */
esp_err_t http_event_handler(esp_http_client_event_t *event) {
    switch(event->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", event->header_key, event->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", event->data_len);
            if (!esp_http_client_is_chunked_response(event->client)) {
                // Write out data
                // printf("%.*s", evt->data_len, (char*)evt->data);
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

void http_client_init() {
    if (http_client_inited) {
        ESP_LOGI(TAG, "http client already set up, no need to re-init");
        return;
    }

    ESP_LOGI(TAG, "initing http client...");

    // TODO :: build this URL with the same logic that
    // build_request uses to prevent wasting time forgetting
    // to update BASE_URL #define...
    esp_http_client_config_t http_config = {
        .url = "http://192.168.1.70/tides?spot_name=wedge&days=2",
        .event_handler = http_event_handler,
        .buffer_size = MAX_READ_BUFFER_SIZE
    };

    client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGI(TAG, "Error initing http client");
        return;
    }

    http_client_inited = true;
    ESP_LOGI(TAG, "Successful init of http client");
}

// Caller passes in endpoint (tides/swell) the values for the 2 query params,
// a pointer to a block of already-allocated memory for the base url + endpoint,
// and a pointer to a block of already-allocated memory to hold the query params structs
request http_client_build_request(char* endpoint, char *spot, char *days, char *url_buf, query_param *params) {
    query_param temp_params[] = {
        {
            .key = "days",
            .value = days
        },
        {
            .key = "spot",
            .value = spot
        }
    };

    memcpy(params, temp_params, sizeof(temp_params));

    strcpy(url_buf, URL_BASE);
    strcat(url_buf, endpoint);
    request tide_request = {
        .url = url_buf,
        .params = params,
        .num_params = sizeof(temp_params) / sizeof(query_param)
    };

    return tide_request;
}

/*
 * request obj is optional, but highly recommended to ensure the
 * right url/params are set up. If not supplied, request will be
 * performed using whatever was last set.
 */
int http_client_perform_request(request *request_obj, char **read_buffer) {
    if (request_obj) {
        // assume we won't have that many query params. Could calc this too
        char req_url[strlen(request_obj->url) + 40];
        strcpy(req_url, request_obj->url);
        strcat(req_url, "?");
        for (int i = 0; i < request_obj->num_params; i++) {
            query_param param = request_obj->params[i];
            strcat(req_url, param.key);
            strcat(req_url, "=");
            strcat(req_url, param.value);
        }

        ESP_ERROR_CHECK(esp_http_client_set_url(client, req_url));
        ESP_LOGI(TAG, "Setting url to %s", req_url);
    }

    esp_err_t error = esp_http_client_perform(client);
    if (error != ESP_OK) {
        const char *err_text = esp_err_to_name(error);
        ESP_LOGI(TAG, "Error performing test GET, error: %s", err_text);

        // clean up and re-init client
        error = esp_http_client_cleanup(client);
        if (error != ESP_OK) {
            ESP_LOGI(TAG, "Error cleaning up  http client connection");
        }

        http_client_inited = false;
        return 0;
    }

    int content_length = esp_http_client_get_content_length(client);
    int status = esp_http_client_get_status_code(client);
    if (status >= 200 && status <= 299) {
        ESP_LOGI(TAG, "GET success! Status=%d, Content-length=%d", status, content_length);
    } else {
        ESP_LOGI(TAG, "GET failed. Status=%d, Content-length=%d", status, content_length);
        error = esp_http_client_close(client);
        if (error != ESP_OK) {
            const char *err_str = esp_err_to_name(error);
            ESP_LOGI(TAG, "Error closing http client connection: %s", err_str);
            return 0;
        }
    }

    int alloced_space_used = 0;
    if (content_length < MAX_READ_BUFFER_SIZE) {
        // Did a lot of work here to try to read into buffer in chunks since default response buffer
        // inside client is inited to ~512 bytes but something's borked in the SDK. This is technically
        // double-allocating buffers of MAX_READ_BUFFER_SIZE since there's one internally and another
        // here, but hopefully the quick malloc/free shouldn't cause any issues
        *read_buffer = malloc(content_length + 1);
        int length_received = esp_http_client_read(client, *read_buffer, content_length);
        (*read_buffer)[length_received + 1] = '\0';
        alloced_space_used = length_received + 1;
    } else {
        ESP_LOGI(TAG, "Not enough room in read buffer: buffer=%d, content=%d", MAX_READ_BUFFER_SIZE, content_length);
    }

    // Close current connection but don't free http_client data and un-init with cleanup
    error = esp_http_client_close(client);
    if (error != ESP_OK) {
        const char *err_str = esp_err_to_name(error);
        ESP_LOGI(TAG, "Error closing http client connection: %s", err_str);
    }

    return alloced_space_used;
}
