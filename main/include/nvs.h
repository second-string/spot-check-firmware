#ifndef NVS_H
#define NVS_H

#include "http_server.h"

// number_of_days stored as string for bundling it into a queryparam
// later w/o having to alloc and pass around a new buff. Easier to go
// str -> int vs. other direction
typedef struct {
    char        *number_of_days;
    char        *spot_name;
    char        *spot_uid;
    char        *spot_lat;
    char        *spot_lon;
    char        *forecast_types[MAX_NUM_FORECAST_TYPES];
    unsigned int forecast_type_count;
} spot_check_config;

void               nvs_init();
bool               nvs_get_uint32(char *key, uint32_t *val);
bool               nvs_set_uint32(char *key, uint32_t val);
void               nvs_save_config(spot_check_config *config);
esp_err_t          nvs_full_erase();
spot_check_config *nvs_get_config();
char              *get_next_forecast_type(char **types);

// When allocing a new config struct, on the stack, zero out
// all of the indices starting with the supplied index so our logic
// in nvs_save_config doesn't try to deref a random ptr from
// the default inited memory
void nvs_zero_forecast_types(int starting_from, char **forecast_types);

#endif
