# PondMerge core as an IDF component; sources and headers live at the repo root.
# The trailing -std=gnu++17 overrides the project-wide C++ standard for this
# component on purpose: it proves the core stays C++17-clean (task-book 11).
#
# The PM_* sizing macros are PUBLIC on purpose. The public header's inline
# templates (pm_ptr, pm_access, Result) and the static_asserts a consumer's own
# source writes against PM_MAX_OBJECTS must see the SAME values the core was
# compiled with -- a mismatch is an ODR violation (different object layout),
# not a link error, so it must never depend on the consumer remembering to
# pass matching -D flags. Every TU of a component that REQUIRES pondmerge
# receives these definitions automatically; do NOT re-define them in the
# consumer (a stale private -D would override this one and reintroduce the
# mismatch this propagation exists to close).
#
# Sizing: a consuming project may override any of these by setting the plain
# CMake variable BEFORE the component is processed (project CMakeLists, above
# include(project.cmake) or in a set() in the main component):
#   set(PM_MAX_OBJECTS 128)
# Anything unset falls back to the device sizing below (256 live objects,
# ~30 KiB of metadata on the S3), not the library-wide 1024 default.
idf_component_register(SRCS "../../../src/core.cpp"
                       INCLUDE_DIRS "../../../include"
                       PRIV_REQUIRES esp_timer freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_ESP32)

# Defaults sized for the on-device acceptance firmware and examples; see the
# metadata budget table in README.md before raising these on a small target.
if(NOT DEFINED PM_MAX_OBJECTS)
    set(PM_MAX_OBJECTS 256)
endif()
if(NOT DEFINED PM_MAX_POOLS)
    set(PM_MAX_POOLS 16)
endif()
if(NOT DEFINED PM_MAX_SEGMENTS)
    set(PM_MAX_SEGMENTS 64)
endif()
if(NOT DEFINED PM_SL_COUNT)
    set(PM_SL_COUNT 4)
endif()
if(NOT DEFINED PM_FL_MAX)
    set(PM_FL_MAX 24)
endif()
if(NOT DEFINED PM_DEBUG)
    set(PM_DEBUG 1)
endif()
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    PM_MAX_OBJECTS=${PM_MAX_OBJECTS}
    PM_MAX_POOLS=${PM_MAX_POOLS}
    PM_MAX_SEGMENTS=${PM_MAX_SEGMENTS}
    PM_SL_COUNT=${PM_SL_COUNT}
    PM_FL_MAX=${PM_FL_MAX}
    PM_DEBUG=${PM_DEBUG})
