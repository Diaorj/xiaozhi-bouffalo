# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# idf.py keeps the original ESP32 path; the BL SDK Makefile selects bouffalo.
set(XIAOZHI_PLATFORM "esp32" CACHE STRING "Build platform: esp32 or bouffalo")
if(XIAOZHI_PLATFORM STREQUAL "bouffalo")
    include("${CMAKE_CURRENT_LIST_DIR}/ports/bouffalo/CMakeLists.txt")
    return()
elseif(NOT XIAOZHI_PLATFORM STREQUAL "esp32")
    message(FATAL_ERROR "Unknown XIAOZHI_PLATFORM: ${XIAOZHI_PLATFORM}")
endif()

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.5.2")
project(xiaozhi)
