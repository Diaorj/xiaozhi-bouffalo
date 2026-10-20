# This file is included from the root CMakeLists.txt.
find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

if(NOT CONFIG_FREERTOS)
    message(FATAL_ERROR "The Bouffalo startup needs CONFIG_FREERTOS=y in its defconfig")
endif()
sdk_add_include_directories("${CMAKE_CURRENT_LIST_DIR}")

# Start with only the Bouffalo entry; original XiaoZhi modules are added later.
sdk_set_main_file("${CMAKE_CURRENT_LIST_DIR}/main.cc")

project(xiaozhi_bouffalo)

# Keep the ESP log adapter local to application sources, not SDK libraries.
target_include_directories(app PRIVATE "${CMAKE_CURRENT_LIST_DIR}/compat")
target_include_directories(${CMAKE_PROJECT_NAME}.elf PRIVATE "${CMAKE_CURRENT_LIST_DIR}/compat")
