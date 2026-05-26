add_executable(JsonWriterTest
  JsonWriterTest.cpp
  ${REPO_ROOT}/lib/JsonIO/JsonWriter.cpp
  ${REPO_ROOT}/lib/JsonIO/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(JsonWriterTest PRIVATE
  ${REPO_ROOT}/lib/JsonIO
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(JsonWriterTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(JsonWriterTest)
