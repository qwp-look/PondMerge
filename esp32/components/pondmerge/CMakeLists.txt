# PondMerge core as an IDF component; sources and headers live at the repo root.
# The trailing -std=gnu++17 overrides the project-wide C++ standard for this
# component on purpose: it proves the core stays C++17-clean (task-book 11).
idf_component_register(SRCS "../../../src/core.cpp"
                       INCLUDE_DIRS "../../../include"
                       PRIV_REQUIRES esp_timer freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_ESP32 PM_MAX_OBJECTS=256)
