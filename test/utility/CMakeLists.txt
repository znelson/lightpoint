add_executable(UtilityTest
  MemoryTest.cpp
  FunctionRefTest.cpp
  Utf8Test.cpp
  ${REPO_ROOT}/lib/Utility/Utf8.cpp
  SerializationTest.cpp
)

target_include_directories(UtilityTest PRIVATE
  ${REPO_ROOT}/lib/Utility
)

# lightpoint_test_hal_stubs supplies the HalStorage / HalFile bodies that
# Serialization.h's HalFile overloads reference, plus the HalStorageTestApi.h
# seeding helpers used by SerializationTest.
target_link_libraries(UtilityTest PRIVATE
  lightpoint_test_hal_stubs
  lightpoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UtilityTest)
