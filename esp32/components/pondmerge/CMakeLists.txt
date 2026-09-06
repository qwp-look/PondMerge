# PondMerge core as an IDF component; sources and headers live at the repo root.
idf_component_register(SRCS "../../../src/core.cpp"
                       INCLUDE_DIRS "../../../include"
                       PRIV_REQUIRES esp_timer freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_ESP32 PM_MAX_OBJECTS=256)
