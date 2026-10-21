# This file is included from the root CMakeLists.txt.
find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

if(NOT CONFIG_FREERTOS)
    message(FATAL_ERROR "The Bouffalo startup needs CONFIG_FREERTOS=y in its defconfig")
endif()
if(NOT CONFIG_WIFI6 OR NOT CONFIG_FHOST OR NOT CONFIG_LWIP OR NOT CONFIG_RF)
    message(FATAL_ERROR "The network bootstrap needs WIFI6, FHOST, LWIP and RF enabled")
endif()
# Also expose the FHOST-compatible lwipopts_user.h to SDK network components.
sdk_add_include_directories("${CMAKE_CURRENT_LIST_DIR}")

# Build the shared state machine with a platform-specific mutex.
sdk_set_main_file("${CMAKE_CURRENT_LIST_DIR}/main.cc")

project(xiaozhi_bouffalo)
target_sources(app PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../main/device_state_machine.cc"
    "${CMAKE_CURRENT_LIST_DIR}/cpp_runtime.cc"
    "${CMAKE_CURRENT_LIST_DIR}/wifi_init.cc"
    "${CMAKE_CURRENT_LIST_DIR}/wifi_sdk_bridge.c")

# Keep the ESP log adapter local to application sources, not SDK libraries.
target_include_directories(app PRIVATE "${CMAKE_CURRENT_LIST_DIR}/compat")
target_include_directories(${CMAKE_PROJECT_NAME}.elf PRIVATE "${CMAKE_CURRENT_LIST_DIR}/compat")

# The original state machine uses C++17 structured bindings.
set_target_properties(app ${CMAKE_PROJECT_NAME}.elf PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(app PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main")
target_include_directories(${CMAKE_PROJECT_NAME}.elf PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main")

# Select only the Bouffalo mutex backend; ESP-IDF/host builds keep std::mutex.
target_compile_definitions(app PRIVATE XIAOZHI_PLATFORM_BOUFFALO=1)
target_compile_definitions(${CMAKE_PROJECT_NAME}.elf PRIVATE XIAOZHI_PLATFORM_BOUFFALO=1)
