add_executable(CssParserTest
  CssParserTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
)

target_include_directories(CssParserTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub/css
  # Mirrors PlatformIO's per-library include-root convention so that
  # production CssStyle.h's `#include <Typesetter/TextAlign.h>` resolves.
  # Add a similar line in any future test whose transitive include chain
  # reaches a lib-style `<Name/Foo.h>` include.
  ${REPO_ROOT}/lib/Typesetter
)

# lightpoint_test_hal_stubs provides the halPlatform / halStorage globals and
# stub method bodies for the production lib/hal/ HalPlatform + HalStorage
# headers, plus the HalStorageTestApi seeding helpers.
target_link_libraries(CssParserTest PRIVATE
  lightpoint_test_hal_stubs
  lightpoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssParserTest)
