#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdint.h>
#include "nvs.h"

#define URL_BASE "http://spotcheck.brianteam.dev/"

typedef struct {
    char *key;
    char *value;
} query_param;

typedef struct {
    char *       url;
    query_param *params;
    uint8_t      num_params;
} request;

bool http_client_inited;

void    http_client_init();
request http_client_build_request(char *endpoint, spot_check_config *config, char *url_buf, query_param *params,
                                  uint8_t num_params);
int     http_client_perform_request(request *request_obj, char **read_buffer);

#endif
