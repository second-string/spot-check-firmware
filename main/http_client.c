#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "memfault/metrics/metrics.h"
#include "memfault/panics/assert.h"

#include "constants.h"
#include "http_client.h"
#include "spot_check.h"
#include "wifi.h"

// Must included below constants.h where we overwite the define of LOG_LOCAL_LEVEL
#include "log.h"

#define TAG SC_TAG_HTTP_CLIENT

#define MAX_QUERY_PARAM_LENGTH 15
#define MAX_READ_BUFFER_SIZE 1024

static SemaphoreHandle_t request_lock;
static uint16_t          failed_http_perform_reqs;
static uint16_t          failed_http_perform_posts;

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

    failed_http_perform_reqs  = 0;
    failed_http_perform_posts = 0;
}

/*
 * Caller passes in endpoint (tides/swell) the values for the 2 query params, a pointer to a block of already-allocated
 * memory for the base url + endpoint, and a pointer to a block of already-allocated memory to hold the query params
 * structs
 */
http_request_t http_client_build_get_request(char              *endpoint,
                                             spot_check_config *config,
                                             char              *url_buf,
                                             query_param       *params,
                                             uint8_t            num_params) {
    http_request_t req = {
        .req_type = HTTP_REQ_TYPE_GET,
    };

    if (params && num_params > 0) {
        query_param temp_params[num_params];
        if (strcmp(endpoint, "conditions") == 0 || strcmp(endpoint, "screen_update") == 0 ||
            strcmp(endpoint, "swell_chart") == 0 || strcmp(endpoint, "tides_chart") == 0) {
            MEMFAULT_ASSERT(num_params == 4);
            temp_params[0] = (query_param){.key = "device_id", .value = spot_check_get_serial()};
            temp_params[1] = (query_param){.key = "lat", .value = config->spot_lat};
            temp_params[2] = (query_param){.key = "lon", .value = config->spot_lon};
            temp_params[3] = (query_param){.key = "spot_id", .value = config->spot_uid};
        } else if (strcmp(endpoint, "tides") == 0 || strcmp(endpoint, "swell") == 0) {
            // backwards compat
            temp_params[1] = (query_param){.key = "spot_id", .value = config->spot_uid};
        }

        memcpy(params, temp_params, sizeof(temp_params));
        req.get_args.num_params = sizeof(temp_params) / sizeof(query_param);
        req.get_args.params     = params;
    }

    strcpy(url_buf, URL_BASE);
    if (endpoint) {
        strcat(url_buf, endpoint);
    }
    req.url = url_buf;
    log_printf(LOG_LEVEL_DEBUG, "Built request URL: %s", url_buf);
    log_printf(LOG_LEVEL_DEBUG, "Built %u request query params:", req.get_args.num_params);
    for (uint8_t i = 0; i < num_params; i++) {
        log_printf(LOG_LEVEL_DEBUG, "Param %u - %s: %s", i, req.get_args.params[i].key, req.get_args.params[i].value);
    }

    return req;
}

/*
 * Caller passes in endpoint, a pointer to a block of already-allocated memory for the base url + endpoint, and the
 * already-built post data string (only used as json for now).
 * Bit of a redundant function in the current request obj format but used for parity with GET flow.
 */
http_request_t http_client_build_post_request(char *endpoint, char *url_buf, char *post_data, size_t post_data_size) {
    // Build our url with memcpy (no null term) then strcpy (null term)
    size_t base_len = strlen(URL_BASE);
    memcpy(url_buf, URL_BASE, base_len);
    strcpy(url_buf + base_len, endpoint);
    http_post_args_t post_args = {
        .post_data      = post_data,
        .post_data_size = post_data_size,
    };

    http_request_t req = {
        .req_type  = HTTP_REQ_TYPE_POST,
        .url       = url_buf,
        .post_args = post_args,
    };

    return req;
}

/*
 * Request-type-agnostic function for initiating the actual http contact with server.
 * NOTE: only performs the HTTP request (and in the case of a POST, writes the post data to the socket). Does not read
 * out response data, does not clean up client (unless there was a failure). Caller responsible for calling
 * http_client_read_response after this function to read data into buffer and close client, OR manually reading data
 * and closing in the case of maniupulating large responses as they're chunked in.
 */
bool http_client_perform(http_request_t *request_obj, esp_http_client_handle_t *client) {
    MEMFAULT_ASSERT(client);
    MEMFAULT_ASSERT(request_obj);

    // Build out most shared variables here to minimize number of checks to req_type later in function.
    // TODO :: really all of this stuff should be added to the request_obj_t struct in the build_request functions, so
    // this func can be fully (or at least moreso) request-type-agnostic and just blindly populate stuff
    char                     req_type_str[5];
    uint16_t                *failed_error_ptr = NULL;
    esp_http_client_method_t method;
    char                     content_type[17];
    MemfaultMetricId         memfault_key;
    switch (request_obj->req_type) {
        case HTTP_REQ_TYPE_GET:
            strcpy(req_type_str, "GET");
            strcpy(content_type, "text/html");
            memfault_key     = MEMFAULT_METRICS_KEY(failed_http_reqs);
            failed_error_ptr = &failed_http_perform_reqs;
            method           = HTTP_METHOD_GET;
            break;
        case HTTP_REQ_TYPE_POST:
            strcpy(req_type_str, "POST");
            strcpy(content_type, "application/json");
            memfault_key     = MEMFAULT_METRICS_KEY(failed_http_posts);
            failed_error_ptr = &failed_http_perform_posts;
            method           = HTTP_METHOD_POST;
            break;
        default:
            MEMFAULT_ASSERT(0);
    }

    if (!wifi_is_connected_to_network()) {
        log_printf(LOG_LEVEL_INFO,
                   "Attempted to make %s request, not connected to any wifi network yet so bailing",
                   req_type_str);
        return false;
    }

    // assume we won't have that many query params. Could calc this too
    char req_url[strlen(request_obj->url) + 256];
    strcpy(req_url, request_obj->url);

    if (request_obj->req_type == HTTP_REQ_TYPE_GET && request_obj->get_args.num_params > 0) {
        log_printf(LOG_LEVEL_DEBUG, "Adding %d query params to URL", request_obj->get_args.num_params);
        strcat(req_url, "?");
        for (int i = 0; i < request_obj->get_args.num_params; i++) {
            query_param param = request_obj->get_args.params[i];
            strcat(req_url, param.key);
            strcat(req_url, "=");
            strcat(req_url, param.value);
            strcat(req_url, "&");
        }
    }

    // Note: using port field means you have to use host and path options instead of URL - it generates the URL
    // internally based on these and setting the url field manually overwrites all that
    esp_http_client_config_t http_config = {
        .url               = req_url,
        .event_handler     = http_event_handler,
        .buffer_size       = MAX_READ_BUFFER_SIZE,
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
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
                   "Initing http client for %s request with url '%s:%d'...",
                   req_type_str,
                   http_config.url,
                   http_config.port);
        *client = esp_http_client_init(&http_config);
        if (!(*client)) {
            log_printf(LOG_LEVEL_INFO, "Error initing http client, returning without sending request");
            (*failed_error_ptr)++;
            req_start_success = false;
            break;
        }

        size_t open_data_size = 0;
        ESP_ERROR_CHECK(esp_http_client_set_method(*client, method));
        ESP_ERROR_CHECK(esp_http_client_set_header(*client, "Content-Type", content_type));
        if (request_obj->req_type == HTTP_REQ_TYPE_POST) {
            ESP_ERROR_CHECK(esp_http_client_set_post_field(*client,
                                                           request_obj->post_args.post_data,
                                                           request_obj->post_args.post_data_size));
            open_data_size = request_obj->post_args.post_data_size;
        }

        esp_err_t err = esp_http_client_open(*client, open_data_size);

        if (err != ESP_OK) {
            log_printf(LOG_LEVEL_ERROR, "Error opening http client, error: %s", esp_err_to_name(err));
            (*failed_error_ptr)++;

            err = esp_http_client_cleanup(*client);
            if (err != ESP_OK) {
                log_printf(LOG_LEVEL_ERROR, "Error cleaning up http client connection after failure to open!!!");
            }

            req_start_success = false;
            break;
        } else if (request_obj->req_type == HTTP_REQ_TYPE_POST) {
            // POSTs have an extra step after opening to actually write the data, but we only want to perform it if the
            // open was successful. GETs are fine with just the open.
            int write_err =
                esp_http_client_write(*client, request_obj->post_args.post_data, request_obj->post_args.post_data_size);
            if (write_err < 0) {
                log_printf(LOG_LEVEL_ERROR, "Error performing POST in call to esp_http_client_write");
                req_start_success = false;
                break;
            }

            // Preemptively check status here even though it's also checked inthe http_client_check-response function
            // called after this
            uint16_t status   = esp_http_client_get_status_code(*client);
            req_start_success = status <= 200 || status > 299;
        }

    } while (0);

    // Always give back no matter what happened with the req
    xSemaphoreGive(request_lock);

    if (!req_start_success) {
        memfault_metrics_heartbeat_add(memfault_key, 1);
    }

    return req_start_success;
}

/*
 * Check headers and status code to make sure request was successful. Should only be used internally by http request
 * functions before they read out data in different manners (not static because cli needs access for debugging).
 * Returns success, content length returned through last arg.
 */
bool http_client_check_response(esp_http_client_handle_t *client, int *content_length) {
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
            log_printf(LOG_LEVEL_WARN,
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
 * Returns ESP_OK on success, ESP_FAIL for failure. Returns malloced data and the size of that data in the two pointer
 * args.
 */
esp_err_t http_client_read_response_to_buffer(esp_http_client_handle_t *client,
                                              char                    **response_data,
                                              size_t                   *response_data_size) {
    MEMFAULT_ASSERT(client);
    MEMFAULT_ASSERT(response_data);
    MEMFAULT_ASSERT(response_data_size);

    esp_err_t err            = ESP_FAIL;
    size_t    bytes_received = 0;
    do {
        int  content_length = 0;
        bool success        = http_client_check_response(client, &content_length);
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
 * Returns ESP_OK on success, ESP_FAIL for failure. Returns total bytes saved to NVS in pointer arg.
 */
esp_err_t http_client_read_response_to_flash(esp_http_client_handle_t *client,
                                             esp_partition_t          *partition,
                                             uint32_t                  offset_into_partition,
                                             size_t                   *bytes_saved_size) {
    MEMFAULT_ASSERT(client);
    MEMFAULT_ASSERT(partition);

    esp_err_t err            = ESP_FAIL;
    size_t    bytes_received = 0;
    do {
        int  content_length = 0;
        bool success        = http_client_check_response(client, &content_length);
        if (!success) {
            break;
        } else if (content_length == 0) {
            // Not an error, but no reason to continue with logic
            err = ESP_OK;
            break;
        }

        log_printf(LOG_LEVEL_INFO,
                   "Reading %u payload bytes into flash in chunks of size %u",
                   content_length,
                   MAX_READ_BUFFER_SIZE);

        uint32_t moving_screen_img_addr = offset_into_partition;
        int      length_received        = 0;
        uint8_t *response_data          = malloc(MAX_READ_BUFFER_SIZE);
        if (!response_data) {
            log_printf(LOG_LEVEL_ERROR, "Malloc of %u bytes failed for http response!", content_length + 1);
            break;
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
            break;
        } else {
            log_printf(LOG_LEVEL_DEBUG, "Rcvd %zu bytes total of response data and saved to flash", bytes_received);
            err = ESP_OK;
        }
    } while (0);

    esp_err_t cleanup_err = esp_http_client_cleanup(*client);
    if (cleanup_err != ESP_OK) {
        err = cleanup_err;
        log_printf(LOG_LEVEL_ERROR,
                   "Call to esp_http_client_cleanup after reading response to flash failed with err: %s. Malloced buff "
                   "already freed so not altering bytes_received returned to caller",
                   esp_err_to_name(cleanup_err));
    }

    *bytes_saved_size = bytes_received;
    return err;
}

/*
 * Perform a test query to make sure we actually have an active internet connection. NOTE: blocking, so make sure
 * whatever is calling can wait
 */
bool http_client_check_internet() {
    char   url[80];
    char  *res           = NULL;
    size_t bytes_alloced = 0;

    http_request_t req = http_client_build_get_request((char *)"health", NULL, url, NULL, 0);

    esp_http_client_handle_t client;
    bool                     success = http_client_perform(&req, &client);
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

void http_client_get_failures(uint16_t *get_failures, uint16_t *post_failures) {
    *get_failures  = failed_http_perform_reqs;
    *post_failures = failed_http_perform_posts;
}
