#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "memfault/panics/assert.h"

#include "constants.h"
#include "http_client.h"
#include "wifi.h"

// Must included below constants.h where we overwite the define of LOG_LOCAL_LEVEL
#include "log.h"

#define TAG SC_TAG_HTTP_CLIENT

#define MAX_QUERY_PARAM_LENGTH 15
#define MAX_READ_BUFFER_SIZE 1024

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");

static SemaphoreHandle_t request_lock;

/* Technically unnecessary, should be stubbed out for non-debug build */
esp_err_t http_event_handler(esp_http_client_event_t *event) {
    switch (event->event_id) {
        case HTTP_EVENT_ERROR:
            log_printf(LOG_LEVEL_DEBUG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            log_printf(LOG_LEVEL_DEBUG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            log_printf(LOG_LEVEL_DEBUG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            log_printf(LOG_LEVEL_DEBUG,
                       "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
                       event->header_key,
                       event->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            log_printf(LOG_LEVEL_DEBUG, "HTTP_EVENT_ON_DATA, len=%d", event->data_len);
            break;
        case HTTP_EVENT_ON_FINISH:
            log_printf(LOG_LEVEL_DEBUG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            log_printf(LOG_LEVEL_DEBUG, "HTTP_EVENT_DISCONNECTED");
            break;
        case HTTP_EVENT_REDIRECT:
            log_printf(LOG_LEVEL_DEBUG, "HTTP_EVENT_REDIRECT");
            break;
    }
    return ESP_OK;
}

void http_client_init() {
    request_lock = xSemaphoreCreateMutex();
    MEMFAULT_ASSERT(request_lock);
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
        if (strcmp(endpoint, "conditions") == 0 || strcmp(endpoint, "screen_update") == 0 ||
            strcmp(endpoint, "test_swell_chart.raw") == 0 || strcmp(endpoint, "test_tide_chart.raw") == 0 ||
            strcmp(endpoint, "swell_chart") == 0 || strcmp(endpoint, "tides_chart") == 0) {
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
    log_printf(LOG_LEVEL_DEBUG, "Built request URL: %s", url_buf);
    log_printf(LOG_LEVEL_DEBUG, "Built %u request query params:", req.num_params);
    for (uint8_t i = 0; i < num_params; i++) {
        log_printf(LOG_LEVEL_DEBUG, "Param %u - %s: %s", i, req.params[i].key, req.params[i].value);
    }

    return req;
}

/*
 * NOTE: only performs the HTTP request. Does not read out response data, does not clean up client (unless there was a
 * failure). Caller responsible for calling http_client_read_response after this function to read data into buffer and
 * close client, OR manually reading data and closing in the case of maniupulating large responses as they're chunked
 * in.
 */
bool http_client_perform_request(request *request_obj, esp_http_client_handle_t *client) {
    MEMFAULT_ASSERT(client);
    MEMFAULT_ASSERT(request_obj);

    if (!wifi_is_connected_to_network()) {
        log_printf(LOG_LEVEL_INFO, "Attempted to make GET request, not connected to any wifi network yet so bailing");
        return false;
    }

    // assume we won't have that many query params. Could calc this too
    char req_url[strlen(request_obj->url) + 80];
    strcpy(req_url, request_obj->url);

    if (request_obj->num_params > 0) {
        log_printf(LOG_LEVEL_DEBUG, "Adding %d query params to URL", request_obj->num_params);
        strcat(req_url, "?");
        for (int i = 0; i < request_obj->num_params; i++) {
            query_param param = request_obj->params[i];
            strcat(req_url, param.key);
            strcat(req_url, "=");
            strcat(req_url, param.value);
            strcat(req_url, "&");
        }
    }

    // Note: using port field means you have to use host and path options instead of URL - it generates the URL
    // internally based on these and setting the url field manually overwrites all that
    esp_http_client_config_t http_config = {
        .url            = req_url,
        .event_handler  = http_event_handler,
        .buffer_size    = MAX_READ_BUFFER_SIZE,
        .cert_pem       = (char *)&server_cert_pem_start,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    bool req_start_success = true;
    do {
        BaseType_t lock_success = xSemaphoreTake(request_lock, pdMS_TO_TICKS(5000));
        if (lock_success == pdFALSE) {
            req_start_success = false;
            log_printf(LOG_LEVEL_ERROR,
                       "Failed to take http req lock in timeout, returning failure for the request status");
            break;
        }

        log_printf(LOG_LEVEL_INFO,
                   "Initing http client for GET request with url '%s'...",
                   http_config.url,
                   http_config.port);
        *client = esp_http_client_init(&http_config);
        if (!(*client)) {
            log_printf(LOG_LEVEL_INFO, "Error initing http client, returning without sending request");
            req_start_success = false;
            break;
        }

        ESP_ERROR_CHECK(esp_http_client_set_method(*client, HTTP_METHOD_GET));
        ESP_ERROR_CHECK(esp_http_client_set_header(*client, "Content-Type", "text/html"));

        esp_err_t err = esp_http_client_open(*client, 0);
        // Opens and sends the request since we have no data
        if (err != ESP_OK) {
            log_printf(LOG_LEVEL_ERROR, "Error opening http client, error: %s", esp_err_to_name(err));

            // In looking at internals of esp_http_client_open, as long as we're not using client in async mode, this
            // error is 1 to 1 with the socket < 0 log line from a failed call to esp_transport_connect. Internally all
            // it does is call the transport close function of the tcp/ssl transport struct internal to the client
            // handle. There are no negative repercussions from doing this extra times even when this is a normal
            // cleanup scenario. Hopefully this is all that's required to get rid of the situation where we get 'stuck'
            // with no open sockets, aka the tcp/ssl transport can't open a connection. We call close instead of
            // cleanup, and before the cleanup call, because cleanup does a ton of freeing of all of the internal data
            // of the client handle so nothing should be accessed after that call.
            if (err == ESP_ERR_HTTP_CONNECT) {
                esp_http_client_close(*client);
            }

            // clean up and return no space allocated
            err = esp_http_client_cleanup(*client);
            if (err != ESP_OK) {
                log_printf(LOG_LEVEL_INFO, "Error cleaning up  http client connection");
            }

            req_start_success = false;
            break;
        }
    } while (0);

    // Always give back no matter what happened with the req
    xSemaphoreGive(request_lock);

    return req_start_success;
}

// TODO :: refactor the perform functions to be one func with arg for post or get. Almost identical now, just have
// to resolve the small differences in implementation of the URL build w/ query params and setting client values.
// Will need to include a call to esp_http_client_write for the post case
// Update: I think the differences have been eliminated in request building logic. Just need to bundle into one func
int http_client_perform_post(request                  *request_obj,
                             char                     *post_data,
                             size_t                    post_data_size,
                             esp_http_client_handle_t *client) {
    if (!wifi_is_connected_to_network()) {
        log_printf(LOG_LEVEL_INFO, "Attempted to make POST request, not connected to any wifi network yet so bailing");
        return 0;
    }

    esp_http_client_config_t http_config = {
        .url            = request_obj->url,
        .event_handler  = http_event_handler,
        .buffer_size    = MAX_READ_BUFFER_SIZE,
        .cert_pem       = (char *)&server_cert_pem_start,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
    };

    bool req_start_success = true;
    do {
        BaseType_t lock_success = xSemaphoreTake(request_lock, pdMS_TO_TICKS(5000));
        if (lock_success == pdFALSE) {
            req_start_success = false;
            log_printf(LOG_LEVEL_ERROR,
                       "Failed to take http req lock in timeout, returning failure for the request status");
            break;
        }

        log_printf(LOG_LEVEL_INFO, "Initing http client for post request...");
        *client = esp_http_client_init(&http_config);
        if (!(*client)) {
            req_start_success = false;
            log_printf(LOG_LEVEL_INFO, "Error initing http client, returning without sending request");
            break;
        }

        // TODO :: these shouldn't assert, fail gracefully
        ESP_ERROR_CHECK(esp_http_client_set_method(*client, HTTP_METHOD_POST));
        ESP_ERROR_CHECK(esp_http_client_set_header(*client, "Content-Type", "application/json"));
        ESP_ERROR_CHECK(esp_http_client_set_post_field(*client, post_data, post_data_size));

        log_printf(LOG_LEVEL_INFO, "Performing POST to %s with data size %u", request_obj->url, post_data_size);
        esp_err_t err = esp_http_client_open(*client, post_data_size);
        if (err != ESP_OK) {
            log_printf(LOG_LEVEL_ERROR, "Error performing POST, error: %s", esp_err_to_name(err));

            // See explanation comment for extra close call here in http_client_perform_request
            if (err == ESP_ERR_HTTP_CONNECT) {
                esp_http_client_close(*client);
            }

            // always clean up on failure
            err = esp_http_client_cleanup(*client);
            if (err != ESP_OK) {
                log_printf(LOG_LEVEL_INFO, "Error cleaning up  http client connection");
            }

            req_start_success = false;
            break;
        } else {
            int write_err = esp_http_client_write(*client, post_data, post_data_size);
            if (write_err < 0) {
                log_printf(LOG_LEVEL_ERROR, "Error performing POST in call to esp_http_client_write");
                req_start_success = false;
                break;
            }

            uint16_t status   = esp_http_client_get_status_code(*client);
            req_start_success = status <= 200 || status > 299;
        }
    } while (0);

    // Always give back no matter what happened with the req
    xSemaphoreGive(request_lock);

    return req_start_success;
}

/*
 * Check headers and status code to make sure request was successful. Should only be used internally by http request
 * functions before they read out data in different manners.
 * Returns success, content length returned through last arg.
 */
static bool http_client_check_response(esp_http_client_handle_t *client, int *content_length) {
    MEMFAULT_ASSERT(client);
    MEMFAULT_ASSERT(content_length);

    bool success = false;

    // Kicks off and blocks until all headers downloaded. Can get remainder of headers (like status_code) from their
    // helper functions, content_length is returned directly
    *content_length = esp_http_client_fetch_headers(*client);

    // Check status to make sure we have actual good data to read out
    int status = esp_http_client_get_status_code(*client);
    if (status >= 200 && status <= 299) {
        if (*content_length < 0) {
            log_printf(LOG_LEVEL_INFO,
                       "Status code successful (%d), but error fetching headers with negative content-length, bailing",
                       status);
        } else if (content_length == 0) {
            log_printf(LOG_LEVEL_ERROR,
                       "Status code successful (%d), but content length of zero after fetching headers, bailing");
        } else {
            success = true;
            log_printf(LOG_LEVEL_INFO, "Request success! Status=%d, Content-length=%d", status, *content_length);
        }
    } else {
        log_printf(LOG_LEVEL_INFO,
                   "Request failed: status=%d, "
                   "Content-length=%d",
                   status,
                   *content_length);
    }

    return success;
}

/*
 * Read response from http requeste into caller-supplied buffer. Caller responsible for freeing malloced buffer
 * saved in response_data pointer if return value > 0. Request must have been sent through client using
 * http_client_perform_request.
 * Returns -1 for error, otherwise number of bytes malloced.
 */
esp_err_t http_client_read_response_to_buffer(esp_http_client_handle_t *client,
                                              char                    **response_data,
                                              size_t                   *response_data_size) {
    MEMFAULT_ASSERT(client);
    MEMFAULT_ASSERT(response_data);
    MEMFAULT_ASSERT(response_data_size);
    esp_err_t err = ESP_FAIL;

    size_t bytes_received = 0;
    bool   success        = false;
    do {
        int content_length = 0;
        success            = http_client_check_response(client, &content_length);
        if (!success || content_length == 0) {
            break;
        }

        if (content_length < MAX_READ_BUFFER_SIZE) {
            *response_data = malloc(content_length + 1);
            if (!response_data) {
                log_printf(LOG_LEVEL_ERROR, "Malloc of %u bytes failed for http response!", content_length + 1);
                break;
            }

            int length_received = esp_http_client_read(*client, *response_data, content_length);
            if (length_received < 0) {
                // Don't bother making the caller free anything since we have nothing to give them. Free here and
                // return -1 for error
                free(*response_data);
                log_printf(LOG_LEVEL_ERROR, "Error reading response after successful http client request");
                break;
            } else {
                (*response_data)[length_received] = '\0';
                bytes_received                    = length_received + 1;
                err                               = ESP_OK;
                log_printf(LOG_LEVEL_DEBUG, "Rcvd %zu bytes of response data: %s", bytes_received, *response_data);
            }
        } else {
            log_printf(
                LOG_LEVEL_ERROR,
                "Content length received in response (%d) larger than max read buffer size of %u, aborting request",
                content_length,
                MAX_READ_BUFFER_SIZE);
            break;
        }
    } while (0);

    esp_err_t cleanup_err = esp_http_client_cleanup(*client);
    if (cleanup_err != ESP_OK) {
        err = cleanup_err;
        log_printf(LOG_LEVEL_ERROR,
                   "Call to esp_http_client_cleanup after reading response to buffer failed with err: %s. Returning "
                   "err to caller",
                   esp_err_to_name(cleanup_err));
    }

    *response_data_size = bytes_received;
    return err;
}

/*
 * Read response from http request in chunks into flash partition. Request must have been sent through client using
 * http_client_perform_request. Caller must erase desired location in flash first.
 * Returns -1 for error, otherwise number of bytes saved to flash.
 */
int http_client_read_response_to_flash(esp_http_client_handle_t *client,
                                       esp_partition_t          *partition,
                                       uint32_t                  offset_into_partition) {
    MEMFAULT_ASSERT(client);
    MEMFAULT_ASSERT(partition);

    int  content_length = 0;
    bool success        = http_client_check_response(client, &content_length);
    if (!success) {
        return -1;
    } else if (content_length == 0) {
        return 0;
    }

    size_t bytes_received = 0;
    log_printf(LOG_LEVEL_INFO,
               "Reading %u payload bytes into flash in chunks of size %u",
               content_length,
               MAX_READ_BUFFER_SIZE);

    uint32_t moving_screen_img_addr = offset_into_partition;
    int      length_received        = 0;
    uint8_t *response_data          = malloc(MAX_READ_BUFFER_SIZE);
    if (!response_data) {
        log_printf(LOG_LEVEL_ERROR, "Malloc of %u bytes failed for http response!", content_length + 1);
        return -1;
    }
    do {
        // Pull in chunk and immediately write to flash
        length_received = esp_http_client_read(*client, (char *)response_data, MAX_READ_BUFFER_SIZE);
        if (length_received > 0) {
            if (moving_screen_img_addr + length_received > partition->size) {
                log_printf(LOG_LEVEL_ERROR,
                           "Attempting to write 0x%02X bytes to partition at offset 0x%02X which would "
                           "overflow the boundary of 0x%02X bytes, aborting",
                           length_received,
                           moving_screen_img_addr,
                           partition->size);
                break;
            }
            esp_partition_write(partition, moving_screen_img_addr, response_data, length_received);
            log_printf(LOG_LEVEL_DEBUG,
                       "Wrote %d bytes to screen image partition at offset %d",
                       length_received,
                       moving_screen_img_addr);
            moving_screen_img_addr += length_received;
            bytes_received += length_received;
        }
    } while (length_received > 0);

    if (response_data) {
        free(response_data);
    }

    if (length_received < 0) {
        // NVS has already been marked as invalid at time of flash erase, so just return error
        log_printf(LOG_LEVEL_ERROR, "Error reading response after successful http client request");
        return -1;
    } else {
        log_printf(LOG_LEVEL_DEBUG, "Rcvd %zu bytes total of response data and saved to flash", bytes_received);
    }

    esp_err_t cleanup_err = esp_http_client_cleanup(*client);
    if (cleanup_err != ESP_OK) {
        log_printf(LOG_LEVEL_ERROR,
                   "Call to esp_http_client_cleanup after reading response to flash failed with err: %s. Malloced buff "
                   "already freed so not altering bytes_received returned to caller",
                   esp_err_to_name(cleanup_err));
    }

    return bytes_received;
}

/*
 * Perform a test query to make sure we actually have an active internet connection. NOTE: blocking, so make sure
 * whatever is calling can wait
 */
bool http_client_check_internet() {
    char   url[80];
    char  *res           = NULL;
    size_t bytes_alloced = 0;

    request req = http_client_build_request((char *)"health", NULL, url, NULL, 0);

    esp_http_client_handle_t client;
    bool                     success = http_client_perform_request(&req, &client);
    if (success) {
        esp_err_t http_err = http_client_read_response_to_buffer(&client, &res, &bytes_alloced);
        if (http_err == ESP_OK && res && bytes_alloced > 0) {
            // Don't care about response, just want to check network connection
            log_printf(LOG_LEVEL_DEBUG, "http client API healthcheck successful");
            free(res);
            return true;
        } else {
            log_printf(LOG_LEVEL_DEBUG, "http client API healthcheck failed at http_client_read_response_to_buffer");
        }
    } else {
        log_printf(LOG_LEVEL_DEBUG, "http client API healthcheck failed at http_client_perform_request");
    }

    return false;
}
