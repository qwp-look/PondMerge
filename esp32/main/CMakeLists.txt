# Which acceptance groups this firmware contains depends on the target's static
# DRAM. The reason is not performance: the suite pins its own zone size to 64
# segments in its assertions (test [10] asserts 16 pools x 4 segments fills the
# zone, test [2] creates a 32-segment pool), so it cannot be shrunk. A classic
# ESP32 has ~200 KiB of static DRAM in total and the link fails on the 256 KiB
# zone with "dram0_0_seg overflowed by 126,744 bytes".
#
#   ESP32-S3 and similar : suite + concurrency + model   (zone 256 KiB)
#   classic ESP32        : concurrency + model           (zone 64 KiB)
#
# CONFIG_IDF_TARGET is the CMake variable IDF sets from the resolved sdkconfig,
# so the fork follows the actual target rather than a hand-set option that could
# disagree with it.
if(CONFIG_IDF_TARGET STREQUAL "esp32")
    set(PM_DEVICE_NO_SUITE 1)
    # The concurrency test uses the first 8 KiB of the buffer and the model up to
    # 64 KiB, so 64 KiB is the whole requirement once the suite is out.
    set(PM_TEST_ZONE_BYTES 65536)
else()
    set(PM_DEVICE_NO_SUITE 0)
    set(PM_TEST_ZONE_BYTES 262144)
endif()

set(PM_DEVICE_SRCS
    "main.cpp"
    "../../tests/concurrency_esp32.cpp"
    "../../tests/model.cpp")
if(NOT PM_DEVICE_NO_SUITE)
    list(APPEND PM_DEVICE_SRCS "../../tests/suite.cpp")
endif()

idf_component_register(SRCS ${PM_DEVICE_SRCS}
                       INCLUDE_DIRS "../../tests" "../../include" "../components/pondmerge")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
# PM_DEVICE_BUILD: model.cpp reuses the shared zone buffer instead of its own
# static one (device DRAM budget), and main.cpp sizes the model run for the
# device (round-5 guide section 7). PM_TEST_ZONE_BYTES is the size of that shared
# buffer, set above from the target.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PM_MAX_OBJECTS=256
    PM_DEVICE_BUILD=1
    PM_TEST_ZONE_BYTES=${PM_TEST_ZONE_BYTES})
# With PSRAM enabled the suite's 256 KiB zone is placed in external memory (see
# tests/suite.cpp for why); the attribute is passed from the build system so the
# source keeps a single, cppcheck-clean configuration.
if(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_ZONE_ATTR=EXT_RAM_BSS_ATTR)
endif()
if(PM_DEVICE_NO_SUITE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_DEVICE_NO_SUITE=1)
endif()
