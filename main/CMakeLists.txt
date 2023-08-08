idf_component_register(
    SRCS
        "main.c"
        "url_decode.c"
        "nvs.c"
        "wifi.c"
        "gpio.c"
        "timer.c"
        "mdns_local.c"
        "http_client.c"
        "json.c"
        "http_server.c"
        "ota_task.c"
        "scheduler_task.c"
        "cli_task.c"
        "uart.c"
        "log.c"
        "cli_commands.c"
        "i2c.c"
        "bq24196.c"
        "cd54hc4094.c"
        "display.c"
        "flash_partition.c"
        "screen_img_handler.c"
        "sntp_time.c"
        "sleep_handler.c"
        "spot_check.c"
        "memfault_platform_port.c"
        "memfault_interface.c"
    INCLUDE_DIRS
        "include"
        ${MEMFAULT_FIRMWARE_SDK}/ports/include
)

# These are included in default idf flags but include for any future changes
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror -Wall -Wextra)

# Manually include these as errors because idf defaults to '-Wno-error'ing them and there's no way to remove or override them. Last flag wins in compilation call so these are respected since they're appended last
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=unused-function -Werror=unused-variable -Werror=deprecated-declarations -Werror=unused-but-set-variable)
