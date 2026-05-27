#include <Typesetter.h>
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "GfxRenderer.h"  // test-only stub

namespace {

// Captures pages emitted by Typesetter via the completePageFn callback so
// tests can assert on emission count, ordering, and xpath indices.
struct CapturedPage {
  std::unique_ptr<Page> page;
  uint16_t xpathParagraphIndex;
  uint16_t xpathListItemIndex;
};

class TypesetterFixture : public ::testing::Test {
 protected:
  static constexpr int kFontId = 1;
  static constexpr float kLineCompression = 1.0f;
  static constexpr bool kExtraParagraphSpacing = false;
  static constexpr uint16_t kViewportWidth = 600;
  static constexpr uint16_t kViewportHeight = 400;

  GfxRenderer renderer;
  std::vector<CapturedPage> emitted;

  Typesetter makeTypesetter() {
    return Typesetter(renderer, kFontId, kLineCompression, kExtraParagraphSpacing, kViewportWidth, kViewportHeight,
                      [this](std::unique_ptr<Page> p, uint16_t parIdx, uint16_t liIdx) {
                        emitted.push_back({std::move(p), parIdx, liIdx});
                      });
  }

  std::shared_ptr<ImageBlock> makeImage(int16_t width, int16_t height) {
    return std::make_shared<ImageBlock>("dummy.png", width, height);
  }
};

}  // namespace

// --- Default state --------------------------------------------------------

TEST_F(TypesetterFixture, ConstructorInitializesState) {
  auto ts = makeTypesetter();
  EXPECT_EQ(ts.getCompletedPageCount(), 0);
  EXPECT_EQ(ts.getWordsExtractedInBlock(), 0);
  EXPECT_TRUE(emitted.empty());
}

// --- forcePageBreak -------------------------------------------------------

TEST_F(TypesetterFixture, ForcePageBreakOnEmptyStateIsNoOp) {
  auto ts = makeTypesetter();
  ts.forcePageBreak();
  EXPECT_EQ(ts.getCompletedPageCount(), 0);
  EXPECT_TRUE(emitted.empty());
}

TEST_F(TypesetterFixture, ForcePageBreakWithContentEmitsAndIncrements) {
  auto ts = makeTypesetter();
  ts.submitImage(makeImage(100, 50), 0, 0);
  ts.forcePageBreak();
  EXPECT_EQ(ts.getCompletedPageCount(), 1);
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_FALSE(emitted[0].page->elements.empty());
}

// --- finish ---------------------------------------------------------------

TEST_F(TypesetterFixture, FinishOnEmptyStateDoesNotEmit) {
  auto ts = makeTypesetter();
  ts.finish();
  EXPECT_EQ(ts.getCompletedPageCount(), 0);
  EXPECT_TRUE(emitted.empty());
}

TEST_F(TypesetterFixture, FinishAfterImageEmitsFinalPage) {
  auto ts = makeTypesetter();
  ts.submitImage(makeImage(100, 50), 0, 0);
  ts.finish();
  EXPECT_EQ(ts.getCompletedPageCount(), 1);
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_FALSE(emitted[0].page->elements.empty());
}

// --- submitImage page-fit behavior ----------------------------------------

TEST_F(TypesetterFixture, SubmitImageFitsOnSameInitialPage) {
  auto ts = makeTypesetter();
  // Two images, both small enough to fit on one page.
  ts.submitImage(makeImage(100, 50), 0, 0);
  ts.submitImage(makeImage(100, 50), 0, 0);
  // No page break expected yet.
  EXPECT_EQ(ts.getCompletedPageCount(), 0);
  EXPECT_TRUE(emitted.empty());

  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_EQ(emitted[0].page->elements.size(), 2u);
}

TEST_F(TypesetterFixture, SubmitImageDoesNotFitForcesPageBreakFirst) {
  auto ts = makeTypesetter();
  // First image: tall enough to leave less than the second image's height of free space.
  // Viewport height is 400. First image is 300 tall, second is 200 tall.
  // 300 + 200 = 500 > 400, so the second forces a break.
  ts.submitImage(makeImage(100, 300), 0, 0);
  EXPECT_EQ(ts.getCompletedPageCount(), 0);
  ts.submitImage(makeImage(100, 200), 0, 0);
  EXPECT_EQ(ts.getCompletedPageCount(), 1);
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_EQ(emitted[0].page->elements.size(), 1u);  // only the first image
}

TEST_F(TypesetterFixture, SubmitImageOnEmptyStateCreatesInitialPage) {
  auto ts = makeTypesetter();
  ts.submitImage(makeImage(100, 50), 0, 0);
  EXPECT_EQ(ts.getCompletedPageCount(), 0);  // page in-progress, not yet emitted
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_EQ(emitted[0].page->elements.size(), 1u);
}

// --- xpath index propagation ----------------------------------------------

TEST_F(TypesetterFixture, XpathIncrementsPropagateToEmittedPage) {
  auto ts = makeTypesetter();
  ts.incrementXpathParagraphIndex();
  ts.incrementXpathParagraphIndex();
  ts.incrementXpathListItemIndex();
  ts.submitImage(makeImage(100, 50), 0, 0);
  ts.finish();

  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_EQ(emitted[0].xpathParagraphIndex, 2);
  EXPECT_EQ(emitted[0].xpathListItemIndex, 1);
}

TEST_F(TypesetterFixture, XpathIncrementsAccumulateAcrossPages) {
  auto ts = makeTypesetter();
  // First page: 2 paragraphs in, then emit.
  ts.incrementXpathParagraphIndex();
  ts.incrementXpathParagraphIndex();
  ts.submitImage(makeImage(100, 50), 0, 0);
  ts.forcePageBreak();
  // Second page: 3 more paragraphs (total xpathParagraphIndex==5).
  ts.incrementXpathParagraphIndex();
  ts.incrementXpathParagraphIndex();
  ts.incrementXpathParagraphIndex();
  ts.submitImage(makeImage(100, 50), 0, 0);
  ts.finish();

  ASSERT_EQ(emitted.size(), 2u);
  EXPECT_EQ(emitted[0].xpathParagraphIndex, 2);
  EXPECT_EQ(emitted[1].xpathParagraphIndex, 5);
}

// --- footnote / wordsExtractedInBlock --------------------------------------

TEST_F(TypesetterFixture, WordsExtractedInBlockResetWorks) {
  auto ts = makeTypesetter();
  EXPECT_EQ(ts.getWordsExtractedInBlock(), 0);
  ts.resetWordsExtractedInBlock();
  EXPECT_EQ(ts.getWordsExtractedInBlock(), 0);
  // No public API to set wordsExtracted directly; submitParagraph would but
  // that is Tier 2. We verify resetWordsExtractedInBlock leaves it at 0.
}

TEST_F(TypesetterFixture, AddPendingFootnoteDoesNotAffectAccessor) {
  auto ts = makeTypesetter();
  // The pending queue is internal; just verify the accessor stays untouched.
  // addPendingFootnote affects state consumed by submitParagraph (Tier 2).
  FootnoteEntry entry;
  std::strncpy(entry.number, "1", sizeof(entry.number) - 1);
  ts.addPendingFootnote(5, entry);
  EXPECT_EQ(ts.getWordsExtractedInBlock(), 0);
}
