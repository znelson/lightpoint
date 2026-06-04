add_executable(TypesetterTest
  TypesetterTest.cpp
  PageStubs.cpp
  ${REPO_ROOT}/lib/Typesetter/Typesetter.cpp
  # Real layout engine: Tier 2 tests exercise submitParagraph / partialFlush
  # which run end-to-end through ParsedText + TextBlock + Hyphenation + Utf8.
  ${REPO_ROOT}/lib/Typesetter/Typesetter/ParsedText.cpp
  ${REPO_ROOT}/lib/Typesetter/Typesetter/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Utility/Utf8.cpp
  ${REPO_ROOT}/lib/Hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Hyphenation/LiangHyphenation.cpp
  # ParsedText.cpp and TextBlock.cpp call BidiUtils::startsWithRtl /
  # detectParagraphLevel / computeVisualWordOrder; link the real impl so
  # English-only test inputs go down the same code paths as production.
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
)

# Include order matters: this directory wins over ${REPO_ROOT}/lib/GfxRenderer/
# so production code under test sees the test-only stub GfxRenderer.h instead
# of the real one (which pulls in EpdFontFamily, HalDisplay, Bitmap, etc.).
target_include_directories(TypesetterTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/Typesetter
  # Required for EpdFontFamily.h (Style enum on layout method signatures).
  ${REPO_ROOT}/lib/EpdFont
  # Typesetter.cpp uses makeUniqueNoThrow<Page>() from lib/Utility/Memory.h.
  ${REPO_ROOT}/lib/Utility
  # Hyphenator headers for the real layout pipeline; Utf8.h and the
  # Serialization header (used by TextBlock.cpp) both come from lib/Utility,
  # already on the include path above.
  ${REPO_ROOT}/lib/Hyphenation
  # BidiUtils.h / minibidi.h for the RTL detection path that ParsedText
  # and TextBlock now call into.
  ${REPO_ROOT}/lib/MiniBidi
)

target_link_libraries(TypesetterTest PRIVATE
  lightpoint_test_hal_stubs
  lightpoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TypesetterTest)
