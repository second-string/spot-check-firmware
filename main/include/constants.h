#ifndef CONSTANTS_H
#define CONSTANTS_H

// #define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

// esp freertos port configMINIMAL_STACK_SIZE is 768 base plus optionals compiled in
// (esp-idf/components/freertos/esp_additions/include/freertos/FreeRTOSConfg.h).
// Define our own minimum here (esp freertos port takes stack size in BYTES not WORDS like vanilla, so stupid)
#define SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES (1024)

#define MS_PER_SEC (1000)
#define SECS_PER_MIN (60)
#define MINS_PER_HOUR (60)

typedef enum {
    SC_TAG_BQ24196 = 0x00,
    SC_TAG_CD54HC4094,
    SC_TAG_CLI_CMD,
    SC_TAG_CLI,
    SC_TAG_DISPLAY,
    SC_TAG_PARTITION,
    SC_TAG_GPIO,
    SC_TAG_HTTP_CLIENT,
    SC_TAG_HTTP_SERVER,
    SC_TAG_I2C,
    SC_TAG_JSON,
    SC_TAG_MAIN,
    SC_TAG_MDNS,
    SC_TAG_NVS,
    SC_TAG_OTA,
    SC_TAG_SCHEDULER,
    SC_TAG_SCREEN_IMG_HANDLER,
    SC_TAG_SLEEP_HANDLER,
    SC_TAG_SNTP,
    SC_TAG_TIMER,
    SC_TAG_UART,
    SC_TAG_URL_DECODE,
    SC_TAG_WIFI,
    SC_TAG_SPOT_CHECK,
    SC_TAG_COUNT,
    // Canot go above 32 elements, used as a bitmask in log.c for faster lookup in blacklist
} sc_tag_t;

static const char* const tag_strs[SC_TAG_COUNT] = {
    [SC_TAG_BQ24196]            = "[sc-bq24196]",
    [SC_TAG_CD54HC4094]         = "[sc-cd54hc4094]",
    [SC_TAG_CLI_CMD]            = "[sc-cli-cmd]",
    [SC_TAG_CLI]                = "[sc-cli]",
    [SC_TAG_DISPLAY]            = "[sc-display]",
    [SC_TAG_PARTITION]          = "[sc-partition]",
    [SC_TAG_GPIO]               = "[sc-gpio]",
    [SC_TAG_HTTP_CLIENT]        = "[sc-http-clnt]",
    [SC_TAG_HTTP_SERVER]        = "[sc-http_srvr]",
    [SC_TAG_I2C]                = "[sc-i2c]",
    [SC_TAG_JSON]               = "[sc-json]",
    [SC_TAG_MAIN]               = "[sc-main]",
    [SC_TAG_MDNS]               = "[sc-mdns]",
    [SC_TAG_NVS]                = "[sc-nvs]",
    [SC_TAG_OTA]                = "[sc-ota]",
    [SC_TAG_SCHEDULER]          = "[sc-scheduler]",
    [SC_TAG_SCREEN_IMG_HANDLER] = "[sc-scrn-img-hndlr]",
    [SC_TAG_SLEEP_HANDLER]      = "[sc-sleep-hndlr]",
    [SC_TAG_SNTP]               = "[sc-sntp]",
    [SC_TAG_TIMER]              = "[sc-timer]",
    [SC_TAG_UART]               = "[sc-uart]",
    [SC_TAG_URL_DECODE]         = "[sc-url-decode]",
    [SC_TAG_WIFI]               = "[sc-wifi]",
    [SC_TAG_SPOT_CHECK]         = "[sc-spot-check]",
};

#endif
