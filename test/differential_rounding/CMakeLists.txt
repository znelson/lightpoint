add_executable(DifferentialRoundingTest
  DifferentialRoundingTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utility/Utf8.cpp
)

target_include_directories(DifferentialRoundingTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utility
)

target_link_libraries(DifferentialRoundingTest PRIVATE
  lightpoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DifferentialRoundingTest)
