add_executable(StreamingJsonParserTest
  StreamingJsonParserTest.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(StreamingJsonParserTest PRIVATE
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(StreamingJsonParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StreamingJsonParserTest)
