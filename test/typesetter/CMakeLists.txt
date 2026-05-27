add_executable(TypesetterTest
  TypesetterTest.cpp
  PageStubs.cpp
  ${REPO_ROOT}/lib/Typesetter/Typesetter.cpp
)

# Include order matters: this directory wins over ${REPO_ROOT}/lib/GfxRenderer/
# so production code under test sees the test-only stub GfxRenderer.h instead
# of the real one (which pulls in EpdFontFamily, HalDisplay, Bitmap, etc.).
target_include_directories(TypesetterTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/Typesetter
  # Transitively required by Typesetter/Page.h -> blocks/TextBlock.h -> EpdFontFamily.h.
  # We only need the headers (no symbols linked) since tests don't instantiate fonts.
  ${REPO_ROOT}/lib/EpdFont
  # Typesetter.cpp uses makeUniqueNoThrow<Page>() from lib/Memory/Memory.h.
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(TypesetterTest PRIVATE
  lightpoint_test_hal_stubs
  lightpoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TypesetterTest)
