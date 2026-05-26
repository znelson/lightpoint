# Shared HAL stubs for host gtest suites. The production lib/hal/ headers
# are both FreeRTOS-clean (HalPlatform.h is just <cstdint>; HalStorage.h
# uses pimpl + a file-static FreeRTOS mutex inside HalStorage.cpp), so tests
# include the real headers verbatim and link these stub .cpp files instead.
#
# If production grows a new method, dependent test builds fail to link
# until a stub lands here -- a deliberate signal rather than silent
# divergence between test and production interfaces.
#
# Tests that need readable file content seed it via HalStorageTestApi.h's
# seedHalFileContent / makeReadableHalFile helpers.

add_library(lightpoint_test_hal_stubs STATIC
  HalPlatform.cpp
  HalStorage.cpp
)

# lib/hal/ is on the include path so consumers resolve <HalPlatform.h> and
# <HalStorage.h> to the real production headers; lib/Print/ is needed because
# HalStorage.h pulls it in for HalFile's Print base class. ${CMAKE_CURRENT_SOURCE_DIR}
# is on the path so the test cpp files here find HalStorageTestApi.h.
target_include_directories(lightpoint_test_hal_stubs PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/Print
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(lightpoint_test_hal_stubs PUBLIC
  lightpoint_test_common
)
