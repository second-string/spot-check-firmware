#ifndef WIFI_H
#define WIFI_H

#include "freertos/event_groups.h"

typedef struct {
    char  *ssid;
    char  *password;
    size_t ssid_len;
    size_t password_len;
} connect_to_network_task_args;

/* Base init that needs to be done on boot no matter what mode we're headed for */
void wifi_init();

/* Uses an event group internally to yield until the wifi task sets the connected bit */
void wifi_block_until_connected();

/*
 * Uses an event group internally to yield until the wifi task sets the connected bit with a timeout in ms
 * Returns true if connected, false if timed out.
 */
bool wifi_block_until_connected_timeout(uint32_t ms_to_wait);

/* Used by tasks to check if connected yet in order to perform logic without having to block on event group */
bool wifi_is_connected_to_network();

/* Simply sets mode and starts, expects config to be done */
void wifi_start_sta();

/*
 * Inits config and event handler for provisioning. Supports being called
 * more than once
 */
void wifi_init_provisioning();

/*
 * Kicks off provisioning mode. Does no checking of previous provisioned status, just stop http server and starts
 * manager.
 */
void wifi_start_provisioning();

/* Deinits provisioning manager, unregisters from event handler, and sets internal flag */
void wifi_deinit_provisioning();

/* Check if we're provisioned */
bool wifi_is_provisioned();

#endif
