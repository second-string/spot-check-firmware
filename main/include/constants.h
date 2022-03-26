#ifndef CONSTANTS_H
#define CONSTANTS_H

// #define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

// esp freertos port configMINIMAL_STACK_SIZE is 768 base plus optionals compiled in
// (esp-idf/components/freertos/esp_additions/include/freertos/FreeRTOSConfg.h).
// Define our own minimum here (esp freertos port takes stack size in BYTES not WORDS like vanilla, so stupid)
#define SPOT_CHECK_MINIMAL_STACK_SIZE_BYTES (1024)

#endif
