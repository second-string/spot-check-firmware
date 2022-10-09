# spot-check-firmware

![Overview image of Spot Check rev. 3.1](/img/spot_check_rev_3_1_overview.jpeg)

## Embedded C code for the Spot Check project
Spot Check is a custom hardware, firmware, and software project to download and display calendar, weather, and surf forecast information. There are 4 main parts to the project, each with its own repo:

* Firmware repo: this repo
* Hardware repo: https://github.com/dot4qu/spot-check-hardware
* Backend API repo: https://github.com/dot4qu/spot-check-api
* Configuration iOS app repo: https://github.com/dot4qu/spot-check-ios

 The basic behavior of this firmware is to access a custom Typescript API, parse the JSON or binary image payload, and display that information. There is OTA support where each device checks with the server on boot and compares its current version with the available server version and updates its firmware automatically if behind. Wifi provisioning and device configuration is performed through the iOS app to securely transfer network credentials and settings to and from the firmware.

This project is based on the esp-idf v5.0 SDK which itself is based on a FreeRTOS port. The firmware mostly makes use of the lower-level FreeRTOS structures and functionality rather than the esp-idf helpers when available.

### Rev 3.1

![Front image of Spot Check rev. 3.1](/img/spot_check_rev_3_1_front.jpeg)

_Prototype enclosure for rev. 3.x_

![Spot Check rev. 3.1 PCBA](/img/spot_check_rev_3_1_pcba.jpeg)

_Rev. 3.1 PCBA_

Firmware versions 0.1.0 and later  
This firmware works with hardware rev 3.1 detailed in the hardware repo README. Features include accurate time and date, air temp, wind speed and direction, and swell and tide chart display on an ED060SC4 e-ink display. There is also a CLI for interacting with the device over a USB <-> serial connection.

### Rev 1 / 2

![Spot Check rev. 1 & 2 display](/img/spot_check_rev_2_overview.jpg)

_LED strip display for rev. 1 and 2_

![Spot Check rev 2 PCBA](/img/spot_check_rev_2_pcba.jpg)

_Rev. 2 PCBA_

![Spot Check rev 1 PCBA](/img/spot_check_rev_1_pcba.png)

_Rev. 1 PCBA_

Firmware versions 0.0.0 to 0.0.7  
This firmware works with hardware revs 1 and 2 detailed in the hardware repo README. Air temperature, wind speed and direction, and tide height are displayed using a 60x6 matrix of LEDs created by stacking WS2812 strips. 

