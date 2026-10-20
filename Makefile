SDK_DEMO_PATH ?= $(abspath .)
BL_SDK_BASE ?= $(abspath ../../..)
export BL_SDK_BASE
DEFCONFIG ?= ports/bouffalo/defconfig
# Select the Bouffalo path in the root CMakeLists.txt.
cmake_definition += -DXIAOZHI_PLATFORM=bouffalo
include $(BL_SDK_BASE)/project.build
