#pragma once

void sntp_time_init();
void sntp_time_start();
bool sntp_time_is_synced();
void sntp_time_status_str(char *out_str);
