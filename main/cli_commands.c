#include <string.h>

#include "FreeRTOS_CLI.h"
#include "freertos/FreeRTOS.h"

#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "bq24196.h"
#include "cli_commands.h"
#include "log.h"

#define TAG "sc-cli-cmd"

typedef enum {
    INFO_STATE_BANNER,
    INFO_STATE_VERSION,
    INFO_STATE_COMPILE_DATE,
} info_state_t;

static const char *banner[] = {
    "   _____             _      _____ _               _",
    "  / ____|           | |    / ____| |             | |",
    " | (___  _ __   ___ | |_  | |    | |__   ___  ___| | __",
    "  \\___ \\| '_ \\ / _ \\| __| | |    | '_ \\ / _ \\/ __| |//",
    "  ____) | |_) | (_) | |_  | |____| | | |  __/ (__|   <",
    " |_____/| .__/ \\___/ \\__|  \\_____|_| |_|\\___|\\___|_|_\\",
    "        | |",
    "        |_|",
    NULL,
};

static BaseType_t cli_command_info(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    static uint8_t      banner_line = 0;
    static info_state_t state       = INFO_STATE_BANNER;

    BaseType_t rval = pdFALSE;
    switch (state) {
        case INFO_STATE_BANNER: {
            if (banner[banner_line] == NULL) {
                state           = INFO_STATE_VERSION;
                write_buffer[0] = '\n';
                write_buffer[1] = 0x00;
                banner_line     = 0;
            } else {
                strcpy(write_buffer, banner[banner_line]);
                banner_line++;
            }

            rval = pdTRUE;
            break;
        }
        case INFO_STATE_VERSION: {
            const esp_partition_t *current_partition_version = esp_ota_get_running_partition();
            esp_app_desc_t         current_image_info_version;
            esp_ota_get_partition_description(current_partition_version, &current_image_info_version);

            char version[41];
            sprintf(version, "Version: %s", current_image_info_version.version);
            strcpy(write_buffer, version);
            state = INFO_STATE_COMPILE_DATE;

            rval = pdTRUE;
            break;
        }
        case INFO_STATE_COMPILE_DATE: {
            const esp_partition_t *current_partition_compile_date = esp_ota_get_running_partition();
            esp_app_desc_t         current_image_info_compile_date;
            esp_ota_get_partition_description(current_partition_compile_date, &current_image_info_compile_date);

            char version[60];
            sprintf(version,
                    "Compiled on %s at %s",
                    current_image_info_compile_date.date,
                    current_image_info_compile_date.time);
            strcpy(write_buffer, version);

            state = INFO_STATE_BANNER;
            rval  = pdFALSE;
            break;
        }
        default:
            assert(false);
    }

    return rval;
}

static BaseType_t cli_command_gpio(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t  action_len;
    const char *action = FreeRTOS_CLIGetParameter(cmd_str, 1, &action_len);
    if (action == NULL) {
        strcpy(write_buffer, "Error: usage is 'gpio <action> <arg>'");
        return pdFALSE;
    }

    BaseType_t  pin_len;
    const char *pin_str = FreeRTOS_CLIGetParameter(cmd_str, 2, &pin_len);
    if (pin_str == NULL) {
        strcpy(write_buffer,
               "gpio:\n\tset <pin>: toggle gpio on\n\tclr <pin> toggle gpio off\n\tget <pin>: get gpio level");
        return pdFALSE;
    }

    uint8_t pin = strtoul(pin_str, NULL, 10);
    if (pin > 36) {
        char msg[50];
        sprintf("gpio %s %s: Pin must be between 0 and 36", action, pin_str);
        strcpy(write_buffer, msg);
        return pdFALSE;
    }

    if (action_len == 3 && strncmp(action, "set", action_len) == 0) {
        gpio_set_level(pin, 1);
        strcpy(write_buffer, "OK");
    } else if (action_len == 3 && strncmp(action, "clr", action_len) == 0) {
        gpio_set_level(pin, 0);
        strcpy(write_buffer, "OK");
    } else if (action_len == 3 && strncmp(action, "get", action_len) == 0) {
        char msg[20];
        sprintf(msg, "IO%u: %u", pin, gpio_get_level(pin));
        strcpy(write_buffer, msg);
    } else {
        strcpy(write_buffer, "Command did not match any available 'gpio' subcommands");
        return pdFALSE;
    }

    return pdFALSE;
}

static BaseType_t cli_command_reset(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    esp_restart();

    while (1) {
    }

    return pdFALSE;
}

static BaseType_t cli_command_bq(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t  action_len;
    const char *action = FreeRTOS_CLIGetParameter(cmd_str, 1, &action_len);
    if (action == NULL) {
        strcpy(write_buffer, "Error: usage is 'bq <action> <arg>'");
        return pdFALSE;
    }

    if (action_len == 8 && strncmp(action, "writereg", action_len) == 0) {
        strcpy(write_buffer, "bq writereg not currently supported");
    } else if (action_len == 7 && strncmp(action, "readreg", action_len) == 0) {
        BaseType_t  reg_len;
        const char *reg_str = FreeRTOS_CLIGetParameter(cmd_str, 2, &reg_len);
        if (reg_str == NULL) {
            strcpy(write_buffer, "Error: usage is 'bq readreg <reg hex>'");
            return pdFALSE;
        }

        bq24196_reg_t reg     = strtoul(reg_str, NULL, 16);
        uint8_t       reg_val = 0xFF;
        switch (reg) {
            case BQ24196_REG_INPUT_SRC_CTRL:
                reg_val = bq24196_read_input_src_ctrl_reg();
                break;
            case BQ24196_REG_CHARGE_TERM:
                reg_val = bq24196_read_charge_term_reg();
                break;
            case BQ24196_REG_STATUS:
                reg_val = bq24196_read_status_reg();
                break;
            case BQ24196_REG_FAULT:
                reg_val = bq24196_read_fault_reg();
                break;
            default: {
                char err_msg[40];
                sprintf(err_msg, "0x%02X is not a valid BQ24196 regsister", reg);
                strcpy(write_buffer, err_msg);
                return pdFALSE;
            }
        }
        char msg[40];
        sprintf(msg, "Successfully read  0x%02X from addr 0x%02X", reg_val, reg);
        strcpy(write_buffer, msg);
    } else if (action_len == 4 && strncmp(action, "dwdg", action_len) == 0) {
        bq24196_disable_watchdog();
        strcpy(write_buffer, "OK");
    } else if (action_len == 4 && strncmp(action, "dchg", action_len) == 0) {
        bq24196_disable_charging();
        strcpy(write_buffer, "OK");
    } else if (action_len == 4 && strncmp(action, "wisc", action_len) == 0) {
        bq24196_write_input_src_ctrl_reg();
        strcpy(write_buffer, "OK");
    } else {
        strcpy(write_buffer, "Command did not match any available 'bq' subcommands");
    }

    return pdFALSE;
}

void cli_command_register_all() {
    static const CLI_Command_Definition_t info_cmd = {
        .pcCommand                   = "info",
        .pcHelpString                = "info: Print info about the firmware",
        .pxCommandInterpreter        = cli_command_info,
        .cExpectedNumberOfParameters = 0,
    };

    static const CLI_Command_Definition_t reset_cmd = {
        .pcCommand                   = "reset",
        .pcHelpString                = "reset: Execute firmware reset",
        .pxCommandInterpreter        = cli_command_reset,
        .cExpectedNumberOfParameters = 0,
    };

    static const CLI_Command_Definition_t bq_cmd = {
        .pcCommand                   = "bq",
        .pcHelpString                = "bq: Perform actions on the BQ24196 IC",
        .pxCommandInterpreter        = cli_command_bq,
        .cExpectedNumberOfParameters = -1,
    };

    static const CLI_Command_Definition_t gpio_cmd = {
        .pcCommand    = "gpio",
        .pcHelpString = "gpio:\n\tset <pin>: toggle gpio on\n\tclr <pin> toggle gpio off\n\tget <pin>: get gpio level",
        .pxCommandInterpreter        = cli_command_gpio,
        .cExpectedNumberOfParameters = 2,
    };

    FreeRTOS_CLIRegisterCommand(&info_cmd);
    FreeRTOS_CLIRegisterCommand(&reset_cmd);
    FreeRTOS_CLIRegisterCommand(&bq_cmd);
    FreeRTOS_CLIRegisterCommand(&gpio_cmd);
}