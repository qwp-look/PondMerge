cmake_minimum_required(VERSION 3.16)
# Reuses the same pondmerge component as the acceptance firmware
# (esp32/components/pondmerge), so the example compiles the exact core that
# the tests certify -- not a second copy of it.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../esp32/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pondmerge_sensor_pipeline)
