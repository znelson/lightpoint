add_executable(Utf8Test
  Utf8Test.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(Utf8Test PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(Utf8Test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Utf8Test)
