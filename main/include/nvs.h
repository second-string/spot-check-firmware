#ifndef NVS_H
#define NVS_H

#include "http_server.h"

// number_of_days stored as string for bundling it into a queryparam
// later w/o having to alloc and pass around a new buff. Easier to go
// str -> int vs. other direction
typedef struct {
    char *number_of_days;
    char *spot_name;
    char *spot_uid;
    char *forecast_types[MAX_NUM_FORECAST_TYPES];
    unsigned int forecast_type_count;
} spot_check_config;


void nvs_init();
void nvs_save_config(spot_check_config *config);
spot_check_config *nvs_get_config();
char *get_next_forecast_type(char **types);

#endif
