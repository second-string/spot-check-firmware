# spot-check-embedded
#### Embedded C code for an ESP-32 accessing a custom API, parsing the JSON payload, and outputting LED strip data through the RMT component. 
Firmware for the esp32 based on the esp-idf SDK to setup, retrieve surf data, and display it on an attached LED matrix

Works in conjunction with [a custom api](https://github.com/dot4qu/spot-check-api) for formatting and adjusting payload size and [an iOS app](https://github.com/dot4qu/spot-check-ios) for getting esp32 connected to local network and applying custom user Spot Check configuration.
