#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_partition.h"
#include "nvs.h"

// Needs trailing slash!
#define URL_BASE "https://spotcheck.brianteam.com/"
// #define URL_BASE "http://192.168.1.76:9080/"

typedef enum {
    HTTP_REQ_TYPE_GET,
    HTTP_REQ_TYPE_POST,

    HTTP_REQ_TYPE_COUNT,
} http_req_type_t;

typedef struct {
    char *key;
    char *value;
} query_param;

typedef struct {
    query_param *params;
    uint8_t      num_params;
} http_get_args_t;

typedef struct {
    char  *post_data;
    size_t post_data_size;
} http_post_args_t;

typedef struct {
    char           *url;
    http_req_type_t req_type;
    union {
        http_get_args_t  get_args;
        http_post_args_t post_args;
    };
} http_request_t;

// Expose event handler so OTA task can callback to it
esp_err_t      http_event_handler(esp_http_client_event_t *event);
void           http_client_init();
http_request_t http_client_build_get_request(char              *endpoint,
                                             spot_check_config *config,
                                             char              *url_buf,
                                             query_param       *params,
                                             uint8_t            num_params);
http_request_t http_client_build_post_request(char *endpoint, char *url_buf, char *post_data, size_t post_data_size);
bool           http_client_perform_with_retries(http_request_t           *request_obj,
                                                uint8_t                   additional_retries,
                                                esp_http_client_handle_t *client,
                                                int                      *content_length);
esp_err_t      http_client_read_response_to_buffer(esp_http_client_handle_t *client,
                                                   int                       content_length,
                                                   char                    **response_data,
                                                   size_t                   *response_data_size);
esp_err_t      http_client_read_response_to_flash(esp_http_client_handle_t *client,
                                                  int                       content_length,
                                                  esp_partition_t          *partition,
                                                  uint32_t                  offset_into_partition,
                                                  size_t                   *bytes_saved_size);
bool           http_client_check_internet();

// This is for debugging with cli, isn't necessary long term
void http_client_get_failures(uint16_t *get_failures, uint16_t *post_failures);
#endif
