# v20 made the suite's fixture geometry a compile-time knob (tests/suite.cpp:
# PM_TEST_SEG_BYTES x 64 segments), so BOTH targets now run the full
# suite + concurrency + model on a 64 KiB zone of 1 KiB segments -- the
# classic ESP32 for the first time (its ~200 KiB of static DRAM cannot hold
# the old 256 KiB zone: "dram0_0_seg overflowed by 126,744 bytes"), and the S3
# in plain internal DRAM (the zone no longer needs PSRAM at this size).

set(PM_DEVICE_SRCS
    "main.cpp"
    "../../tests/concurrency_esp32.cpp"
    "../../tests/model.cpp"
    "../../tests/suite.cpp")

idf_component_register(SRCS ${PM_DEVICE_SRCS}
                       INCLUDE_DIRS "../../tests" "../../include" "../components/pondmerge")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
# PM_DEVICE_BUILD: model.cpp reuses the shared zone buffer instead of its own
# static one (device DRAM budget), and main.cpp sizes the model run for the
# device (round-5 guide section 7). PM_TEST_SEG_BYTES is the fixture geometry
# knob that tests/suite.cpp derives its whole zone from.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PM_MAX_OBJECTS=256
    PM_DEVICE_BUILD=1
    PM_TEST_SEG_BYTES=1024)
# The zone attribute stays available to the build system (see tests/suite.cpp)
# but nothing requires external memory at this fixture size any more.
if(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_ZONE_ATTR=EXT_RAM_BSS_ATTR)
endif()
