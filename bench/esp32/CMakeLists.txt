# PondMerge on-target benchmarks -- a SEPARATE IDF project from the acceptance
# firmware in esp32/, so that measuring never disturbs the acceptance build.
#
# It compiles the very same benchmark source as the host target (bench/) plus
# the core, with PM_BENCH_NO_HOST_MAIN so the file's host main() is not defined
# and app_main drives it instead.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pondmerge_bench)
