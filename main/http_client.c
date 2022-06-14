#include <string.h>
#include "freertos/FreeRTOS.h"

#include "constants.h"
#include "esp_partition.h"
#include "http_client.h"
#include "wifi.h"

// Must included below constants.h where we overwite the define of LOG_LOCAL_LEVEL
#include "log.h"

#define TAG "sc-http-client"

#define MAX_QUERY_PARAM_LENGTH 15
#define MAX_READ_BUFFER_SIZE 1024

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");

/* Technically unnecessary, should be stubbed out for non-debug build */
esp_err_t http_event_handler(esp_http_client_event_t *event) {
    switch (event->event_id) {
        case HTTP_EVENT_ERROR:
            log_printf(TAG, LOG_LEVEL_DEBUG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            log_printf(TAG, LOG_LEVEL_DEBUG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            log_printf(TAG, LOG_LEVEL_DEBUG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            log_printf(TAG,
                       LOG_LEVEL_DEBUG,
                       "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
                       event->header_key,
                       event->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            log_printf(TAG, LOG_LEVEL_DEBUG, "HTTP_EVENT_ON_DATA, len=%d", event->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            log_printf(TAG, LOG_LEVEL_DEBUG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            log_printf(TAG, LOG_LEVEL_DEBUG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            log_printf(TAG, LOG_LEVEL_DEBUG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

void http_client_init() {
}

// Caller passes in endpoint (tides/swell) the values for the 2 query params,
// a pointer to a block of already-allocated memory for the base url + endpoint,
// and a pointer to a block of already-allocated memory to hold the query params structs
request http_client_build_request(char              *endpoint,
                                  spot_check_config *config,
                                  char              *url_buf,
                                  query_param       *params,
                                  uint8_t            num_params) {
    request req = {0};
    if (params && num_params > 0) {
        query_param temp_params[num_params];
        if (strcmp(endpoint, "conditions") == 0 || strcmp(endpoint, "screen_update") == 0) {
            temp_params[0] = (query_param){.key = "lat", .value = config->spot_lat};
            temp_params[1] = (query_param){.key = "lon", .value = config->spot_lon};
            temp_params[2] = (query_param){.key = "spot_id", .value = config->spot_uid};
        } else if (strcmp(endpoint, "tides") == 0 || strcmp(endpoint, "swell") == 0) {
            temp_params[0] = (query_param){.key = "days", .value = config->number_of_days};
            temp_params[1] = (query_param){.key = "spot_id", .value = config->spot_uid};
        }
        memcpy(params, temp_params, sizeof(temp_params));
        req.num_params = sizeof(temp_params) / sizeof(query_param);
        req.params     = params;
    }

    strcpy(url_buf, URL_BASE);
    if (endpoint) {
        strcat(url_buf, endpoint);
    }
    req.url = url_buf;

    return req;
}

/*
 * NOTE: only performs the HTTP request. Does not read out response data, does not clean up client (unless there was a
 * failure). Caller responsible for calling http_client_read_response after this function to read data into buffer and
 * close client, OR manually reading data and closing in the case of maniupulating large responses as they're chunked
 * in.
 */
bool http_client_perform_request(request *request_obj, esp_http_client_handle_t *client) {
    configASSERT(client);

    if (!wifi_is_network_connected()) {
        log_printf(TAG, LOG_LEVEL_INFO, "Attempted to make GET request, not connected to internet yet so bailing");
        return 0;
    }

    log_printf(TAG, LOG_LEVEL_INFO, "Initing http client for request...");

    esp_http_client_config_t http_config = {
        .host           = "spotcheck.brianteam.dev",
        .path           = "/",
        .event_handler  = http_event_handler,
        .buffer_size    = MAX_READ_BUFFER_SIZE,
        .cert_pem       = (char *)&server_cert_pem_start,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    *client = esp_http_client_init(&http_config);
    if (!client) {
        log_printf(TAG, LOG_LEVEL_INFO, "Error initing http client, returning without sending request");
        return 0;
    }

    if (request_obj) {
        // assume we won't have that many query params. Could calc this too
        char req_url[strlen(request_obj->url) + 60];
        strcpy(req_url, request_obj->url);

        if (request_obj->num_params > 0) {
            strcat(req_url, "?");
            for (int i = 0; i < request_obj->num_params; i++) {
                query_param param = request_obj->params[i];
                strcat(req_url, param.key);
                strcat(req_url, "=");
                strcat(req_url, param.value);
                strcat(req_url, "&");
            }
        }

        ESP_ERROR_CHECK(esp_http_client_set_url(*client, req_url));
        log_printf(TAG, LOG_LEVEL_INFO, "Setting url to %s", req_url);
    }

    ESP_ERROR_CHECK(esp_http_client_set_method(*client, HTTP_METHOD_GET));
    ESP_ERROR_CHECK(esp_http_client_set_header(*client, "Content-type", "text/html"));

    esp_err_t err = esp_http_client_open(*client, 0);
    // Opens and sends the request since we have no data
    if (err != ESP_OK) {
        log_printf(TAG, LOG_LEVEL_ERROR, "Error opening http client, error: %s", esp_err_to_name(err));

        // clean up and return no space allocated
        err = esp_http_client_cleanup(*client);
        if (err != ESP_OK) {
            log_printf(TAG, LOG_LEVEL_INFO, "Error cleaning up  http client connection");
        }

        return false;
    }

    return true;
}

// TODO :: refactor the perform functions to be one func with arg for post or get. Almost identical now, just have to
// resolve the small differences in implementation of the URL build w/ query params and setting client values. Will need
// to include a call to esp_http_client_write for the post case
int http_client_perform_post(request                  *request_obj,
                             char                     *post_data,
                             size_t                    post_data_size,
                             esp_http_client_handle_t *client) {
    if (!wifi_is_network_connected()) {
        log_printf(TAG, LOG_LEVEL_INFO, "Attempted to make POST request, not connected to internet yet so bailing");
        return 0;
    }

    log_printf(TAG, LOG_LEVEL_INFO, "Initing http client for post...");

    esp_http_client_config_t http_config = {
        .host           = "spotcheck.brianteam.dev",
        .path           = "/",
        .event_handler  = http_event_handler,
        .buffer_size    = MAX_READ_BUFFER_SIZE,
        .cert_pem       = (char *)&server_cert_pem_start,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    *client = esp_http_client_init(&http_config);
    if (!client) {
        log_printf(TAG, LOG_LEVEL_INFO, "Error initing http client, returning without sending request");
        return 0;
    }

    int retval = 0;

    ESP_ERROR_CHECK(esp_http_client_set_url(*client, request_obj->url));
    ESP_ERROR_CHECK(esp_http_client_set_method(*client, HTTP_METHOD_POST));
    ESP_ERROR_CHECK(esp_http_client_set_header(*client, "Content-type", "application/json"));
    ESP_ERROR_CHECK(esp_http_client_set_post_field(*client, post_data, post_data_size));

    log_printf(TAG, LOG_LEVEL_INFO, "Performing POST to %s with data size %u", request_obj->url, post_data_size);
    esp_err_t err = esp_http_client_perform(*client);
    if (err != ESP_OK) {
        log_printf(TAG, LOG_LEVEL_ERROR, "Error performing POST, error: %s", esp_err_to_name(err));
        retval = -1;
    } else {
        uint16_t status = esp_http_client_get_status_code(*client);
        retval          = status <= 200 || status > 299;
    }

    return retval;
}

/*
 * Read response data into caller-supplied buffer from open request in caller-supplied client. Request must have been
 * sent through client using http_client_perform_request.
 * Returns error code, return bytes malloced through response_data_size which caller is responsible for freeing.
 */
int http_client_read_response(esp_http_client_handle_t *client, char **response_data, size_t *response_data_size) {
    configASSERT(client);

    int retval = 0;

    // Kicks off and blocks until all headers downloaded. Can get remainder of headers (like status_code) from their
    // helper functions, content_length is returned directly
    int content_length = esp_http_client_fetch_headers(*client);

    size_t bytes_received = 0;
    do {
        // Check status to make sure we have actual good data to read out
        int status = esp_http_client_get_status_code(*client);
        if (status >= 200 && status <= 299) {
            log_printf(TAG, LOG_LEVEL_INFO, "Request success! Status=%d, Content-length=%d", status, content_length);

            if (content_length < 0) {
                log_printf(TAG, LOG_LEVEL_INFO, "Error fetching headers with negative content-length, bailing");
                bytes_received = 0;
                retval         = -1;
                break;
            } else if (content_length == 0) {
                log_printf(TAG, LOG_LEVEL_ERROR, "Content length of zero after fetching headers, bailing");
                bytes_received = 0;
                retval         = -1;
                break;
            }

        } else {
            log_printf(TAG,
                       LOG_LEVEL_INFO,
                       "Request failed: status=%d, "
                       "Content-length=%d",
                       status,
                       content_length);
            bytes_received = 0;
            retval         = -1;
            break;
        }

        // If content-length is less than the max buffer size, we assume it's a regular API response (json, protobuf,
        // whatever), and read it all in one go. If it's more, we assume we're receiving a screen image that we'll need
        // to save to flash, since that's the only endpoint that currently returns anywhere near more data than the max
        // buffer size.
        if (content_length < MAX_READ_BUFFER_SIZE) {
            *response_data      = malloc(content_length + 1);
            int length_received = esp_http_client_read(*client, *response_data, content_length);
            if (length_received < 0) {
                // Don't bother making the caller free anything since we have nothing to give them. Free here and return
                // 0 alloced
                bytes_received = 0;
                free(*response_data);
                log_printf(TAG, LOG_LEVEL_ERROR, "Error reading response after successful http client request");
                break;
            } else {
                (*response_data)[length_received] = '\0';
                bytes_received                    = length_received + 1;
                log_printf(TAG, LOG_LEVEL_DEBUG, "Rcvd %zu bytes of response data: %s", bytes_received, *response_data);
            }
        } else {
            log_printf(
                TAG,
                LOG_LEVEL_INFO,
                "Content-length of %d too big for max local buffer of %d bytes. Assuming this is an image, chunking "
                "response and saving to flash partition",
                MAX_READ_BUFFER_SIZE,
                content_length);

            const esp_partition_t *screen_img_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                                   ESP_PARTITION_SUBTYPE_ANY,
                                                                                   SCREEN_IMG_PARTITION_LABEL);

            // Erase only the size of the image currently stored (internal spi flash functions will erase to page
            // boundary automatically)
            uint32_t screen_img_size = 0;
            nvs_get_uint32(SCREEN_IMG_SIZE_NVS_KEY, &screen_img_size);
            if (screen_img_size) {
                esp_partition_erase_range(screen_img_partition, 0x0, screen_img_size);
            } else {
                log_printf(TAG,
                           LOG_LEVEL_DEBUG,
                           "%s NVS key had zero value, not erasing any of screen img partition",
                           SCREEN_IMG_SIZE_NVS_KEY);
            }

            uint32_t moving_screen_img_addr = 0x0;
            int      length_received        = 0;
            *response_data                  = malloc(MAX_READ_BUFFER_SIZE);
            do {
                // Pull in chunk and immediately write to flash
                length_received = esp_http_client_read(*client, *response_data, MAX_READ_BUFFER_SIZE);
                if (length_received > 0) {
                    esp_partition_write(screen_img_partition, moving_screen_img_addr, *response_data, length_received);
                    log_printf(TAG,
                               LOG_LEVEL_DEBUG,
                               "Wrote %d bytes to screen image partition at offset %d",
                               length_received,
                               moving_screen_img_addr);
                    moving_screen_img_addr += length_received;
                }
            } while (length_received > 0);

            bytes_received = MAX_READ_BUFFER_SIZE;
            if (length_received < 0) {
                // TODO :: Figure out what to do in error case
                log_printf(TAG, LOG_LEVEL_ERROR, "Error reading response after successful http client request");
                retval = -1;
            } else {
                // Save metadata as last action to make sure all steps have succeeded and there's a valid image in flash
                nvs_set_uint32(SCREEN_IMG_SIZE_NVS_KEY, moving_screen_img_addr);
                nvs_set_uint32(SCREEN_IMG_WIDTH_PX_NVS_KEY, 100);
                nvs_set_uint32(SCREEN_IMG_HEIGHT_PX_NVS_KEY, 50);
                log_printf(TAG,
                           LOG_LEVEL_DEBUG,
                           "Rcvd %zu bytes total of response data and saved to flash",
                           bytes_received);
            }
        }
    } while (0);

    *response_data_size = bytes_received;

    esp_err_t err = esp_http_client_cleanup(*client);
    if (err != ESP_OK) {
        log_printf(TAG, LOG_LEVEL_INFO, "Error cleaning up  http client connection");
        retval = err;
    }
    log_printf(TAG, LOG_LEVEL_INFO, "Cleaned up http client after request");

    // Return 0 if successful, the error code or -1 if anything else
    return retval;
}
