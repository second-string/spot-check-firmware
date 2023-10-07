#include "constants.h"

#include <string.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>

#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "memfault/panics/assert.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "constants.h"
#include "http_server.h"
#include "log.h"
#include "mdns_local.h"
#include "scheduler_task.h"
#include "spot_check.h"
#include "wifi.h"

#define TAG SC_TAG_WIFI

// Local configuration network
#define CONFIG_AP_SSID CONFIG_CONFIGURATION_ACCESS_POINT_SSID

// Set when connected to STA and recieved IP, does NOT indicate actual connection to the internet
#define WIFI_EVENT_GROUP_CONNECTED_TO_NETWORK_BIT (1 << 0)

// This can be really bad on boot sometimes
#define PROVISIONED_NETWORK_CONNECTION_MAXIMUM_RETRY 6

static bool wifi_is_provisioning_inited = false;

static esp_event_handler_instance_t provisioning_manager_event_handler;
static EventGroupHandle_t           wifi_event_group;
static volatile int                 sta_connect_attempts = 0;

/*
 * Main event handler function for setting whether we're connected or not and printing out statuses
 */
void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    // STA mode events (connecting to internet-based station)
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_CONNECTED:
                log_printf(LOG_LEVEL_INFO, "Got STA_CONN event");
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                // This case occurs both when we can't connect to previously provisioned network on startup (because it
                // no longer exists) or temporary discons from network that is still present. Hand off to scheduler to
                // poll.
                if (sta_connect_attempts < PROVISIONED_NETWORK_CONNECTION_MAXIMUM_RETRY) {
                    esp_wifi_connect();
                    sta_connect_attempts++;
                } else {
                    scheduler_mode_t mode = scheduler_get_mode();
                    if (mode == SCHEDULER_MODE_INIT) {
                        // Failed to connect to network on boot. We disregard potential case that for some reason the
                        // network isn't working/available and assume re-provisioning is needed, so we don't kick
                        // offline mode. If we're reprovisioning, we're giving up on ever having a successful network
                        // request this boot, so device will either need to be rebooted to retry connecting to network
                        // or provisioned with new creds
                        log_printf(LOG_LEVEL_INFO,
                                   "Got STA_DISCON and max quick retries during boot, leaving scheduler in INIT mode "
                                   "so main can start the prov mgr");
                    } else {
                        // This case is a loss of connection to the network after the device has been successfully
                        // running. Kick to offline and scheduler will handle either healthchecking (if network
                        // available) or wifi re-initing (if trying to reconnect to network)
                        log_printf(
                            LOG_LEVEL_INFO,
                            "Got STA_DISCON and max quick retries, kickng scheduler into offline mode to poll until "
                            "network is found / comes back");

                        xEventGroupClearBits(wifi_event_group, WIFI_EVENT_GROUP_CONNECTED_TO_NETWORK_BIT);
                        spot_check_set_offline_mode();
                    }
                }
                break;
            }
            default:
                log_printf(LOG_LEVEL_INFO, "Got unknown WIFI event id: %d", event_id);
        }
    }

    // IP events, needed for actual connection to provisioned network once in STA mode
    if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                log_printf(LOG_LEVEL_INFO, "Setting CONNECTED bit, got ip:" IPSTR, IP2STR(&event->ip_info.ip));
                sta_connect_attempts = 0;
                mdns_advertise_tcp_service();

                // Signal to any tasks blocking on an internet connection that they're good to go
                xEventGroupSetBits(wifi_event_group, WIFI_EVENT_GROUP_CONNECTED_TO_NETWORK_BIT);

                // We only start our http server upon IP assignment if this is a normal startup
                // in STA mode where we already have creds. If we're in this state after connecting
                // through a provisioning, we might not have enough sockets (I think) and the http server
                // start will fail sometimes. In the PROV_END case below, we force a reboot once we're done
                // provisioning, which always frees up the ability to successfully start the http_server
                if (!wifi_is_provisioning_inited) {
                    http_server_start();
                }
                break;
            }
            default:
                log_printf(LOG_LEVEL_INFO, "Got unknown IP event id: %d", event_id);
        }
    }

    // Provisioning manager events
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_INIT:
                log_printf(LOG_LEVEL_INFO, "Provisioning inited event emitted");
                break;
            case WIFI_PROV_START:
                log_printf(LOG_LEVEL_INFO, "Provisioning started event emitted");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                size_t             ssid_len     = strnlen((char *)wifi_sta_cfg->ssid, sizeof(wifi_sta_cfg->ssid));
                log_printf(LOG_LEVEL_INFO,
                           "Received provisioning creds event - SSID: %s (length %d), PW: %s",
                           wifi_sta_cfg->ssid,
                           ssid_len,
                           wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                log_printf(LOG_LEVEL_ERROR,
                           "Provisioning failed: %s",
                           (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "AP PW incorrect" : "AP not found");
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                log_printf(LOG_LEVEL_INFO, "Provisioning successful event emitted");
                break;
            case WIFI_PROV_END: {
                log_printf(LOG_LEVEL_INFO, "Provisioning complete event emitted, restarting");
                wifi_deinit_provisioning();
                esp_restart();
                break;
            }
            case WIFI_PROV_DEINIT:
                log_printf(LOG_LEVEL_INFO, "Provisioning deinited event emitted");
                break;
            default:
                log_printf(LOG_LEVEL_INFO, "Received unsupported provisioning event: %d", event_id);
                break;
        }
    }
}

void wifi_start_sta() {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_init(void *event_handler) {
    wifi_event_group = xEventGroupCreate();
    MEMFAULT_ASSERT(wifi_event_group);

    ESP_ERROR_CHECK(esp_netif_init());

    // Set up IP and WIFI event handlers. The PROV handler will set up a full connection by itself
    // when user goes through provisioning, but on subsequent boots we connect to the AP through these
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t default_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&default_config));
}

/*
 * Returns true if prov starting successfully, false if already provisioned
 */
void wifi_start_provisioning() {
    if (wifi_event_group == NULL) {
        log_printf(
            LOG_LEVEL_ERROR,
            "wifi_init() not called before trying to start provisioning or connect to sta, failing irrecoverably");
        return;
    }

    // SSID / device name (softap / ble respectively)
    char *service_name = CONFIG_AP_SSID;

    // 0 is plaintext, 1 uses a secure handshake for key exchange with proof of possession
    wifi_prov_security_t security = WIFI_PROV_SECURITY_0;

    // Only used for WIFI_PROV_SECURITY_1 security level
    const char *proof_of_poss = "abc123";

    // Network password (only used for softap prov)
    const char *service_key = NULL;

    // MUST be manually stopped before trying to start prov mgr, otherwise call to
    // wifi_prov_mgr_start_provisioning will return ESP_FAIL and fail error check since it needs it's
    // own http_server for softap provisioning
    http_server_stop();

    log_printf(LOG_LEVEL_INFO, "%s", "Starting provisioning manager");
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, proof_of_poss, service_name, service_key));
}

void wifi_init_provisioning() {
    if (wifi_event_group == NULL) {
        log_printf(LOG_LEVEL_ERROR,
                   "wifi_init() not called before trying to set up provisioning, failing irrecoverably");
        return;
    }

    if (wifi_is_provisioning_inited) {
        wifi_deinit_provisioning();
    }

    // Provisioning-specific event handler deprecated, subscribe on default event loop handler.
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_PROV_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        wifi_event_handler,
                                                        NULL,
                                                        &provisioning_manager_event_handler));

    wifi_prov_mgr_config_t config = {
        .scheme               = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,  // Only needed for BT/BLE provisioning
        // .app_event_handler = { .event_cb = provisioning_event_handler }         // deprecated in favor of default
        // event loop
    };

    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
    wifi_is_provisioning_inited = true;
    log_printf(LOG_LEVEL_DEBUG, "Provisioning manager inited");
}

void wifi_deinit_provisioning() {
    wifi_prov_mgr_deinit();
    esp_event_handler_instance_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, provisioning_manager_event_handler);
    wifi_is_provisioning_inited = false;
}

/*
 * Should be called by any task that needs to wait forever until the device is connected to a wifi network. Does NOT
 * necessarily mnean  there is an available inernet connection. Does not clear the bit on exit, as that should only be
 * done by the wifi task handler if connection is lost.
 */
void wifi_block_until_connected() {
    wifi_block_until_connected_timeout(portMAX_DELAY);
}

/*
 * Should be called by any task that is waiting on a connection to a newtork but wants to give up eventually. Does NOT
 * necessarily mnean  there is an available inernet connection. Does not clear the bit on exit, as that should only be
 * done by the wifi task handler if connection is lost. Returns true if connected, false if timed out.
 */
bool wifi_block_until_connected_timeout(uint32_t ms_to_wait) {
    uint32_t ticks_to_wait;
    if (ms_to_wait == portMAX_DELAY) {
        ticks_to_wait = portMAX_DELAY;
    } else {
        ticks_to_wait = pdMS_TO_TICKS(ms_to_wait);
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_EVENT_GROUP_CONNECTED_TO_NETWORK_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           ticks_to_wait);
    return bits & WIFI_EVENT_GROUP_CONNECTED_TO_NETWORK_BIT;
}

/*
 * Returns true if esp is actively connected to a wifi network and has been assigned an IP.
 * Does NOT necessarily mean there is an available internet connection.
 */
bool wifi_is_connected_to_network() {
    return WIFI_EVENT_GROUP_CONNECTED_TO_NETWORK_BIT &
           xEventGroupWaitBits(wifi_event_group, WIFI_EVENT_GROUP_CONNECTED_TO_NETWORK_BIT, pdFALSE, pdTRUE, 0);
}

/*
 * Just uses the esp-idf api function to check the NVS creds, so it doesn't need provisioning to be running
 */
bool wifi_is_provisioned() {
    bool already_provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&already_provisioned));
    return already_provisioned;
}
