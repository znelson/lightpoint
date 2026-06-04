add_executable(StreamingJsonParserTest
  StreamingJsonParserTest.cpp
  ${REPO_ROOT}/lib/JsonIO/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/Utility/Utf8.cpp
)

target_include_directories(StreamingJsonParserTest PRIVATE
  ${REPO_ROOT}/lib/JsonIO
  ${REPO_ROOT}/lib/Utility
)

target_link_libraries(StreamingJsonParserTest PRIVATE
  lightpoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StreamingJsonParserTest)
