#include <string.h>

#include "FreeRTOS_CLI.h"
#include "freertos/FreeRTOS.h"

#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spi_flash.h"
#include "esp_system.h"

#include "bq24196.h"
#include "cd54hc4094.h"
#include "cli_commands.h"
#include "constants.h"
#include "display.h"
#include "http_client.h"
#include "log.h"
#include "nvs.h"

#define TAG "sc-cli-cmd"

typedef enum {
    INFO_STATE_BANNER,
    INFO_STATE_VERSION,
    INFO_STATE_COMPILE_DATE,
} info_state_t;

typedef enum {
    PARTITION_STATE_START,
    PARTITION_STATE_LISTING,
    PARTITION_STATE_END,
} partition_state_t;

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

BaseType_t cli_command_info(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
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

        uint8_t temp_reg = strtoul(reg_str, NULL, 16);
        if (temp_reg >= BQ24196_REG_COUNT) {
            char err_msg[40];
            sprintf(err_msg, "0x%02X is not a valid BQ24196 register", temp_reg);
            strcpy(write_buffer, err_msg);
            return pdFALSE;
        }

        bq24196_reg_t reg     = temp_reg;
        uint8_t       reg_val = 0xFF;
        switch (reg) {
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
                bq24196_read_reg(reg, &reg_val);
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
    } else {
        strcpy(write_buffer, "Command did not match any available 'bq' subcommands");
    }

    return pdFALSE;
}

static BaseType_t cli_command_shiftreg(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t  action_len;
    const char *action = FreeRTOS_CLIGetParameter(cmd_str, 1, &action_len);
    if (action == NULL) {
        strcpy(write_buffer, "Error: usage is 'shiftreg <action> <arg>'");
        return pdFALSE;
    }

    if (action_len == 6 && strncmp(action, "output", action_len) == 0) {
        const char *output_val_str = FreeRTOS_CLIGetParameter(cmd_str, 2, &action_len);
        if (output_val_str == NULL) {
            strcpy(write_buffer, "Error: usage is 'shiftreg output <output_byte_val>'");
            return pdFALSE;
        }

        uint8_t output_val = strtoul(output_val_str, NULL, 16);
        cd54hc4094_set_output(output_val);
        char retstr[40];
        sprintf(retstr, "Shift register pins set to 0x%02X", output_val);
        strcpy(write_buffer, retstr);
    }

    return pdFALSE;
}

static BaseType_t cli_command_api(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t  endpoint_len;
    const char *endpoint = FreeRTOS_CLIGetParameter(cmd_str, 1, &endpoint_len);
    if (endpoint == NULL) {
        strcpy(write_buffer, "Error: usage is 'api <endpoint>'");
        return pdFALSE;
    }

    // Make sure to update list of endpoints in http_client_build_request if changing this list
    const char *const endpoints_with_query_params[2] = {
        "conditions",
        "screen_update",
    };

    // If entered endpoint is in list to include config query params, include that data in call to
    // http_client_build_request
    bool include_params = false;
    for (uint8_t i = 0; i < sizeof(endpoints_with_query_params) / sizeof(char *); i++) {
        if (strlen(endpoints_with_query_params[i]) == endpoint_len &&
            strncmp(endpoints_with_query_params[i], endpoint, endpoint_len) == 0) {
            include_params = true;
            break;
        }
    }

    char               url[80];
    char              *res           = NULL;
    size_t             bytes_alloced = 0;
    spot_check_config *config        = NULL;
    query_param        params[3];
    uint8_t            num_params = 0;
    if (include_params) {
        config     = nvs_get_config();
        num_params = 3;
    }
    request                  req = http_client_build_request((char *)endpoint, config, url, params, num_params);
    esp_http_client_handle_t client;
    bool                     success = http_client_perform_request(&req, &client);
    if (!success) {
        log_printf(TAG, LOG_LEVEL_ERROR, "Error making request, aborting");
    } else {
        ESP_ERROR_CHECK(http_client_read_response(&client, &res, &bytes_alloced));
        if (res && bytes_alloced > 0) {
            strcpy(write_buffer, res);
            free(res);
        }
    }
    return pdFALSE;
}

static BaseType_t cli_command_partition(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t               retval = pdFALSE;
    static partition_state_t state  = PARTITION_STATE_START;
    BaseType_t               action_len;
    const char              *action = FreeRTOS_CLIGetParameter(cmd_str, 1, &action_len);
    if (action == NULL) {
        strcpy(write_buffer, "Error: usage is '<action> [<label>]'");
        return pdFALSE;
    }

    BaseType_t  part_label_len;
    const char *part_label = FreeRTOS_CLIGetParameter(cmd_str, 2, &part_label_len);
    // char        temp[100];
    // sprintf(temp, "action: %s - label: %s, label_len: %zu", action, part_label, part_label_len);
    // strcpy(write_buffer, temp);
    // return pdFALSE;

    if (action_len == 4 && strncmp(action, "read", action_len) == 0) {
        if (part_label == NULL) {
            strcpy(write_buffer, "Error: usage is 'read <label>'");
            return pdFALSE;
        }

        const esp_partition_t *part =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, part_label);
        if (part == NULL) {
            strcpy(write_buffer, "No partition by that name found");
        }

        uint8_t temp[16];
        ESP_ERROR_CHECK(esp_partition_read(part, 0x00, temp, 16));

        log_printf(TAG, LOG_LEVEL_INFO, "First 16 bytes of the %s partition:", part_label);
        for (int i = 0; i < sizeof(temp); i++) {
            log_printf(TAG, LOG_LEVEL_INFO, "%02X", temp[i]);
        }

        // Empty out the buffer since we'll print the output of the last command if we don't
        strcpy(write_buffer, "");
    } else if (action_len == 5 && strncmp(action, "erase", action_len) == 0) {
        if (part_label == NULL) {
            strcpy(write_buffer, "Error: usage is 'erase <label>'");
            return pdFALSE;
        }

        const esp_partition_t *part =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, part_label);
        if (part == NULL) {
            strcpy(write_buffer, "No partition by that name found");
        }

        char msg[60];
        if ((part_label_len == 3 && strcmp(part_label, "nvs") == 0) ||
            (part_label_len == 10 && strcmp(part_label, "screen_img") == 0)) {
            esp_partition_erase_range(part, 0x0, part->size);
            sprintf(msg, "Successfully erased '%s' partition", part->label);
            strcpy(write_buffer, msg);
        } else {
            sprintf(msg, "Erasing of '%s' partition not allowed!", part->label);
            strcpy(write_buffer, msg);
        }
    } else if (action_len == 4 && strncmp(action, "list", action_len) == 0) {
        static esp_partition_iterator_t iter = NULL;
        static const esp_partition_t   *part = NULL;
        switch (state) {
            case PARTITION_STATE_START:
                iter = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);

                char part_header[100];
                sprintf(part_header, "%10s, %5s, %5s, %5s", "label", "type", "subtype", "size");
                strcpy(write_buffer, part_header);

                state  = PARTITION_STATE_LISTING;
                retval = pdTRUE;
                break;
            case PARTITION_STATE_LISTING:
                if (iter == NULL) {
                    state = PARTITION_STATE_END;
                } else {
                    part = esp_partition_get(iter);

                    // With the null check on the below esp_partition_check, it shouldn't be possible to get a null part
                    // here but check anyway
                    if (part == NULL) {
                        state = PARTITION_STATE_END;
                    } else {
                        char part_str[100];
                        sprintf(part_str, "%10s, %5d, %5d, 0x%5X", part->label, part->type, part->subtype, part->size);
                        strcpy(write_buffer, part_str);

                        iter   = esp_partition_next(iter);
                        retval = pdTRUE;
                    }
                }
                break;
            case PARTITION_STATE_END:
                esp_partition_iterator_release(iter);
                iter  = NULL;
                part  = NULL;
                state = PARTITION_STATE_START;
                break;
        }
    } else {
        strcpy(write_buffer, "Unknown partition command");
    }

    return retval;
}

static BaseType_t cli_command_display(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t  action_len;
    const char *action = FreeRTOS_CLIGetParameter(cmd_str, 1, &action_len);
    if (action == NULL) {
        strcpy(write_buffer, "Error: usage is '<action> [<x> <y>]'");
        return pdFALSE;
    }

    if (action_len == 5 && strncmp(action, "flash", action_len) == 0) {
        BaseType_t  x_coord_len;
        BaseType_t  y_coord_len;
        const char *x_coord_str = FreeRTOS_CLIGetParameter(cmd_str, 2, &x_coord_len);
        const char *y_coord_str = FreeRTOS_CLIGetParameter(cmd_str, 3, &y_coord_len);

        uint32_t x_coord = 0;
        uint32_t y_coord = 0;
        if (x_coord_str) {
            x_coord = strtoul(x_coord_str, NULL, 10);
        }
        if (y_coord_str) {
            y_coord = strtoul(y_coord_str, NULL, 10);
        }

        spi_flash_mmap_handle_t spi_flash_handle;
        uint32_t                screen_img_len    = 0;
        uint32_t                screen_img_width  = 0;
        uint32_t                screen_img_height = 0;
        bool                    success           = nvs_get_uint32(SCREEN_IMG_SIZE_NVS_KEY, &screen_img_len);
        if (!success) {
            strcpy(write_buffer, "No screen img size value stored in NVS, cannot render screen out of flash");
            return pdFALSE;
        }
        success = nvs_get_uint32(SCREEN_IMG_WIDTH_PX_NVS_KEY, &screen_img_width);
        if (!success) {
            strcpy(write_buffer, "No screen img width value stored in NVS, cannot render screen out of flash");
            return pdFALSE;
        }
        success = nvs_get_uint32(SCREEN_IMG_HEIGHT_PX_NVS_KEY, &screen_img_height);
        if (!success) {
            strcpy(write_buffer, "No screen img height value stored in NVS, cannot render screen out of flash");
            return pdFALSE;
        }

        const esp_partition_t *screen_img_partition =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "screen_img");

        // TODO :: make sure screen_img_len length is less that buffer size (or at least a reasonable number to malloc)
        // mmap handles the large malloc internally, and the call the munmap below frees it
        const uint8_t *mapped_flash = NULL;
        esp_partition_mmap(screen_img_partition,
                           0x0,
                           screen_img_len,
                           SPI_FLASH_MMAP_DATA,
                           (const void **)&mapped_flash,
                           &spi_flash_handle);
        display_render_image((uint8_t *)mapped_flash, screen_img_width, screen_img_height, 1, x_coord, y_coord);

        spi_flash_munmap(spi_flash_handle);

        char msg[80];
        sprintf(msg, "Rendered image from flash at (%u, %u)", x_coord, y_coord);
        strcpy(write_buffer, msg);
    } else {
        strcpy(write_buffer, "Unknown display command");
    }

    return pdFALSE;
}

static BaseType_t cli_command_nvs(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t  action_len;
    const char *action = FreeRTOS_CLIGetParameter(cmd_str, 1, &action_len);
    if (action == NULL) {
        strcpy(write_buffer, "Error: usage is '<action> <key> [<number>]'");
        return pdFALSE;
    }

    BaseType_t  key_len;
    const char *key_str = FreeRTOS_CLIGetParameter(cmd_str, 2, &key_len);
    if (key_str == NULL) {
        strcpy(write_buffer, "Error: usage is '<action> <key> [<number>]'");
        return pdFALSE;
    }

    // Have to copy to new buffer to add a null term, otherwise key_str contains value string with no term between
    char key[key_len + 1];
    strncpy(key, key_str, key_len);

    if (action_len == 3 && strncmp(action, "get", action_len) == 0) {
        uint32_t val     = 0;
        bool     success = nvs_get_uint32(key, &val);
        if (success) {
            char msg[50];
            sprintf(msg, "%s: %u", key, val);
            strcpy(write_buffer, msg);
        } else {
            strcpy(write_buffer, "Failed to get value from NVS");
        }
    } else if (action_len == 3 && strncmp(action, "set", action_len) == 0) {
        BaseType_t  val_len;
        const char *val_str = FreeRTOS_CLIGetParameter(cmd_str, 3, &val_len);
        if (val_str == NULL) {
            strcpy(write_buffer, "Error: usage is 'set <key> <value>'");
            return pdFALSE;
        }

        uint32_t val     = strtoul(val_str, NULL, 10);
        bool     success = nvs_set_uint32(key, val);
        if (success) {
            char msg[50];
            sprintf(msg, "SET %s: %u", key, val);
            strcpy(write_buffer, msg);
        } else {
            strcpy(write_buffer, "Failed to write value to NVS");
        }
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

    static const CLI_Command_Definition_t shiftreg_cmd = {
        .pcCommand = "shiftreg",
        .pcHelpString =
            "shiftreg:\n\toutput: Set the output pins of the shift register to each bit in the value (in hex)",
        .pxCommandInterpreter        = cli_command_shiftreg,
        .cExpectedNumberOfParameters = 2,
    };

    static const CLI_Command_Definition_t api_cmd = {
        .pcCommand            = "api",
        .pcHelpString         = "api:\n\t<endpoint>: send request to API endpoint with base URL set in menuconfig",
        .pxCommandInterpreter = cli_command_api,
        .cExpectedNumberOfParameters = 1,
    };

    static const CLI_Command_Definition_t partition_cmd = {
        .pcCommand = "partition",
        .pcHelpString =
            "partition:\n\tread <label>: read the first 16 bytes of a partition\n\terase <label>: erase a "
            "partition\n\tlist: list the current device partition table",
        .pxCommandInterpreter        = cli_command_partition,
        .cExpectedNumberOfParameters = -1,
    };

    static const CLI_Command_Definition_t display_cmd = {
        .pcCommand    = "display",
        .pcHelpString = "display:\n\tflash <x> <y>: render the image currently in flash as the specified coordinates",
        .pxCommandInterpreter        = cli_command_display,
        .cExpectedNumberOfParameters = -1,
    };

    static const CLI_Command_Definition_t nvs_cmd = {
        .pcCommand = "nvs",
        .pcHelpString =
            "nvs:\n\tget <key>: get the uint32 value stored for the key\n\tset <key> <number>: set a uint32 value in "
            "NVS for a "
            "given key",
        .pxCommandInterpreter        = cli_command_nvs,
        .cExpectedNumberOfParameters = -1,
    };

    FreeRTOS_CLIRegisterCommand(&info_cmd);
    FreeRTOS_CLIRegisterCommand(&reset_cmd);
    FreeRTOS_CLIRegisterCommand(&bq_cmd);
    FreeRTOS_CLIRegisterCommand(&gpio_cmd);
    FreeRTOS_CLIRegisterCommand(&shiftreg_cmd);
    FreeRTOS_CLIRegisterCommand(&api_cmd);
    FreeRTOS_CLIRegisterCommand(&partition_cmd);
    FreeRTOS_CLIRegisterCommand(&display_cmd);
    FreeRTOS_CLIRegisterCommand(&nvs_cmd);
}
