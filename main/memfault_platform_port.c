//! @file
//!
//! Copyright 2023 Memfault, Inc
//!
//! Licensed under the Apache License, Version 2.0 (the "License");
//! you may not use this file except in compliance with the License.
//! You may obtain a copy of the License at
//!
//!     http://www.apache.org/licenses/LICENSE-2.0
//!
//! Unless required by applicable law or agreed to in writing, software
//! distributed under the License is distributed on an "AS IS" BASIS,
//! WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//! See the License for the specific language governing permissions and
//! limitations under the License.
//!
//! Glue layer between the Memfault SDK and the underlying platform
//!
//! TODO: Fill in FIXMEs below for your platform

#include "memfault/components.h"
#include "memfault/ports/reboot_reason.h"

#include <stdbool.h>
#include <sys/time.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "log.h"
#include "spot_check.h"

#define TAG SC_TAG_MFLT_PORT

#define NUM_BYTES_SHA (16)

// These are declared static global because memfault told me i had to

void memfault_platform_get_device_info(sMemfaultDeviceInfo *info) {
    // IMPORTANT: All strings returned in info must be constant
    // or static as they will be used _after_ the function returns

    // See https://mflt.io/version-nomenclature for more context
    *info = (sMemfaultDeviceInfo){
        // An ID that uniquely identifies the device in your fleet
        // (i.e serial number, mac addr, chip id, etc)
        // Regular expression defining valid device serials: ^[-a-zA-Z0-9_]+$
        .device_serial = spot_check_get_serial(),
        // A name to represent the firmware running on the MCU.
        // (i.e "ble-fw", "main-fw", or a codename for your project)
        .software_type = "spot-check-fw",
        // The version of the "software_type" currently running.
        // "software_type" + "software_version" must uniquely represent
        // a single binary
        .software_version = spot_check_get_fw_version(),
        // The revision of hardware for the device. This value must remain
        // the same for a unique device.
        // (i.e evt, dvt, pvt, or rev1, rev2, etc)
        // Regular expression defining valid hardware versions: ^[-a-zA-Z0-9_\.\+]+$
        .hardware_version = spot_check_get_hw_version(),
    };
}

//! Last function called after a coredump is saved. Should perform
//! any final cleanup and then reset the device
void memfault_platform_reboot(void) {
    // This doesn't seem to have an effect - I'm assuming it's somehow overriden by the esp menuconfig option to halt
    // cpu on dump
    esp_restart();
    while (1) {
    }  // unreachable
}

bool memfault_platform_time_get_current(sMemfaultCurrentTime *memfault_time) {
    time_t now = 0;
    time(&now);

    // Return invalid time if we haven't synced sntp yet and let the memfault server set the rxd time
    // This value is 1 year past the epoch start
    if (now < 31540000) {
        *memfault_time = (sMemfaultCurrentTime){
            .type = kMemfaultCurrentTimeType_UnixEpochTimeSec,
            .info =
                {
                    .unix_timestamp_secs = now,
                },
        };
        return false;
    }

    *memfault_time = (sMemfaultCurrentTime){
        .type = kMemfaultCurrentTimeType_UnixEpochTimeSec,
        .info =
            {
                .unix_timestamp_secs = now,
            },
    };

    return true;
}

size_t memfault_platform_sanitize_address_range(void *start_addr, size_t desired_size) {
    // TODO :: leaving this as-is for now, but this is the memory layout from the linker file:
    /*
     *
     * Memory Configuration
     *
     * Name             Origin             Length             Attributes
     * iram0_0_seg      0x0000000040080000 0x0000000000020000 xr
     * iram0_2_seg      0x00000000400d0020 0x000000000032ffe0 xr
     * dram0_0_seg      0x000000003ffb0000 0x000000000002c200 rw
     * drom0_0_seg      0x000000003f400020 0x00000000003fffe0 r
     * rtc_iram_seg     0x00000000400c0000 0x0000000000002000 xrw
     * rtc_data_seg     0x000000003ff80000 0x0000000000002000 rw
     * rtc_slow_seg     0x0000000050000000 0x0000000000002000 rw
     * extern_ram_seg   0x000000003f800000 0x0000000000400000 xrw
     * *default*        0x0000000000000000 0xffffffffffffffff
     *
     * I think theoretically we want to restrict to these sections, but still not sure what's going on with that
     * *default* at the end
     */

    static const struct {
        uint32_t start_addr;
        size_t   length;
    } s_mcu_mem_regions[] = {
        // !FIXME: Update with list of valid memory banks to collect in a coredump
        {.start_addr = 0x00000000, .length = 0xFFFFFFFF},
    };

    for (size_t i = 0; i < MEMFAULT_ARRAY_SIZE(s_mcu_mem_regions); i++) {
        const uint32_t lower_addr = s_mcu_mem_regions[i].start_addr;
        const uint32_t upper_addr = lower_addr + s_mcu_mem_regions[i].length;
        if ((uint32_t)start_addr >= lower_addr && ((uint32_t)start_addr < upper_addr)) {
            return MEMFAULT_MIN(desired_size, upper_addr - (uint32_t)start_addr);
        }
    }

    return 0;
}

int memfault_platform_boot(void) {
    // TODO :: do I need to do anything here...?
    // !FIXME: Add init to any platform specific ports here.
    // (This will be done in later steps in the getting started Guide)

    memfault_build_info_dump();
    memfault_device_info_dump();
    memfault_platform_reboot_tracking_boot();

    // initialize the event storage buffer
    static uint8_t                   s_event_storage[1024];
    const sMemfaultEventStorageImpl *evt_storage =
        memfault_events_storage_boot(s_event_storage, sizeof(s_event_storage));

    // configure trace events to store into the buffer
    memfault_trace_event_boot(evt_storage);

    // record the current reboot reason
    memfault_reboot_tracking_collect_reset_info(evt_storage);

    // configure the metrics component to store into the buffer
    sMemfaultMetricBootInfo boot_info = {
        .unexpected_reboot_count = memfault_reboot_tracking_get_crash_count(),
    };
    memfault_metrics_boot(evt_storage, &boot_info);

    MEMFAULT_LOG_INFO("Memfault Initialized!");

    return 0;
}
