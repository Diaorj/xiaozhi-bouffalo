# This file is included from the root CMakeLists.txt.
find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

if(NOT CONFIG_FREERTOS)
    message(FATAL_ERROR "The Bouffalo startup needs CONFIG_FREERTOS=y in its defconfig")
endif()
if(NOT CONFIG_POSIX)
    message(FATAL_ERROR "The original std::mutex needs CONFIG_POSIX=y")
endif()
if(NOT CONFIG_WIFI6 OR NOT CONFIG_FHOST OR NOT CONFIG_LWIP OR NOT CONFIG_RF)
    message(FATAL_ERROR "The network bootstrap needs WIFI6, FHOST, LWIP and RF enabled")
endif()
# Also expose the FHOST-compatible lwipopts_user.h to SDK network components.
sdk_add_include_directories("${CMAKE_CURRENT_LIST_DIR}")

# Build the original state machine with the SDK std::mutex backend.
sdk_set_main_file("${CMAKE_CURRENT_LIST_DIR}/main.cc")

project(xiaozhi_bouffalo)
target_sources(app PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../main/device_state_machine.cc"
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

# This firmware always has a pthread backend. GCC 10's weak gthread probe uses
# pthread_cancel, which the SDK does not implement, and would skip mutex calls.
foreach(target app ${CMAKE_PROJECT_NAME}.elf)
    target_compile_definitions(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:_GLIBCXX_GTHREAD_USE_WEAK=0>)
endforeach()
