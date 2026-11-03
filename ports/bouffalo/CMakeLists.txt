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
    "${CMAKE_CURRENT_LIST_DIR}/network_probe.cc"
    "${CMAKE_CURRENT_LIST_DIR}/ota_bootstrap.cc"
    "${CMAKE_CURRENT_LIST_DIR}/websocket_bootstrap.cc"
    "${CMAKE_CURRENT_LIST_DIR}/../../main/audio/audio_service.cc"
    "${CMAKE_CURRENT_LIST_DIR}/opus_decoder_bouffalo.cc"
    "${CMAKE_CURRENT_LIST_DIR}/opus_encoder_bouffalo.cc"
    "${CMAKE_CURRENT_LIST_DIR}/voice_input.cc"
    "${CMAKE_CURRENT_LIST_DIR}/audio_codec_bouffalo.cc"
    "${CMAKE_CURRENT_LIST_DIR}/audio_sdk_bridge.c"
    "${CMAKE_CURRENT_LIST_DIR}/audio_dma_worker.c"
    "${CMAKE_CURRENT_LIST_DIR}/audio_encoder_worker.c"
    "${CMAKE_CURRENT_LIST_DIR}/audio_vad.c"
    "${CMAKE_CURRENT_LIST_DIR}/pcm_ring.c"
    "${CMAKE_CURRENT_LIST_DIR}/memory_diagnostics.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../main/audio/audio_codec.cc"
    "${CMAKE_CURRENT_LIST_DIR}/network_websocket.cc"
    "${CMAKE_CURRENT_LIST_DIR}/tcp_bouffalo.cc"
    "${CMAKE_CURRENT_LIST_DIR}/tls_sdk_bridge.c"
    "${CMAKE_CURRENT_LIST_DIR}/tls_read_diagnostics.c"
    "${CMAKE_CURRENT_LIST_DIR}/websocket_support.cc"
    "${CMAKE_CURRENT_LIST_DIR}/../../main/protocols/protocol.cc"
    "${CMAKE_CURRENT_LIST_DIR}/../../main/protocols/websocket_protocol.cc"
    "${CMAKE_CURRENT_LIST_DIR}/http_bouffalo.cc"
    "${CMAKE_CURRENT_LIST_DIR}/board_bouffalo.cc"
    "${CMAKE_CURRENT_LIST_DIR}/settings_bouffalo.cc"
    "${CMAKE_CURRENT_LIST_DIR}/device_identity.cc"
    "${CMAKE_CURRENT_LIST_DIR}/../../main/ota.cc"
    "${CMAKE_CURRENT_LIST_DIR}/../../main/ota_check_response.cc"
    "${CMAKE_CURRENT_LIST_DIR}/network_time.c"
    "${CMAKE_CURRENT_LIST_DIR}/http_sdk_bridge.c"
    "${CMAKE_CURRENT_LIST_DIR}/wifi_sdk_bridge.c")

# Keep the ESP log adapter local to application sources, not SDK libraries.
target_include_directories(app PRIVATE "${CMAKE_CURRENT_LIST_DIR}/compat")
target_include_directories(${CMAKE_PROJECT_NAME}.elf PRIVATE "${CMAKE_CURRENT_LIST_DIR}/compat")

# Temporary diagnostics for SDKs without pthread key APIs. Keep this opt-in so
# SDKs with real implementations retain their normal behavior.
option(XIAOZHI_PTHREAD_KEY_ASSERT_STUBS "Trap unsupported pthread key API calls" OFF)
if(XIAOZHI_PTHREAD_KEY_ASSERT_STUBS)
    message(WARNING "pthread key assertion stubs enabled: any call will halt the firmware")
    foreach(target app ${CMAKE_PROJECT_NAME}.elf)
        target_include_directories(${target} BEFORE PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/compat/pthread_key_assert")
    endforeach()
    # Link directly into the executable: weak libstdc++ references alone would
    # not extract an otherwise unreferenced member from the application archive.
    target_sources(${CMAKE_PROJECT_NAME}.elf PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/pthread_key_assert.c")
endif()

# The original state machine uses C++17 structured bindings.
set_target_properties(app ${CMAKE_PROJECT_NAME}.elf PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(app PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main")
target_include_directories(${CMAKE_PROJECT_NAME}.elf PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main")

option(XIAOZHI_VOICE_INPUT_TEST "Capture and upload spoken questions" ON)
option(XIAOZHI_AUTO_CHAT "Resume voice listening after each reply" ON)
foreach(target app ${CMAKE_PROJECT_NAME}.elf)
    target_compile_definitions(${target} PRIVATE
        XIAOZHI_VOICE_INPUT_TEST=$<BOOL:${XIAOZHI_VOICE_INPUT_TEST}>
        XIAOZHI_AUTO_CHAT=$<BOOL:${XIAOZHI_AUTO_CHAT}>)
endforeach()

# This firmware always has a pthread backend. GCC 10's weak gthread probe uses
# pthread_cancel, which the SDK does not implement, and would skip mutex calls.
foreach(target app ${CMAKE_PROJECT_NAME}.elf)
    target_compile_definitions(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:_GLIBCXX_GTHREAD_USE_WEAK=0>)
endforeach()

# Keep upstream dependency files pristine; generate the C++17 expected-name adaptation.
set(network_include "${CMAKE_CURRENT_BINARY_DIR}/bouffalo_network_include")
file(MAKE_DIRECTORY "${network_include}")
file(GLOB network_headers "${CMAKE_CURRENT_LIST_DIR}/../../third_party/esp-ml307/include/*.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${network_headers})
foreach(header IN LISTS network_headers)
    get_filename_component(name "${header}" NAME)
    file(READ "${header}" content)
    string(REPLACE "#include <expected>" "#include \"platform/expected.h\"" content "${content}")
    string(REPLACE "std::expected<" "xiaozhi::Expected<" content "${content}")
    string(REPLACE "std::unexpected(" "xiaozhi::Unexpected(" content "${content}")
    if(name STREQUAL "web_socket.h" OR name STREQUAL "tcp.h")
        string(REPLACE "#include <string>" "#include <string>\n#include <atomic>" content "${content}")
        string(REPLACE "bool connected_ = false;" "std::atomic<bool> connected_{false};" content "${content}")
    endif()
    file(WRITE "${network_include}/${name}" "${content}")
endforeach()
foreach(target app ${CMAKE_PROJECT_NAME}.elf)
    target_compile_definitions(${target} PRIVATE XIAOZHI_PLATFORM_BOUFFALO=1 XIAOZHI_AUDIO_PLATFORM_BACKEND=1)
    target_include_directories(${target} PRIVATE "${network_include}"
        "${CMAKE_CURRENT_LIST_DIR}/../../main/boards/common"
        "${CMAKE_CURRENT_LIST_DIR}/../../main/protocols"
        "${CMAKE_CURRENT_LIST_DIR}/../../third_party/tl-expected/include")
endforeach()

set(bouffalo_board_header "$ENV{BL_SDK_BASE}/bsp/board/${BOARD}/board.h")
if(NOT EXISTS "${bouffalo_board_header}")
    message(FATAL_ERROR "SDK BSP header not found: ${bouffalo_board_header}")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/sdk_board.h.in" "${network_include}/sdk_board.h" @ONLY)

# Compile the original WebSocket implementation with checked transport fixes.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(websocket_source "${network_include}/web_socket.cc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/prepare_websocket.py"
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/esp-ml307/src/web_socket.cc")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/prepare_websocket.py"
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/esp-ml307/src/web_socket.cc" "${websocket_source}"
    RESULT_VARIABLE websocket_prepare_result)
if(NOT websocket_prepare_result EQUAL 0)
    message(FATAL_ERROR "Cannot prepare the pinned WebSocket implementation")
endif()
target_sources(app PRIVATE "${websocket_source}")

set(bouffalo_audio_header "$ENV{BL_SDK_BASE}/bsp/common/audio_codec/audio_codec.h")
configure_file("${CMAKE_CURRENT_LIST_DIR}/sdk_audio_codec.h.in" "${network_include}/sdk_audio_codec.h" @ONLY)

# Retain caller/size through the SDK allocator failure hook without editing SDK code.
target_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE
    "-Wl,--wrap=pvPortMalloc" "-Wl,--wrap=kmalloc" "-Wl,--wrap=mbedtls_ssl_read")
