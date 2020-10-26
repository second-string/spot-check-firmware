#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <esp_http_server.h>

#define MAX_KEY_VALUE_QUERYSTRING_AGS 5

httpd_handle_t http_server_start(void);

#endif
