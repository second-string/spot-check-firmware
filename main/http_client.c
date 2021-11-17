#include <string.h>
#include "freertos/FreeRTOS.h"

#include "constants.h"
#include "http_client.h"

// Must included below constants.h where we overwite the define of LOG_LOCAL_LEVEL
#include "esp_log.h"

#define TAG "sc-http-client"

#define MAX_QUERY_PARAM_LENGTH 15
#define MAX_READ_BUFFER_SIZE 4096

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");

/* Technically unnecessary, should be stubbed out for non-debug build */
esp_err_t http_event_handler(esp_http_client_event_t *event) {
    switch (event->event_id) {
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
}

// Caller passes in endpoint (tides/swell) the values for the 2 query params,
// a pointer to a block of already-allocated memory for the base url + endpoint,
// and a pointer to a block of already-allocated memory to hold the query params structs
request http_client_build_request(char *             endpoint,
                                  spot_check_config *config,
                                  char *             url_buf,
                                  query_param *      params,
                                  uint8_t            num_params) {
    query_param temp_params[num_params];
    if (strcmp(endpoint, "conditions") == 0) {
        temp_params[0] = (query_param){.key = "lat", .value = config->spot_lat};
        temp_params[1] = (query_param){.key = "lon", .value = config->spot_lon};
        temp_params[2] = (query_param){.key = "spot_id", .value = config->spot_uid};
    } else {
        temp_params[0] = (query_param){.key = "days", .value = config->number_of_days};
        temp_params[1] = (query_param){.key = "spot_id", .value = config->spot_uid};
    }

    memcpy(params, temp_params, sizeof(temp_params));

    strcpy(url_buf, URL_BASE);
    strcat(url_buf, endpoint);
    request tide_request = {.url = url_buf, .params = params, .num_params = sizeof(temp_params) / sizeof(query_param)};

    return tide_request;
}

/*
 * request obj is optional, but highly recommended to ensure the
 * right url/params are set up. If not supplied, request will be
 * performed using whatever was last set.
 */
int http_client_perform_request(request *request_obj, char **read_buffer) {
    if (!connected_to_network) {
        ESP_LOGI(TAG, "Attempted to make GET request, not connected to internet yet so bailing");
        return 0;
    }

    ESP_LOGI(TAG, "Initing http client for request...");

    esp_http_client_config_t http_config = {
        .host           = "spotcheck.brianteam.dev",
        .path           = "/",
        .event_handler  = http_event_handler,
        .buffer_size    = MAX_READ_BUFFER_SIZE,
        .cert_pem       = (char *)&server_cert_pem_start,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGI(TAG, "Error initing http client, returning without sending request");
        return 0;
    }

    if (request_obj) {
        // assume we won't have that many query params. Could calc this too
        char req_url[strlen(request_obj->url) + 60];
        strcpy(req_url, request_obj->url);
        strcat(req_url, "?");
        for (int i = 0; i < request_obj->num_params; i++) {
            query_param param = request_obj->params[i];
            strcat(req_url, param.key);
            strcat(req_url, "=");
            strcat(req_url, param.value);
            strcat(req_url, "&");
        }

        ESP_ERROR_CHECK(esp_http_client_set_url(client, req_url));
        ESP_LOGI(TAG, "Setting url to %s", req_url);
    }

    ESP_ERROR_CHECK(esp_http_client_set_method(client, HTTP_METHOD_GET));
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "Content-type", "text/html"));

    esp_err_t error = esp_http_client_perform(client);
    if (error != ESP_OK) {
        const char *err_text = esp_err_to_name(error);
        ESP_LOGI(TAG, "Error performing test GET, error: %s", err_text);

        // clean up and return no space allocated
        error = esp_http_client_cleanup(client);
        if (error != ESP_OK) {
            ESP_LOGI(TAG, "Error cleaning up  http client connection");
        }

        return 0;
    }

    int content_length = esp_http_client_get_content_length(client);
    int status         = esp_http_client_get_status_code(client);
    if (status >= 200 && status <= 299) {
        if (content_length < 0) {
            ESP_LOGI(TAG, "Got success status (%d) but no content in response, bailing", status);
            // clean up and return no space allocated
            error = esp_http_client_cleanup(client);
            if (error != ESP_OK) {
                ESP_LOGI(TAG, "Error cleaning up  http client connection");
            }

            return 0;
        }

        ESP_LOGI(TAG, "GET success! Status=%d, Content-length=%d", status, content_length);
    } else {
        ESP_LOGI(TAG,
                 "GET failed. Setting content_length to zero to skip to cleanup at end of function. Status=%d, "
                 "Content-length=%d",
                 status,
                 content_length);
        content_length = 0;
    }

    int alloced_space_used = 0;
    if (content_length < MAX_READ_BUFFER_SIZE) {
        // Did a lot of work here to try to read into buffer in chunks since default response buffer
        // inside client is inited to ~512 bytes but something's borked in the SDK. This is technically
        // double-allocating buffers of MAX_READ_BUFFER_SIZE since there's one internally and another
        // here, but hopefully the quick malloc/free shouldn't cause any issues
        *read_buffer                    = malloc(content_length + 1);
        int length_received             = esp_http_client_read(client, *read_buffer, content_length);
        (*read_buffer)[length_received] = '\0';
        alloced_space_used              = length_received + 1;
    } else {
        ESP_LOGI(TAG, "Not enough room in read buffer: buffer=%d, content=%d", MAX_READ_BUFFER_SIZE, content_length);
    }

    error = esp_http_client_cleanup(client);
    if (error != ESP_OK) {
        ESP_LOGI(TAG, "Error cleaning up  http client connection");
    }
    ESP_LOGI(TAG, "Cleaned up http client after request");

    return alloced_space_used;
}

// 0 for success, error code if not
int http_client_perform_post(request *request_obj,
                             char *   post_data,
                             size_t   post_data_size,
                             char *   response_data,
                             size_t * response_data_size) {
    if (!connected_to_network) {
        ESP_LOGI(TAG, "Attempted to make POST request, not connected to internet yet so bailing");
        return 0;
    }

    ESP_LOGI(TAG, "Initing http client for post...");

    esp_http_client_config_t http_config = {
        .host           = "spotcheck.brianteam.dev",
        .path           = "/",
        .event_handler  = http_event_handler,
        .buffer_size    = MAX_READ_BUFFER_SIZE,
        .cert_pem       = (char *)&server_cert_pem_start,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        ESP_LOGI(TAG, "Error initing http client, returning without sending request");
        return 0;
    }

    int retval = 0;

    ESP_ERROR_CHECK(esp_http_client_set_url(client, request_obj->url));
    ESP_ERROR_CHECK(esp_http_client_set_method(client, HTTP_METHOD_POST));
    ESP_ERROR_CHECK(esp_http_client_set_header(client, "Content-type", "application/json"));
    ESP_ERROR_CHECK(esp_http_client_set_post_field(client, post_data, post_data_size));

    ESP_LOGI(TAG, "Performing POST to %s with data size %u", request_obj->url, post_data_size);
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error performing POST, error: %s", esp_err_to_name(err));
        retval = -1;
    } else {
        retval = esp_http_client_get_status_code(client);
    }

    int content_length = esp_http_client_get_content_length(client);
    int status         = esp_http_client_get_status_code(client);
    if (status >= 200 && status <= 299) {
        if (content_length < 0) {
            ESP_LOGI(TAG, "Got success status (%d) but no content in response, bailing", status);
            esp_err_t error = esp_http_client_cleanup(client);
            if (error != ESP_OK) {
                ESP_LOGI(TAG, "Error cleaning up  http client connection");
            }

            return retval;
        }

        ESP_LOGI(TAG, "POST success! Status=%d, Content-length=%d", status, content_length);
    } else {
        ESP_LOGI(
            TAG,
            "POST failed. Setting content_length to zero to skip to cleanup and return.  Status=%d, Content-length=%d",
            status,
            content_length);
        content_length = 0;
    }

    int alloced_space_used = 0;
    if (content_length < MAX_READ_BUFFER_SIZE) {
        int length_received            = esp_http_client_read(client, response_data, content_length);
        response_data[length_received] = '\0';
        alloced_space_used             = length_received + 1;
    } else {
        ESP_LOGI(TAG, "Not enough room in read buffer: buffer=%d, content=%d", MAX_READ_BUFFER_SIZE, content_length);
    }

    *response_data_size = alloced_space_used;

    esp_err_t error = esp_http_client_cleanup(client);
    if (error != ESP_OK) {
        ESP_LOGI(TAG, "Error cleaning up  http client connection");
    }
    ESP_LOGI(TAG, "Cleaned up http client after request");

    // Return 0 if successful, the error code or -1 if anything else
    return retval >= 200 && retval <= 200 ? 0 : retval;
}
