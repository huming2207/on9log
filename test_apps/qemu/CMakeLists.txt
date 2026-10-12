# ESP32-S3 test app for on9log, run in QEMU by ci/run_tests.sh.
cmake_minimum_required(VERSION 3.22)

# Build the on9log component from this checkout (its directory must be named "on9log").
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
# Only what the test needs, for a faster build.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(on9log_qemu_test)
