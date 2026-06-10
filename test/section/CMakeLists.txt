add_executable(SectionTest
  SectionTest.cpp
  PageStubs.cpp
  ${REPO_ROOT}/lib/Typesetter/Typesetter/Section.cpp
  # Real Page::serialize/deserialize so the round-trip is honest. The
  # PageStubs.cpp file above provides bodies for PageLine/PageImage that
  # Page.cpp's deserialize switch references but the test never invokes.
  ${REPO_ROOT}/lib/Typesetter/Typesetter/Page.cpp
)

target_include_directories(SectionTest PRIVATE
  # Listed BEFORE lib/GfxRenderer so the test-only stub GfxRenderer.h
  # wins (real one pulls in EInkDisplay.h / Bitmap.h which need IDF).
  ${REPO_ROOT}/test/typesetter
  ${REPO_ROOT}/lib/Typesetter
  ${REPO_ROOT}/lib/Typesetter/Typesetter
  # makeUniqueNoThrow lives here; Page.cpp pulls it in via "Memory.h".
  ${REPO_ROOT}/lib/Utility
  # EpdFontFamily is referenced through TextBlock.h's signatures.
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(SectionTest PRIVATE
  lightpoint_test_hal_stubs
  lightpoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SectionTest)
