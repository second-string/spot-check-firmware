#pragma once

#include <sys/time.h>
#include <time.h>

void sntp_time_init();
void sntp_time_start();
bool sntp_time_is_synced();
void sntp_time_status_str(char *out_str);
void sntp_time_get_local_time(struct tm *now_local_out);
void sntp_time_get_time_str(struct tm *now_local, char *time_string, char *date_string);
