#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "nvs.h"

#define URL_BASE "https://spotcheck.brianteam.dev/"

typedef struct {
    char *key;
    char *value;
} query_param;

typedef struct {
    char *       url;
    query_param *params;
    uint8_t      num_params;
} request;

bool          http_client_inited;
volatile bool connected_to_network;

// Expose event handler so OTA task can callback to it
esp_err_t http_event_handler(esp_http_client_event_t *event);
void      http_client_init();
request   http_client_build_request(char *             endpoint,
                                    spot_check_config *config,
                                    char *             url_buf,
                                    query_param *      params,
                                    uint8_t            num_params);
int       http_client_perform_request(request *request_obj, char **read_buffer);

#endif
