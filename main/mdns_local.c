#include "mdns.h"
#include "esp_log.h"

#include "constants.h"
#include "mdns_local.h"

void mdns_init_local() {
    //initialize mDNS service
    ESP_ERROR_CHECK(mdns_init());

    //set hostname
    mdns_hostname_set("myesp32");
    //set default instance
    mdns_instance_name_set("Jhon's ESP32 Thing");

    ESP_LOGI(TAG, "mDNS initialized");
}
