add_executable(ReleaseJsonParserTest
  ReleaseJsonParserTest.cpp
  ${REPO_ROOT}/lib/JsonIO/ReleaseJsonParser.cpp
  ${REPO_ROOT}/lib/JsonIO/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ReleaseJsonParserTest PRIVATE
  ${REPO_ROOT}/lib/JsonIO
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(ReleaseJsonParserTest PRIVATE
  lightpoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReleaseJsonParserTest)
