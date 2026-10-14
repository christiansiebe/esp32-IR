# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Capture the firmware version at configure time (git describe). The result is
# written to main/build_version.h and compiled into the image, so the running
# firmware can report exactly which commit/tag it was built from.
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE GIT_VER
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT GIT_VER)
    set(GIT_VER "unknown")
endif()
file(WRITE ${CMAKE_CURRENT_LIST_DIR}/main/build_version.h
     "#pragma once\n#define FIRMWARE_VERSION \"${GIT_VER}\"\n")
message(STATUS "Firmware version: ${GIT_VER}")

project(esp32-IR)