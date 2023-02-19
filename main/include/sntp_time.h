#pragma once

#include <sys/time.h>
#include <time.h>

void sntp_time_init();
void sntp_time_start();
void sntp_time_stop();
bool sntp_time_is_synced();
void sntp_time_status_str(char *out_str);
void sntp_time_get_local_time(struct tm *now_local_out);
void sntp_time_get_time_str(struct tm *now_local, char *time_string, char *date_string);
void sntp_set_time(uint32_t epoch_secs);
void sntp_set_tz_str(char *new_tz_str);
