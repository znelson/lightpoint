add_executable(HyphenationEvaluationTest
  HyphenationEvaluationTest.cpp
  ${REPO_ROOT}/lib/Hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utility/Utf8.cpp
)

target_include_directories(HyphenationEvaluationTest PRIVATE
  ${REPO_ROOT}/lib/Hyphenation
  ${REPO_ROOT}/lib/Utility
)

target_compile_definitions(HyphenationEvaluationTest PRIVATE
  HYPHENATION_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

target_link_libraries(HyphenationEvaluationTest PRIVATE
  lightpoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HyphenationEvaluationTest)
