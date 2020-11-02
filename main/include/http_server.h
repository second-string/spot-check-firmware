#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/* Max number of characters allowed for each parameter in the json payload (does not include null-term) */
#define MAX_LENGTH_NUMBER_OF_DAYS_PARAM 2
#define MAX_LENGTH_SPOT_NAME_PARAM 49
#define MAX_LENGTH_FORECAST_TYPE_PARAM 24

#define MAX_NUM_FORECAST_TYPES 5

void http_server_start();

#endif
