#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/*
 * Max number of characters allowed for each parameter in the json payload (does not include null-term)
 * (uid should never be > 24 + a null term but pad it out a little)
 */
#define MAX_LENGTH_SPOT_NAME_PARAM (49)
#define MAX_LENGTH_SPOT_UID_PARAM (30)
#define MAX_LENGTH_SPOT_LAT_PARAM (20)
#define MAX_LENGTH_SPOT_LON_PARAM (20)
#define MAX_LENGTH_TZ_STR_PARAM (64)
#define MAX_LENGTH_TZ_DISPLAY_NAME_PARAM (64)
#define MAX_LENGTH_OPERATING_MODE_PARAM (64)
#define MAX_LENGTH_CUSTOM_SCREEN_URL_PARAM (256)
#define MAX_LENGTH_CUSTOM_UPDATE_INTERVAL_SECS_PARAM (7)  // Allows at least up to 3 days plus a null term
#define MAX_LENGTH_ACTIVE_CHART_PARAM (10)

void http_server_start();
void http_server_stop();

#endif
