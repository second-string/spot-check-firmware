#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/* Max number of characters allowed for each parameter in the json payload (does not include null-term) */
#define MAX_LENGTH_NUMBER_OF_DAYS_PARAM 2
#define MAX_LENGTH_SPOT_NAME_PARAM 49
#define MAX_LENGTH_FORECAST_TYPE_PARAM 24

#define MAX_NUM_FORECAST_TYPES 5
// number_of_days stored as string for bundling it into a queryparam
// later w/o having to alloc and pass around a new buff. Easier to go
// str -> int vs. other direction
char *number_of_days;
char *spot_name;
char *forecast_types[MAX_NUM_FORECAST_TYPES];

void http_server_start();

#endif
