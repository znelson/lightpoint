add_executable(MarkdownParserTest
  MarkdownParserTest.cpp
  ${REPO_ROOT}/lib/Markdown/Markdown/MarkdownParser.cpp
  # Real ParsedText so the parser can call addWord/setBlockStyle/etc.
  # and the tests can introspect words after submit.
  ${REPO_ROOT}/lib/Typesetter/Typesetter/ParsedText.cpp
  # ParsedText.cpp pulls in TextBlock for member type definitions and
  # for the layout helpers it doesn't call here but the linker needs.
  ${REPO_ROOT}/lib/Typesetter/Typesetter/blocks/TextBlock.cpp
  # ParsedText uses Utf8 for character counting and Hyphenation for
  # break candidates; not exercised by these tests but pulled in by
  # the translation unit.
  ${REPO_ROOT}/lib/Utility/Utf8.cpp
  ${REPO_ROOT}/lib/Hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Hyphenation/LiangHyphenation.cpp
)

# Include order matters: ${CMAKE_CURRENT_SOURCE_DIR} wins so MarkdownParser.cpp
# resolves <Typesetter.h> to our test stub instead of the real layout engine.
# We still need the real lib/Typesetter on the path for Typesetter/ParsedText.h
# and friends (those are NOT shadowed).
#
# test/typesetter is on the path for the test-only GfxRenderer.h stub that
# TextBlock.h's signatures reference (compiled but never executed here).
target_include_directories(MarkdownParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/typesetter
  ${REPO_ROOT}/lib/Markdown
  ${REPO_ROOT}/lib/Typesetter
  ${REPO_ROOT}/lib/Typesetter/Typesetter
  ${REPO_ROOT}/lib/Utility
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Hyphenation
)

target_link_libraries(MarkdownParserTest PRIVATE
  lightpoint_test_hal_stubs
  lightpoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MarkdownParserTest)
