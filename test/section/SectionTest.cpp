// Round-trip and lifecycle tests for Typesetter::Section. Each test drives
// the public API through openForWrite -> writeHeader -> writePage -> finalize
// against the host-side HalStorage stub (test/stubs/HalStorage.cpp) which
// publishes writes to an in-memory map. A subsequent loadHeader / loadPage /
// forEachAnchor / getPageFor* reads the same path through the same stub.
//
// Pages use PageHorizontalRule exclusively -- it's the only PageElement
// subclass whose round-trip needs no Block or rendering machinery.

#include <Typesetter/Page.h>
#include <Typesetter/Section.h>
#include <gtest/gtest.h>

#include "HalStorageTestApi.h"

namespace {

constexpr const char* kPath = "/test/section.bin";

// Canonical render params used by most tests; mismatch tests build their
// own variants.
struct Params {
  int fontId = 7;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 800;
  uint16_t viewportHeight = 480;
  bool hyphenationEnabled = true;
  bool embeddedStyle = false;
  uint8_t imageRendering = 1;
  bool focusReadingEnabled = false;
};

void writeHeaderFrom(Section& sec, const Params& p) {
  sec.writeHeader(p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment, p.viewportWidth,
                  p.viewportHeight, p.hyphenationEnabled, p.embeddedStyle, p.imageRendering, p.focusReadingEnabled);
}

bool loadHeaderFrom(Section& sec, const Params& p) {
  return sec.loadHeader(p.fontId, p.lineCompression, p.extraParagraphSpacing, p.paragraphAlignment, p.viewportWidth,
                        p.viewportHeight, p.hyphenationEnabled, p.embeddedStyle, p.imageRendering,
                        p.focusReadingEnabled);
}

// Constructs a Page with one PageHorizontalRule. The id parameter biases
// the rule's `width` field (offset by 1 since PageHorizontalRule::deserialize
// rejects width==0); tests don't read width back but distinct widths make
// each page's bytes differ, so a mis-routed loadPage(i) is more likely to
// fail deserialization than silently return the wrong content.
std::unique_ptr<Page> makePage(uint16_t id) {
  auto page = std::make_unique<Page>();
  page->elements.push_back(std::make_shared<PageHorizontalRule>(/*width=*/static_cast<uint16_t>(id + 1),
                                                                /*thickness=*/2,
                                                                /*xPos=*/10, /*yPos=*/20));
  return page;
}

class SectionTest : public ::testing::Test {
 protected:
  void SetUp() override { test_stubs::clearHalFileContent(); }
  void TearDown() override { test_stubs::clearHalFileContent(); }
};

// --- Header round-trip ----------------------------------------------------

TEST_F(SectionTest, EmptySectionRoundTrips) {
  Section sec(kPath);
  Params p;

  ASSERT_TRUE(sec.openForWrite());
  writeHeaderFrom(sec, p);
  ASSERT_TRUE(sec.finalize(/*lut=*/{}, /*anchors=*/{}));

  Section reader(kPath);
  ASSERT_TRUE(loadHeaderFrom(reader, p));
  EXPECT_EQ(reader.pageCount, 0u);
}

TEST_F(SectionTest, LoadHeaderRejectsVersionMismatch) {
  Section sec(kPath);
  Params p;
  ASSERT_TRUE(sec.openForWrite());
  writeHeaderFrom(sec, p);
  ASSERT_TRUE(sec.finalize({}, {}));

  // Tamper the version byte (offset 0) in storage. The next loadHeader
  // should reject and clearCache the file.
  std::string fileBytes;
  {
    HalFile f;
    ASSERT_TRUE(halStorage.openFileForRead("T", kPath, f));
    fileBytes.resize(f.size());
    f.read(fileBytes.data(), fileBytes.size());
  }
  fileBytes[0] = static_cast<char>(Section::FILE_VERSION + 1);
  test_stubs::seedHalFileContent(kPath, fileBytes);

  Section reader(kPath);
  EXPECT_FALSE(loadHeaderFrom(reader, p));
  EXPECT_FALSE(halStorage.exists(kPath));  // clearCache removed it
}

TEST_F(SectionTest, LoadHeaderRejectsParamMismatch) {
  Section sec(kPath);
  Params written;
  ASSERT_TRUE(sec.openForWrite());
  writeHeaderFrom(sec, written);
  ASSERT_TRUE(sec.finalize({}, {}));

  Params different = written;
  different.viewportWidth = 600;  // any single mismatch suffices

  Section reader(kPath);
  EXPECT_FALSE(loadHeaderFrom(reader, different));
  EXPECT_FALSE(halStorage.exists(kPath));  // param mismatch also clears
}

// --- Page round-trip ------------------------------------------------------

TEST_F(SectionTest, MultiPageRoundTrip) {
  Section sec(kPath);
  Params p;
  ASSERT_TRUE(sec.openForWrite());
  writeHeaderFrom(sec, p);

  std::vector<PageLutEntry> lut;
  for (uint16_t i = 0; i < 3; i++) {
    const uint32_t off = sec.writePage(makePage(i));
    ASSERT_NE(off, 0u);
    lut.push_back({off, /*paragraphIndex=*/static_cast<uint16_t>(i * 10),
                   /*listItemIndex=*/static_cast<uint16_t>(i * 5)});
  }
  ASSERT_TRUE(sec.finalize(lut, /*anchors=*/{}));
  EXPECT_EQ(sec.pageCount, 3u);

  Section reader(kPath);
  ASSERT_TRUE(loadHeaderFrom(reader, p));
  EXPECT_EQ(reader.pageCount, 3u);

  for (int i = 0; i < 3; i++) {
    auto page = reader.loadPage(i);
    ASSERT_TRUE(page);
    ASSERT_EQ(page->elements.size(), 1u);
    EXPECT_EQ(page->elements[0]->getTag(), TAG_PageHorizontalRule);
  }
}

// --- Anchor map -----------------------------------------------------------

TEST_F(SectionTest, GetPageForAnchorFindsKnownAndRejectsUnknown) {
  Section sec(kPath);
  Params p;
  ASSERT_TRUE(sec.openForWrite());
  writeHeaderFrom(sec, p);
  const uint32_t off = sec.writePage(makePage(0));
  ASSERT_NE(off, 0u);

  std::vector<std::pair<std::string, uint16_t>> anchors = {
      {"chapter1", 0},
      {"sec1.2", 0},
      {"appendix", 0},
  };
  ASSERT_TRUE(sec.finalize({{off, 0, 0}}, anchors));

  Section reader(kPath);
  EXPECT_EQ(reader.getPageForAnchor("chapter1"), std::optional<uint16_t>{0});
  EXPECT_EQ(reader.getPageForAnchor("appendix"), std::optional<uint16_t>{0});
  EXPECT_EQ(reader.getPageForAnchor("nope"), std::nullopt);
}

TEST_F(SectionTest, ForEachAnchorPredicateSkipsKey) {
  Section sec(kPath);
  Params p;
  ASSERT_TRUE(sec.openForWrite());
  writeHeaderFrom(sec, p);
  const uint32_t off = sec.writePage(makePage(0));
  ASSERT_TRUE(sec.finalize({{off, 0, 0}}, {
                                              {"a", 0},         // len 1
                                              {"chapter1", 0},  // len 8
                                              {"xy", 0},        // len 2
                                              {"appendix", 0},  // len 8
                                          }));

  Section reader(kPath);

  // Predicate accepts only length-8 keys; consumer should see exactly two.
  std::vector<std::string> seen;
  reader.forEachAnchor([](uint32_t keyLen) { return keyLen == 8; },
                       [&](const std::string& key, uint16_t) {
                         seen.push_back(key);
                         return true;
                       });
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0], "chapter1");
  EXPECT_EQ(seen[1], "appendix");
}

TEST_F(SectionTest, ForEachAnchorConsumerStopsIteration) {
  Section sec(kPath);
  Params p;
  ASSERT_TRUE(sec.openForWrite());
  writeHeaderFrom(sec, p);
  const uint32_t off = sec.writePage(makePage(0));
  ASSERT_TRUE(sec.finalize({{off, 0, 0}}, {
                                              {"first", 0},
                                              {"second", 0},
                                              {"third", 0},
                                          }));

  Section reader(kPath);

  std::vector<std::string> seen;
  reader.forEachAnchor([](uint32_t) { return true; },
                       [&](const std::string& key, uint16_t) {
                         seen.push_back(key);
                         return false;  // stop after first
                       });
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0], "first");
}

TEST_F(SectionTest, ForEachAnchorEmptyMapIteratesNothing) {
  Section sec(kPath);
  Params p;
  ASSERT_TRUE(sec.openForWrite());
  writeHeaderFrom(sec, p);
  const uint32_t off = sec.writePage(makePage(0));
  ASSERT_TRUE(sec.finalize({{off, 0, 0}}, /*anchors=*/{}));

  Section reader(kPath);
  int calls = 0;
  EXPECT_TRUE(reader.forEachAnchor(
      [&](uint32_t) {
        calls++;
        return true;
      },
      [&](const std::string&, uint16_t) {
        calls++;
        return true;
      }));
  EXPECT_EQ(calls, 0);
}

// --- Paragraph and list-item LUTs -----------------------------------------

TEST_F(SectionTest, GetPageForParagraphIndexAndInverse) {
  Section sec(kPath);
  Params p;
  ASSERT_TRUE(sec.openForWrite());
  writeHeaderFrom(sec, p);
  std::vector<PageLutEntry> lut;
  for (uint16_t i = 0; i < 3; i++) {
    const uint32_t off = sec.writePage(makePage(i));
    ASSERT_NE(off, 0u);
    // Paragraphs per page (ascending): 0, 5, 12. Each entry stores the
    // paragraph index at the start of that page.
    const uint16_t pIdx = (i == 0) ? 0 : (i == 1) ? 5 : 12;
    lut.push_back({off, pIdx, /*listItemIndex=*/0});
  }
  ASSERT_TRUE(sec.finalize(lut, /*anchors=*/{}));

  Section reader(kPath);

  // Inverse: paragraph index per page.
  EXPECT_EQ(reader.getParagraphIndexForPage(0), std::optional<uint16_t>{0});
  EXPECT_EQ(reader.getParagraphIndexForPage(1), std::optional<uint16_t>{5});
  EXPECT_EQ(reader.getParagraphIndexForPage(2), std::optional<uint16_t>{12});
  EXPECT_EQ(reader.getParagraphIndexForPage(3), std::nullopt);  // out of range

  // Forward: page containing the given paragraph index. The implementation
  // returns the first page whose start paragraph >= target. Page 1 starts at
  // paragraph 5, so paragraph 3 returns 1; paragraph 5 also returns 1.
  EXPECT_EQ(reader.getPageForParagraphIndex(0), std::optional<uint16_t>{0});
  EXPECT_EQ(reader.getPageForParagraphIndex(3), std::optional<uint16_t>{1});
  EXPECT_EQ(reader.getPageForParagraphIndex(5), std::optional<uint16_t>{1});
  EXPECT_EQ(reader.getPageForParagraphIndex(11), std::optional<uint16_t>{2});
  // Past the last LUT entry: implementation clamps to the final page index.
  EXPECT_EQ(reader.getPageForParagraphIndex(99), std::optional<uint16_t>{2});
}

TEST_F(SectionTest, GetPageForListItemIndex) {
  Section sec(kPath);
  Params p;
  ASSERT_TRUE(sec.openForWrite());
  writeHeaderFrom(sec, p);
  std::vector<PageLutEntry> lut;
  for (uint16_t i = 0; i < 3; i++) {
    const uint32_t off = sec.writePage(makePage(i));
    const uint16_t li = (i == 0) ? 0 : (i == 1) ? 3 : 7;
    lut.push_back({off, /*paragraphIndex=*/0, li});
  }
  ASSERT_TRUE(sec.finalize(lut, /*anchors=*/{}));

  Section reader(kPath);
  EXPECT_EQ(reader.getPageForListItemIndex(0), std::optional<uint16_t>{0});
  EXPECT_EQ(reader.getPageForListItemIndex(2), std::optional<uint16_t>{1});
  EXPECT_EQ(reader.getPageForListItemIndex(3), std::optional<uint16_t>{1});
  EXPECT_EQ(reader.getPageForListItemIndex(7), std::optional<uint16_t>{2});
  EXPECT_EQ(reader.getPageForListItemIndex(99), std::optional<uint16_t>{2});
}

// --- clearCache -----------------------------------------------------------

TEST_F(SectionTest, ClearCacheRemovesFile) {
  Section sec(kPath);
  Params p;
  ASSERT_TRUE(sec.openForWrite());
  writeHeaderFrom(sec, p);
  ASSERT_TRUE(sec.finalize({}, {}));
  ASSERT_TRUE(halStorage.exists(kPath));

  EXPECT_TRUE(sec.clearCache());
  EXPECT_FALSE(halStorage.exists(kPath));

  // Idempotent: second call on already-gone file is still success.
  EXPECT_TRUE(sec.clearCache());
}

}  // namespace
