#pragma once
#include "i2c.h"

BaseType_t cli_command_info(char *write_buffer, size_t write_buffer_size, const char *cmd_str);
void       cli_command_register_all();
