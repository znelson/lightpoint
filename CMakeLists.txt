cmake_minimum_required(VERSION 3.16.0)
# Must be set before project.cmake is included so IDF's Kconfig picks it up.
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lightpoint)
