SDK_DEMO_PATH ?= $(abspath .)
BL_SDK_BASE ?= $(abspath ../../..)
export BL_SDK_BASE
DEFCONFIG ?= ports/bouffalo/defconfig
# Select the Bouffalo path in the root CMakeLists.txt.
cmake_definition += -DXIAOZHI_PLATFORM=bouffalo
# Temporary diagnostic only: trap unsupported pthread key calls.
XIAOZHI_PTHREAD_KEY_ASSERT_STUBS ?= OFF
cmake_definition += -DXIAOZHI_PTHREAD_KEY_ASSERT_STUBS=$(XIAOZHI_PTHREAD_KEY_ASSERT_STUBS)
include $(BL_SDK_BASE)/project.build
