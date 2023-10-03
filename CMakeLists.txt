# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(PROJECT_VER "0.0.16")


set(MEMFAULT_FIRMWARE_SDK ~/Developer/memfault-firmware-sdk)
include(${MEMFAULT_FIRMWARE_SDK}/ports/esp_idf/memfault.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spot-check-firmware)

add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf
  POST_BUILD
  COMMAND
  python3 ${MEMFAULT_FIRMWARE_SDK}/scripts/fw_build_id.py ${CMAKE_PROJECT_NAME}.elf)


set(SYMBOL_FILE_DIR ${CMAKE_BINARY_DIR}/memfault_elfs)

add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND shasum  -a 256 ${PROJECT_NAME}.elf | head -c 8 | xargs -I {} ${CMAKE_COMMAND} -E copy ${PROJECT_NAME}.elf ${SYMBOL_FILE_DIR}/${PROJECT_NAME}-{}.elf
    COMMAND ${CMAKE_COMMAND} -E echo "Preserving elf in ${SYMBOL_FILE_DIR}")

