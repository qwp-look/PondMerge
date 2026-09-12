# PondMerge -- dual-form build entry.
#
# This single file lets the repository be consumed two ways WITHOUT duplicating
# a line of source. The two branches are mutually exclusive and each is a no-op
# in the other's context.
#
#   1) ESP-IDF component -- this repository root IS the component directory.
#        Consume by adding this repo to a project's EXTRA_COMPONENT_DIRS, or
#        (once published) through the Espressif Component Registry.
#      ESP-IDF sets ESP_PLATFORM=1 both when it processes a component normally
#      AND in the separate `cmake -P` early-expansion pass that discovers each
#      component's REQUIRES (see tools/cmake/component.cmake,
#      __component_get_requirements, which passes -D ESP_PLATFORM=1). So this
#      guard is correct in both passes.
#
#   2) Plain CMake library for host / generic targets.
#        add_subdirectory(<this dir>)        -> target pondmerge::pondmerge
#        find_package(pondmerge 1.0 REQUIRED) -> same target, after install
#
# The in-repo acceptance project (esp32/) keeps its own thin wrapper at
# esp32/components/pondmerge/ so that it can point at these sources with a
# relative path; that wrapper is NOT the published entry point.

# ---------------------------------------------------------------------------
# FORM 1: plain CMake (host / generic). Skipped entirely inside an IDF build.
# ---------------------------------------------------------------------------
if(NOT ESP_PLATFORM)

    cmake_minimum_required(VERSION 3.16)
    project(pondmerge
        VERSION 1.0.0
        DESCRIPTION "Managed memory for MMU-less MCUs: pools, logical refs, explicit compaction"
        LANGUAGES CXX)

    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # ---------------------------------------------------------------------
    # Tunable compile-time configuration.
    #
    # These are PUBLIC on the target so that every translation unit of a
    # consuming program -- including the header-only parts of the API -- is
    # built with the SAME contract. A mismatch between the library and its
    # consumer would be an ODR violation, not a link error, so it must not be
    # left to chance.
    #
    # PM_MAX_OBJECTS is by far the dominant driver of static RAM. The defaults
    # below are sized for a host or a large MCU; see the metadata budget table
    # in README.md before shipping to a small target.
    # ---------------------------------------------------------------------
    set(PM_MAX_OBJECTS  1024 CACHE STRING "Maximum live object descriptors (dominant metadata cost)")
    set(PM_MAX_POOLS      16 CACHE STRING "Maximum number of pools")
    set(PM_MAX_SEGMENTS   64 CACHE STRING "Maximum number of Auto Zone segments")
    set(PM_SL_COUNT        4 CACHE STRING "TLSF second-level bin count (power of two in [2,16])")
    set(PM_FL_MAX         24 CACHE STRING "TLSF first-level ceiling; the Auto Zone must stay below 2^PM_FL_MAX")
    set(PM_DEBUG           1 CACHE STRING "1 = assertions + advice owner gate; 0 = Release")

    # The core is a single translation unit on purpose: it keeps every internal
    # invariant auditable in one place, and it is what the acceptance suite and
    # the on-target firmware both compile.
    add_library(pondmerge STATIC src/core.cpp)
    add_library(pondmerge::pondmerge ALIAS pondmerge)

    target_include_directories(pondmerge
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

    target_compile_features(pondmerge PUBLIC cxx_std_17)
    target_compile_options(pondmerge PRIVATE -Wall -Wextra -Wno-unused-parameter)

    target_compile_definitions(pondmerge PUBLIC
        PM_MAX_OBJECTS=${PM_MAX_OBJECTS}
        PM_MAX_POOLS=${PM_MAX_POOLS}
        PM_MAX_SEGMENTS=${PM_MAX_SEGMENTS}
        PM_SL_COUNT=${PM_SL_COUNT}
        PM_FL_MAX=${PM_FL_MAX}
        PM_DEBUG=${PM_DEBUG})

    # The host port layer uses pthread_self() for the Debug-only advice owner
    # gate. On glibc >= 2.34 it lives in libc, but linking Threads explicitly
    # keeps other libcs working.
    if(NOT MSVC)
        find_package(Threads REQUIRED)
        target_link_libraries(pondmerge PUBLIC Threads::Threads)
    endif()

    install(TARGETS pondmerge
        EXPORT pondmergeTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT pondmergeTargets
        FILE pondmergeTargets.cmake
        NAMESPACE pondmerge::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pondmerge)

    configure_package_config_file(
        cmake/pondmergeConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/pondmergeConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pondmerge)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/pondmergeConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/pondmergeConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/pondmergeConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pondmerge)

endif() # NOT ESP_PLATFORM

# ---------------------------------------------------------------------------
# FORM 2: ESP-IDF component. Skipped in every non-IDF build.
# ---------------------------------------------------------------------------
if(ESP_PLATFORM)

    idf_component_register(
        SRCS "src/core.cpp"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src"
        PRIV_REQUIRES esp_timer freertos)

    # gnu++17 is forced on purpose: it proves the core stays C++17-clean rather
    # than inheriting whatever standard the surrounding project happens to set.
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_ESP32)

    # Sizing is NOT hardcoded here. A consuming project may set any of these as
    # plain CMake variables before the component is processed, which is the
    # intended way to fit the metadata budget of a small target:
    #
    #   set(PM_MAX_OBJECTS 256)      # 31 KiB instead of 105 KiB of static RAM
    #   set(PM_MAX_POOLS 4)
    #
    # Anything not set falls through to the core defaults in pm_config.h.
    foreach(_pm_var PM_MAX_OBJECTS PM_MAX_POOLS PM_MAX_SEGMENTS PM_SL_COUNT PM_FL_MAX PM_DEBUG)
        if(DEFINED ${_pm_var})
            target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_pm_var}=${${_pm_var}})
        endif()
    endforeach()

endif() # ESP_PLATFORM
