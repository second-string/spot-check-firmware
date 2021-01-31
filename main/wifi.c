#include "constants.h"

#include <string.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "wifi.h"

// Local configuration network
#define CONFIG_AP_SSID CONFIG_CONFIGURATION_ACCESS_POINT_SSID

bool wifi_is_provisioning_inited = false;

static esp_event_handler_instance_t provisioning_manager_event_handler;

/*
 * We're responsible for internally holding the pointer to the event
 * handler function that the caller of wifi_init_* passes in
 */
static void *main_event_handler = NULL;

/* Simply sets mode and starts, expects config to be done */
static void wifi_start_sta() {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_start_provisioning(bool force_reprovision) {
    if (main_event_handler == NULL) {
        ESP_LOGE(TAG,
                 "wifi_init() not called before trying to start provisioning or connect to sta, failing irrecoverably");
        return;
    }

    bool already_provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&already_provisioned));
    if (!already_provisioned || force_reprovision) {
        const char *log = force_reprovision ? "Forcing reprovisioning process"
                                            : "No saved provisioning info, starting provisioning process";
        ESP_LOGI(TAG, "%s", log);

        // SSID / device name (softap / ble respectively)
        char *service_name = CONFIG_AP_SSID;

        // 0 is plaintext, 1 uses a secure handshake for key exchange with proof of possession
        wifi_prov_security_t security = WIFI_PROV_SECURITY_0;

        // Only used for WIFI_PROV_SECURITY_1 security level
        const char *proof_of_poss = "abc123";

        // Network password (only used for softap prov)
        const char *service_key = NULL;

        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, proof_of_poss, service_name, service_key));

        // Can block on provisioning instead of using async event loop
        // wifi_prov_mgr_wait();
        // wifi_prov_mgr_deinit();

    } else {
        // Start STA mode and connect to known existing network
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");
        wifi_deinit_provisioning();
        wifi_start_sta();
    }
}

void wifi_init(void *event_handler) {
    main_event_handler = event_handler;
    ESP_ERROR_CHECK(esp_netif_init());

    // Set up IP and WIFI event handlers. The PROV handler will set up a full connection by itself
    // when user goes through provisioning, but on subsequent boots we connect to the AP through these
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, main_event_handler, NULL));

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t default_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&default_config));
}

void wifi_init_provisioning() {
    if (main_event_handler == NULL) {
        ESP_LOGE(TAG, "wifi_init() not called before trying to set up provisioning, failing irrecoverably");
        return;
    }

    if (wifi_is_provisioning_inited) {
        wifi_deinit_provisioning();
    }

    // Provisioning-specific event handler deprecated, subscribe on default event loop handler.
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, main_event_handler, NULL,
                                                        &provisioning_manager_event_handler));

    wifi_prov_mgr_config_t config = {
        .scheme               = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,  // Only needed for BT/BLE provisioning
        // .app_event_handler = { .event_cb = provisioning_event_handler }         // deprecated in favor of default
        // event loop
    };

    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
    wifi_is_provisioning_inited = true;
}

void wifi_deinit_provisioning() {
    wifi_prov_mgr_deinit();
    esp_event_handler_instance_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, provisioning_manager_event_handler);
    wifi_is_provisioning_inited = false;
}
