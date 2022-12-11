#include <string.h>

#include "FreeRTOS_CLI.h"
#include "freertos/FreeRTOS.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "bq24196.h"
#include "cd54hc4094.h"
#include "cli_commands.h"
#include "conditions_task.h"
#include "constants.h"
#include "display.h"
#include "flash_partition.h"
#include "http_client.h"
#include "log.h"
#include "nvs.h"
#include "screen_img_handler.h"
#include "sleep_handler.h"
#include "sntp_time.h"

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
    // Don't null check this to support making requests to url base
    BaseType_t  endpoint_len;
    const char *endpoint = FreeRTOS_CLIGetParameter(cmd_str, 1, &endpoint_len);

    if (endpoint_len == 3 && strncmp(endpoint, "img", endpoint_len) == 0) {
        BaseType_t  screen_img_str_len;
        const char *screen_img_str = FreeRTOS_CLIGetParameter(cmd_str, 2, &screen_img_str_len);
        if (screen_img_str == NULL) {
            strcpy(write_buffer, "Error: usage is 'api img <screen_img_t>'");
            return pdFALSE;
        }

        screen_img_t screen_img = SCREEN_IMG_COUNT;
        if (screen_img_str_len == 4 && strcmp(screen_img_str, "tide") == 0) {
            screen_img = SCREEN_IMG_TIDE_CHART;
        } else if (screen_img_str_len == 5 && strcmp(screen_img_str, "swell") == 0) {
            screen_img = SCREEN_IMG_SWELL_CHART;
        } else {
            char msg[80];
            sprintf(msg, "Found no matching screen_img_t enum value for img '%s'", screen_img_str);
            strcpy(write_buffer, msg);
            return pdFALSE;
        }

        screen_img_handler_download_and_save(screen_img);
        memset(write_buffer, 0x0, write_buffer_size);
    } else {
        const char *const endpoints_with_query_params[] = {
            "conditions",
            "screen_update",
            // NOTE: Make sure to update list of endpoints in http_client_build_request if changing this list
        };

        // If entered endpoint is in list to include config query params, include that data in call to
        // http_client_build_request
        bool include_params = false;
        for (uint8_t i = 0; i < sizeof(endpoints_with_query_params) / sizeof(char *); i++) {
            if (strlen(endpoints_with_query_params[i]) == endpoint_len &&
                strncmp(endpoints_with_query_params[i], endpoint, endpoint_len) == 0) {
                include_params = true;
                log_printf(LOG_LEVEL_DEBUG, "Including normal query params in this CLI API request");
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

        request req = http_client_build_request((char *)endpoint, config, url, params, num_params);
        memset(write_buffer, 0x0, write_buffer_size);

        esp_http_client_handle_t client;
        bool                     success = http_client_perform_request(&req, &client);
        if (!success) {
            log_printf(LOG_LEVEL_ERROR, "Error making request, aborting");
            return pdFALSE;
        }
        esp_err_t http_err = http_client_read_response_to_buffer(&client, &res, &bytes_alloced);
        if (http_err == ESP_OK && res && bytes_alloced > 0) {
            // strncpy up to write_buffer_size since response might be big (shouldn't be since only text though)
            strncpy(write_buffer, res, MIN(strlen(res), write_buffer_size));
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

    if (action_len == 4 && strncmp(action, "read", action_len) == 0) {
        if (part_label == NULL) {
            strcpy(write_buffer, "Error: usage is 'read <label>'");
            return pdFALSE;
        }

        const esp_partition_t *part = flash_partition_get_screen_img_partition();
        if (part == NULL) {
            strcpy(write_buffer, "No partition by that name found");
        }

        uint8_t temp[16];
        ESP_ERROR_CHECK(esp_partition_read(part, 0x00, temp, 16));

        log_printf(LOG_LEVEL_INFO, "First 16 bytes of the %s partition:", part_label);
        for (int i = 0; i < sizeof(temp); i++) {
            log_printf(LOG_LEVEL_INFO, "%02X", temp[i]);
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
            (part_label_len == 10 && strcmp(part_label, SCREEN_IMG_PARTITION_LABEL) == 0)) {
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

                    // With the null check on the below esp_partition_check, it shouldn't be possible to get a null
                    // part here but check anyway
                    if (part == NULL) {
                        state = PARTITION_STATE_END;
                    } else {
                        char part_str[100];
                        sprintf(part_str, "%10s, %5d, %5d, 0x%5lX", part->label, part->type, part->subtype, part->size);
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
        strcpy(write_buffer, "Error: usage is '<action> [<screen>] [<x> <y>]'");
        return pdFALSE;
    }

    memset(write_buffer, 0x0, write_buffer_size);
    if (action_len == 5 && strncmp(action, "clear", action_len) == 0) {
        display_full_clear();
    } else if (action_len == 3 && strncmp(action, "img", action_len) == 0) {
        BaseType_t  screen_len;
        const char *screen = FreeRTOS_CLIGetParameter(cmd_str, 2, &screen_len);
        if (screen == NULL) {
            strcpy(write_buffer, "Error: usage is '<action> <img_name> [<x> <y>]'");
            return pdFALSE;
        }

        screen_img_t screen_img = SCREEN_IMG_COUNT;
        if (screen_len == 4 && strncmp(screen, "tide", screen_len) == 0) {
            screen_img = SCREEN_IMG_TIDE_CHART;
        } else if (screen_len == 5 && strncmp(screen, "swell", screen_len) == 0) {
            screen_img = SCREEN_IMG_SWELL_CHART;
        } else {
            char msg[80];
            sprintf(msg, "Found no matching screen_img_t enum value for screen '%s'", screen);
            strcpy(write_buffer, msg);
            return pdFALSE;
        }

        // TODO :: Don't know if we want to expose the ability to render img to coords from screen_img_handler in
        // the future or only encompass the logic there
        uint32_t    x_coord = 0;
        uint32_t    y_coord = 0;
        BaseType_t  x_coord_len;
        BaseType_t  y_coord_len;
        const char *x_coord_str = FreeRTOS_CLIGetParameter(cmd_str, 3, &x_coord_len);
        const char *y_coord_str = FreeRTOS_CLIGetParameter(cmd_str, 4, &y_coord_len);
        if (x_coord_str) {
            char termed_str[4];
            strncpy(termed_str, x_coord_str, x_coord_len);
            termed_str[x_coord_len] = '\0';
            x_coord                 = strtoul(termed_str, NULL, 10);
        }
        if (y_coord_str) {
            char termed_str[4];
            strncpy(termed_str, y_coord_str, y_coord_len);
            termed_str[y_coord_len] = '\0';
            y_coord                 = strtoul(termed_str, NULL, 10);
        }
        (void)x_coord;
        (void)y_coord;

        bool success = screen_img_handler_draw_screen_img(screen_img);
        screen_img_handler_render();
        if (!success) {
            strcpy(write_buffer, "CLI command to render screen_img failed");
        }
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
            sprintf(msg, "%s: %lu", key, val);
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
            sprintf(msg, "SET %s: %lu", key, val);
            strcpy(write_buffer, msg);
        } else {
            strcpy(write_buffer, "Failed to write value to NVS");
        }
    }

    return pdFALSE;
}

static BaseType_t cli_command_conditions(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t  type_len;
    const char *type = FreeRTOS_CLIGetParameter(cmd_str, 1, &type_len);
    if (type == NULL) {
        strcpy(write_buffer, "Error: usage is 'conditions <type>' where type is 'conditions|tide|swell|both'");
        return pdFALSE;
    }

    memset(write_buffer, 0x0, write_buffer_size);
    if (type_len == 4 && strncmp(type, "time", type_len) == 0) {
        conditions_trigger_time_update();
        strcpy(write_buffer, "Triggered time update");
    } else if (type_len == 4 && strncmp(type, "date", type_len) == 0) {
        // No conditions trigger for this since it's handled internally when updating time. Functions are only exposed
        // for debugging here
        screen_img_handler_clear_date();
        screen_img_handler_draw_date();
    } else if (type_len == 10 && strncmp(type, "conditions", type_len) == 0) {
        conditions_trigger_conditions_update();
        strcpy(write_buffer, "Triggered conditions update");
    } else if (type_len == 4 && strncmp(type, "tide", type_len) == 0) {
        conditions_trigger_tide_chart_update();
        strcpy(write_buffer, "Triggered tide chart update");
    } else if (type_len == 5 && strncmp(type, "swell", type_len) == 0) {
        conditions_trigger_swell_chart_update();
        strcpy(write_buffer, "Triggered swell chart update");
    } else if (type_len == 4 && strncmp(type, "both", type_len) == 0) {
        conditions_trigger_both_charts_update();
        strcpy(write_buffer, "Triggered both charts update");
    } else {
        strcpy(write_buffer, "Invalid conditions update type, must be 'time|conditions|tide|swell|both'");
    }

    return pdFALSE;
}

BaseType_t cli_command_sntp(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t  action_len;
    const char *action = FreeRTOS_CLIGetParameter(cmd_str, 1, &action_len);
    if (action == NULL) {
        strcpy(write_buffer, "Error: usage is 'sntp <action>' where action is 'sync|status'");
        return pdFALSE;
    }

    if (action_len == 4 && strncmp(action, "sync", action_len) == 0) {
        sntp_time_start();
        strcpy(write_buffer, "OK");
    } else if (action_len == 6 && strncmp(action, "status", action_len) == 0) {
        char status_str[12];
        sntp_time_status_str(status_str);
        strcpy(write_buffer, status_str);
    } else {
        strcpy(write_buffer, "Unknown sntp command");
    }

    return pdFALSE;
}

BaseType_t cli_command_log(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t  action_len;
    const char *action = FreeRTOS_CLIGetParameter(cmd_str, 1, &action_len);
    if (action == NULL) {
        strcpy(write_buffer, "Error: usage is 'log <action> <arg>' where action is 'level'");
        return pdFALSE;
    }

    BaseType_t  arg_len;
    const char *arg = FreeRTOS_CLIGetParameter(cmd_str, 2, &arg_len);
    if (arg == NULL) {
        strcpy(write_buffer, "Error: usage is 'log <arg> <arg>' where arg is 'level'");
        return pdFALSE;
    }

    memset(write_buffer, 0x0, write_buffer_size);
    if (action_len == 5 && strncmp(action, "level", action_len) == 0) {
        if (arg == NULL) {
            strcpy(write_buffer, "Error: usage is 'log level <level>' where level is 'err|warn|info|dbg'");
            return pdFALSE;
        }

        log_level_t new_level;
        if (strncmp(arg, "err", 3) == 0) {
            new_level = LOG_LEVEL_ERROR;
        } else if (strncmp(arg, "warn", 4) == 0) {
            new_level = LOG_LEVEL_WARN;
        } else if (strncmp(arg, "info", 4) == 0) {
            new_level = LOG_LEVEL_INFO;
        } else if (strncmp(arg, "dbg", 3) == 0) {
            new_level = LOG_LEVEL_DEBUG;
        } else {
            sprintf(write_buffer, "Invalid log level '%s', choices are 'err|warn|info|dbg'", arg);
            return pdFALSE;
        }

        log_set_max_log_level(new_level);
    } else {
        strcpy(write_buffer, "Unknown log command");
    }

    return pdFALSE;
}

BaseType_t cli_command_sleep(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t  action_len;
    const char *action = FreeRTOS_CLIGetParameter(cmd_str, 1, &action_len);
    if (action == NULL) {
        strcpy(write_buffer, "Error: usage is 'sleep <action>' where action is 'idle' or 'busy'");
        return pdFALSE;
    }

    memset(write_buffer, 0x0, write_buffer_size);
    if (action_len == 4 && strncmp(action, "busy", action_len) == 0) {
        sleep_handler_set_busy(SYSTEM_IDLE_CLI_BIT);
    } else if (action_len == 4 && strncmp(action, "idle", action_len) == 0) {
        sleep_handler_set_idle(SYSTEM_IDLE_CLI_BIT);
    } else {
        strcpy(write_buffer, "Unknown sleep command");
    }

    return pdFALSE;
}

BaseType_t cli_command_event(char *write_buffer, size_t write_buffer_size, const char *cmd_str) {
    BaseType_t  action_len;
    const char *action = FreeRTOS_CLIGetParameter(cmd_str, 1, &action_len);
    if (action == NULL) {
        strcpy(write_buffer, "Error: usage is 'event <action>' where action is 'sta_discon'");
        return pdFALSE;
    }

    memset(write_buffer, 0x0, write_buffer_size);
    if (action_len == 10 && strncmp(action, "sta_discon", action_len) == 0) {
        esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL, 0, pdMS_TO_TICKS(100));
    } else {
        strcpy(write_buffer, "Unknown event command");
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
        .pcCommand = "bq",
        .pcHelpString =
            "bq:\n\twritereg <reg hex>\n\treadreg <reg hex>\n\tdwdg: disable watchdog\n\tdchg: disable charging",
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
        .pcCommand = "api",
        .pcHelpString =
            "api:\n\timg <tide|swell>: download and save image to flash\n\t<endpoint>: send request "
            "to "
            "API endpoint with base URL set in menuconfig",
        .pxCommandInterpreter        = cli_command_api,
        .cExpectedNumberOfParameters = -1,
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
        .pcCommand = "display",
        .pcHelpString =
            "display:\n\tclear: clear full display\n\timg <tide|swell> [<x> <y>]: render an image "
            "currently in flash at the specified coordinates",
        .pxCommandInterpreter        = cli_command_display,
        .cExpectedNumberOfParameters = -1,
    };

    static const CLI_Command_Definition_t nvs_cmd = {
        .pcCommand = "nvs",
        .pcHelpString =
            "nvs:\n\tget <key>: get the uint32 value stored for the key\n\tset <key> <number>: set a uint32 value "
            "in "
            "NVS for a "
            "given key",
        .pxCommandInterpreter        = cli_command_nvs,
        .cExpectedNumberOfParameters = -1,
    };

    static const CLI_Command_Definition_t conditions_cmd = {
        .pcCommand = "conditions",
        .pcHelpString =
            "conditions <time|conditions|tide|swell>: Trigger an update of one of the conditions as if triggered "
            "by "
            "normal expiration",
        .pxCommandInterpreter        = cli_command_conditions,
        .cExpectedNumberOfParameters = 1,
    };

    static const CLI_Command_Definition_t sntp_cmd = {
        .pcCommand                   = "sntp",
        .pcHelpString                = "sntp:\n\tsync: Force sntp re-sync\n\tstatus: print the sntp current status",
        .pxCommandInterpreter        = cli_command_sntp,
        .cExpectedNumberOfParameters = 1,
    };

    static const CLI_Command_Definition_t log_cmd = {
        .pcCommand                   = "log",
        .pcHelpString                = "log:\n\tlevel: set max log level output to serial",
        .pxCommandInterpreter        = cli_command_log,
        .cExpectedNumberOfParameters = 2,
    };

    static const CLI_Command_Definition_t sleep_cmd = {
        .pcCommand = "sleep",
        .pcHelpString =
            "sleep:\n\tbusy: Set test CLI bit to busy in sleep handler idle event group\n\tidle: Set test CLI bit to "
            "idle in sleep handler idle event group",
        .pxCommandInterpreter        = cli_command_sleep,
        .cExpectedNumberOfParameters = 1,
    };

    static const CLI_Command_Definition_t event_cmd = {
        .pcCommand                   = "event",
        .pcHelpString                = "event <sta_discon>: Post selected event to the default event group",
        .pxCommandInterpreter        = cli_command_event,
        .cExpectedNumberOfParameters = 1,
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
    FreeRTOS_CLIRegisterCommand(&conditions_cmd);
    FreeRTOS_CLIRegisterCommand(&sntp_cmd);
    FreeRTOS_CLIRegisterCommand(&log_cmd);
    FreeRTOS_CLIRegisterCommand(&sleep_cmd);
    FreeRTOS_CLIRegisterCommand(&event_cmd);
}
