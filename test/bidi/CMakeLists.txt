add_executable(BidiUtilsTest
  BidiUtilsTest.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Utility/Utf8.cpp
)

target_include_directories(BidiUtilsTest PRIVATE
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Utility
  # Logging.h: BidiUtils.cpp's LOG_DBG macros compile away to nothing without
  # ENABLE_SERIAL_LOG, but the header still has to resolve.
  ${REPO_ROOT}/lib/Logging
)

target_link_libraries(BidiUtilsTest PRIVATE
  lightpoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BidiUtilsTest)
