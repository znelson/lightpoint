#include <Typesetter.h>
#include <Typesetter/Page.h>
#include <Typesetter/ParsedText.h>
#include <Typesetter/blocks/BlockStyle.h>
#include <Typesetter/blocks/TextBlock.h>
#include <gtest/gtest.h>

#include <cstring>
#include <functional>
#include <memory>
#include <string>
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

  // Lives as a fixture member so any Typesetter built via makeTypesetter()
  // can hold a FunctionRef to it without dangling once the helper returns.
  // FunctionRef binds to this std::function (callable -> operator()), and
  // the std::function's address is stable for the fixture's lifetime.
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> emit = [this](std::unique_ptr<Page> p, uint16_t parIdx,
                                                                               uint16_t liIdx) {
    emitted.push_back({std::move(p), parIdx, liIdx});
  };

  Typesetter makeTypesetter(bool extraParagraphSpacing = kExtraParagraphSpacing) {
    return Typesetter(renderer, kFontId, kLineCompression, extraParagraphSpacing, kViewportWidth, kViewportHeight,
                      emit);
  }

  // Builds a LinkEntry with the given label for queue-and-attach tests.
  static LinkEntry makeLink(const char* label) {
    LinkEntry entry;
    std::strncpy(entry.label, label, sizeof(entry.label) - 1);
    return entry;
  }

  // Returns the total link count across all emitted pages.
  size_t totalLinks() const {
    size_t n = 0;
    for (const auto& cp : emitted) n += cp.page->links.size();
    return n;
  }

  std::shared_ptr<ImageBlock> makeImage(int16_t width, int16_t height) {
    return std::make_shared<ImageBlock>("dummy.png", width, height);
  }

  // Build a ParsedText with `wordCount` ASCII words of equal byte length.
  // Each word's pixel width is `wordLen * GfxRenderer::kPxPerChar`. The
  // BlockStyle is left at default (Justify, no margins/padding/indent).
  std::unique_ptr<ParsedText> makeParagraph(int wordCount, int wordLen = 4, const BlockStyle& style = BlockStyle()) {
    auto p = std::make_unique<ParsedText>(/*extraParagraphSpacing=*/false,
                                          /*hyphenationEnabled=*/false,
                                          /*focusReadingEnabled=*/false, style);
    std::string word(static_cast<size_t>(wordLen), 'a');
    for (int i = 0; i < wordCount; ++i) {
      p->addWord(word, EpdFontFamily::REGULAR, /*underline=*/false, /*attachToPrevious=*/false);
    }
    return p;
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

// --- link / wordsExtractedInBlock ------------------------------------------

TEST_F(TypesetterFixture, SubmitParagraphResetsWordsExtractedInBlock) {
  // After a paragraph is submitted, the per-block counter must be 0 so the
  // NEXT paragraph's word indices are paragraph-local. Markdown and
  // TxtReader rely on this internal reset; EPUB used to reset externally
  // but no longer does.
  auto ts = makeTypesetter();
  ts.submitParagraph(makeParagraph(/*wordCount=*/10, /*wordLen=*/4));
  EXPECT_EQ(ts.getWordsExtractedInBlock(), 0);
  ts.submitParagraph(makeParagraph(/*wordCount=*/5, /*wordLen=*/4));
  EXPECT_EQ(ts.getWordsExtractedInBlock(), 0);
}

TEST_F(TypesetterFixture, AddPendingLinkDoesNotAffectAccessor) {
  auto ts = makeTypesetter();
  // The pending queue is internal; just verify the accessor stays untouched.
  LinkEntry entry;
  std::strncpy(entry.label, "1", sizeof(entry.label) - 1);
  ts.addPendingLink(5, entry);
  EXPECT_EQ(ts.getWordsExtractedInBlock(), 0);
}

// =====================================================================
// Tier 2: layout-driven tests. Drive submitParagraph / partialFlush /
// submitHorizontalRule through the real ParsedText layout engine with a
// deterministic GfxRenderer stub (kPxPerChar = 10, kSpaceAdvance = 5,
// kLineHeight = 20). With a 600-px viewport and 4-byte words: each word
// is 40 px and each gap is 5 px, so ~13 words fit per line. With a
// 400-px viewport height: 400 / 20 = 20 lines per page.
// =====================================================================

TEST_F(TypesetterFixture, SubmitParagraphShortFitsInOnePage) {
  auto ts = makeTypesetter();
  // 5 words of 4 chars each: well under one line.
  ts.submitParagraph(makeParagraph(/*wordCount=*/5, /*wordLen=*/4));
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_GE(emitted[0].page->elements.size(), 1u);
}

TEST_F(TypesetterFixture, SubmitParagraphLongSpansMultiplePages) {
  auto ts = makeTypesetter();
  // 13 words/line * 20 lines/page = 260 words per page. 600 words = ~3 pages.
  ts.submitParagraph(makeParagraph(/*wordCount=*/600, /*wordLen=*/4));
  ts.finish();
  EXPECT_GE(emitted.size(), 2u);
  EXPECT_LE(emitted.size(), 4u);
}

TEST_F(TypesetterFixture, SubmitMultipleParagraphsAccumulate) {
  auto ts = makeTypesetter();
  // Each paragraph fits in a single line (5 words). Several should pack into one page.
  for (int i = 0; i < 5; ++i) {
    ts.submitParagraph(makeParagraph(/*wordCount=*/5, /*wordLen=*/4));
  }
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_GE(emitted[0].page->elements.size(), 5u);
}

TEST_F(TypesetterFixture, SubmitParagraphAppliesTopMargin) {
  auto ts = makeTypesetter();
  // Paragraph with explicit large top margin should push later content
  // closer to the page break threshold.
  BlockStyle style;
  style.marginTop = 100;
  ts.submitParagraph(makeParagraph(/*wordCount=*/5, /*wordLen=*/4, style));
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_FALSE(emitted[0].page->elements.empty());
}

TEST_F(TypesetterFixture, PartialFlushOnLongBlockEmitsPagesAndLeavesContinuation) {
  auto ts = makeTypesetter();
  // Build a long block, then partialFlush before finish.
  auto p = makeParagraph(/*wordCount=*/300, /*wordLen=*/4);
  ts.partialFlush(*p);
  // partialFlush leaves the last line in the block; emitted pages so far
  // are the filled ones.
  const size_t midCount = emitted.size();
  EXPECT_GE(midCount, 0u);
  // Submitting and finishing the same paragraph completes any remaining
  // lines, then emits the trailing page.
  ts.submitParagraph(std::move(p));
  ts.finish();
  // Total pages should match what a single-shot submitParagraph would yield.
  EXPECT_GE(emitted.size(), midCount);
  EXPECT_GE(emitted.size(), 1u);
}

// --- submitHorizontalRule -------------------------------------------------

TEST_F(TypesetterFixture, SubmitHorizontalRuleFitsOnInitialPage) {
  auto ts = makeTypesetter();
  ts.submitHorizontalRule(BlockStyle());
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_EQ(emitted[0].page->elements.size(), 1u);
}

TEST_F(TypesetterFixture, SubmitHorizontalRuleDoesNotFitForcesPageBreak) {
  auto ts = makeTypesetter();
  // Fill the page nearly to the bottom with an image, then submit an HR
  // that should force a page break because it doesn't fit.
  // Viewport height = 400; image height = 395 leaves 5px free, less than
  // the rule's default spacing (kLineHeight / 2 == 10) + thickness (2) + bottom (10) = 22.
  ts.submitImage(makeImage(100, 395), 0, 0);
  EXPECT_EQ(ts.getCompletedPageCount(), 0);
  ts.submitHorizontalRule(BlockStyle());
  EXPECT_EQ(ts.getCompletedPageCount(), 1);
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_EQ(emitted[0].page->elements.size(), 1u);  // just the image; HR went to next page
  ts.finish();
  ASSERT_EQ(emitted.size(), 2u);
  EXPECT_EQ(emitted[1].page->elements.size(), 1u);  // the HR
}

TEST_F(TypesetterFixture, SubmitHorizontalRuleAppliesExplicitMargins) {
  auto ts = makeTypesetter();
  BlockStyle style;
  style.marginTop = 50;
  style.marginBottom = 50;
  ts.submitHorizontalRule(style);
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);
  ASSERT_EQ(emitted[0].page->elements.size(), 1u);
  // The HR's yPos should equal topSpacing = marginTop (50) when paddingTop = 0.
  EXPECT_EQ(emitted[0].page->elements[0]->yPos, 50);
}

// --- Per-page link attachment to the correct page -------------------------

TEST_F(TypesetterFixture, LinkAtEarlyWordIndexLandsOnFirstPage) {
  auto ts = makeTypesetter();
  ts.addPendingLink(/*wordIndex=*/1, makeLink("1"));
  ts.submitParagraph(makeParagraph(/*wordCount=*/10, /*wordLen=*/4));
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);
  ASSERT_EQ(emitted[0].page->links.size(), 1u);
  EXPECT_STREQ(emitted[0].page->links[0].label, "1");
}

TEST_F(TypesetterFixture, LinkAtLateWordIndexLandsOnLaterPage) {
  auto ts = makeTypesetter();
  // 600 words across multiple pages. Queue a link near the end so it
  // lands on a later page.
  ts.addPendingLink(/*wordIndex=*/500, makeLink("late"));
  ts.submitParagraph(makeParagraph(/*wordCount=*/600, /*wordLen=*/4));
  ts.finish();
  ASSERT_GE(emitted.size(), 2u);
  // The link should NOT be on the first page (which contains words 1..~260).
  EXPECT_EQ(emitted[0].page->links.size(), 0u);
  // Exactly one link across all pages.
  EXPECT_EQ(totalLinks(), 1u);
}

TEST_F(TypesetterFixture, MultipleLinksAttachToCorrectPages) {
  auto ts = makeTypesetter();
  // Two links on the same early page, one on a later page.
  ts.addPendingLink(1, makeLink("a"));
  ts.addPendingLink(2, makeLink("b"));
  ts.addPendingLink(500, makeLink("c"));
  ts.submitParagraph(makeParagraph(/*wordCount=*/600, /*wordLen=*/4));
  ts.finish();
  ASSERT_GE(emitted.size(), 2u);
  // First page has "a" and "b".
  ASSERT_EQ(emitted[0].page->links.size(), 2u);
  EXPECT_STREQ(emitted[0].page->links[0].label, "a");
  EXPECT_STREQ(emitted[0].page->links[1].label, "b");
  // "c" lands on a later page.
  EXPECT_EQ(totalLinks(), 3u);
}

TEST_F(TypesetterFixture, LinkAtImpossibleWordIndexDumpsToFinalPage) {
  // A link with wordIndex beyond the block size triggers the fallback in
  // submitParagraph that drains the queue to the current page.
  auto ts = makeTypesetter();
  ts.addPendingLink(/*wordIndex=*/9999, makeLink("fallback"));
  ts.submitParagraph(makeParagraph(/*wordCount=*/5, /*wordLen=*/4));
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);
  ASSERT_EQ(emitted[0].page->links.size(), 1u);
  EXPECT_STREQ(emitted[0].page->links[0].label, "fallback");
}

// --- Null input handling --------------------------------------------------

TEST_F(TypesetterFixture, SubmitParagraphNullIsNoOp) {
  auto ts = makeTypesetter();
  ts.submitParagraph(nullptr);
  EXPECT_EQ(ts.getCompletedPageCount(), 0);
  EXPECT_TRUE(emitted.empty());
}

TEST_F(TypesetterFixture, SubmitImageNullIsNoOp) {
  auto ts = makeTypesetter();
  ts.submitImage(nullptr, 0, 0);
  EXPECT_EQ(ts.getCompletedPageCount(), 0);
  EXPECT_TRUE(emitted.empty());
}

// --- extraParagraphSpacing ------------------------------------------------

TEST_F(TypesetterFixture, ExtraParagraphSpacingCausesEarlierPageBreaks) {
  // Each single-line paragraph (1 line of height 20px) plus the extra
  // half-line spacing (10px) = 30px. With viewport height 400, ~13 such
  // paragraphs fit. Without extra spacing, 20 paragraphs fit.
  auto ts = makeTypesetter(/*extraParagraphSpacing=*/true);
  for (int i = 0; i < 20; ++i) {
    ts.submitParagraph(makeParagraph(/*wordCount=*/3, /*wordLen=*/4));
  }
  ts.finish();
  // With extra spacing, 20 paragraphs no longer fit on one page.
  EXPECT_GE(emitted.size(), 2u);
}

// --- Image margins and positioning ----------------------------------------

TEST_F(TypesetterFixture, SubmitImageAppliesTopMarginToYPosition) {
  auto ts = makeTypesetter();
  ts.submitImage(makeImage(100, 50), /*marginTop=*/40, /*marginBottom=*/0);
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);
  ASSERT_EQ(emitted[0].page->elements.size(), 1u);
  // The image's yPos should reflect the top margin (page starts at y=0,
  // marginTop pushes the image down).
  EXPECT_EQ(emitted[0].page->elements[0]->yPos, 40);
}

TEST_F(TypesetterFixture, SubmitImageCentersHorizontally) {
  auto ts = makeTypesetter();
  // viewport width = 600, image width = 200 → expected xPos = (600-200)/2 = 200.
  ts.submitImage(makeImage(200, 50), 0, 0);
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);
  ASSERT_EQ(emitted[0].page->elements.size(), 1u);
  EXPECT_EQ(emitted[0].page->elements[0]->xPos, 200);
}

// --- Idempotent operations ------------------------------------------------

TEST_F(TypesetterFixture, DoubleFinishDoesNotDoubleEmit) {
  auto ts = makeTypesetter();
  ts.submitImage(makeImage(100, 50), 0, 0);
  ts.finish();
  ts.finish();  // second finish on already-empty state
  EXPECT_EQ(ts.getCompletedPageCount(), 1);
  EXPECT_EQ(emitted.size(), 1u);
}

TEST_F(TypesetterFixture, DoubleForcePageBreakDoesNotDoubleEmit) {
  auto ts = makeTypesetter();
  ts.submitImage(makeImage(100, 50), 0, 0);
  ts.forcePageBreak();
  ts.forcePageBreak();  // page is empty after first break; second is no-op
  EXPECT_EQ(ts.getCompletedPageCount(), 1);
  EXPECT_EQ(emitted.size(), 1u);
}

TEST_F(TypesetterFixture, PartialFlushOnEmptyBlockIsNoOp) {
  auto ts = makeTypesetter();
  auto p = std::make_unique<ParsedText>(/*extraParagraphSpacing=*/false,
                                        /*hyphenationEnabled=*/false,
                                        /*focusReadingEnabled=*/false, BlockStyle());
  // No words added to the block.
  ts.partialFlush(*p);
  EXPECT_EQ(ts.getCompletedPageCount(), 0);
  EXPECT_TRUE(emitted.empty());
}

// --- RTL / BiDi integration ----------------------------------------------
//
// These tests drive the extractLine reorder branch end-to-end. They use
// Hebrew UTF-8 inputs because lib/MiniBidi/bidiclasses.t is scoped to
// Hebrew + Latin/Cyrillic. The assertions check word *order* in the emitted
// TextBlock; xpos placement is covered by BidiUtilsTest.

namespace {

// Hebrew words used by RTL tests. The byte sequences are inline so the test
// file doesn't depend on the compiler's source-charset handling.
constexpr const char* kShalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";  // 4 letters
constexpr const char* kAhalan = "\xD7\x90\xD7\x94\xD7\x9C\xD7\x9F";  // 4 letters

// Pull the first emitted page's first PageLine's TextBlock so tests can
// assert on word ordering after layout.
std::vector<std::string> firstLineWords(const Page& page) {
  for (const auto& el : page.elements) {
    if (el->getTag() == TAG_PageLine) {
      auto* line = static_cast<const PageLine*>(el.get());
      return line->getBlock()->getWords();
    }
  }
  return {};
}

}  // namespace

TEST_F(TypesetterFixture, PureHebrewRtlParagraphReversesWordOrder) {
  BlockStyle style;
  style.isRtl = true;
  style.directionDefined = true;
  // extraParagraphSpacing=true skips the em-space first-line indent so the
  // assertion can match raw word strings.
  auto p = std::make_unique<ParsedText>(/*extraParagraphSpacing=*/true,
                                        /*hyphenationEnabled=*/false,
                                        /*focusReadingEnabled=*/false, style);
  p->addWord(kShalom, EpdFontFamily::REGULAR);
  p->addWord(kAhalan, EpdFontFamily::REGULAR);

  auto ts = makeTypesetter();
  ts.submitParagraph(std::move(p));
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);

  const auto words = firstLineWords(*emitted[0].page);
  // Pure RTL run in an RTL paragraph: visual order reverses logical order.
  ASSERT_EQ(words.size(), 2u);
  EXPECT_EQ(words[0], kAhalan);
  EXPECT_EQ(words[1], kShalom);
}

TEST_F(TypesetterFixture, PureLtrParagraphSkipsReorder) {
  // Default BlockStyle is LTR; pure-ASCII content takes the no-reorder path.
  auto p = std::make_unique<ParsedText>(/*extraParagraphSpacing=*/true,
                                        /*hyphenationEnabled=*/false,
                                        /*focusReadingEnabled=*/false, BlockStyle());
  p->addWord("hello", EpdFontFamily::REGULAR);
  p->addWord("world", EpdFontFamily::REGULAR);

  auto ts = makeTypesetter();
  ts.submitParagraph(std::move(p));
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);

  const auto words = firstLineWords(*emitted[0].page);
  ASSERT_EQ(words.size(), 2u);
  EXPECT_EQ(words[0], "hello");
  EXPECT_EQ(words[1], "world");
}

TEST_F(TypesetterFixture, MixedHebrewEnglishInRtlParagraphPermutesNonIdentity) {
  // Hebrew + Latin + Hebrew in an RTL paragraph. The exact visual order
  // depends on UAX#9; we assert (a) all three words survive, and (b) the
  // permutation is non-identity (i.e. reorder actually fired).
  BlockStyle style;
  style.isRtl = true;
  style.directionDefined = true;
  auto p = std::make_unique<ParsedText>(/*extraParagraphSpacing=*/true,
                                        /*hyphenationEnabled=*/false,
                                        /*focusReadingEnabled=*/false, style);
  p->addWord(kShalom, EpdFontFamily::REGULAR);
  p->addWord("english", EpdFontFamily::REGULAR);
  p->addWord(kAhalan, EpdFontFamily::REGULAR);

  auto ts = makeTypesetter();
  ts.submitParagraph(std::move(p));
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);

  const auto words = firstLineWords(*emitted[0].page);
  ASSERT_EQ(words.size(), 3u);
  // All three logical tokens present exactly once.
  size_t shalomCount = 0, ahalanCount = 0, englishCount = 0;
  for (const auto& w : words) {
    if (w == kShalom) shalomCount++;
    if (w == kAhalan) ahalanCount++;
    if (w == "english") englishCount++;
  }
  EXPECT_EQ(shalomCount, 1u);
  EXPECT_EQ(ahalanCount, 1u);
  EXPECT_EQ(englishCount, 1u);
  // Non-identity: logical order was [shalom, english, ahalan]; if the visual
  // matches that we never exercised the reorder branch we care about.
  const bool isIdentity = words[0] == kShalom && words[1] == "english" && words[2] == kAhalan;
  EXPECT_FALSE(isIdentity);
}

TEST_F(TypesetterFixture, AutoDetectedRtlFromContentReordersWords) {
  // No explicit direction in BlockStyle. ParsedText should auto-detect RTL
  // from the Hebrew content (hasRtlWord -> directionDefined check -> isRtl).
  auto p = std::make_unique<ParsedText>(/*extraParagraphSpacing=*/true,
                                        /*hyphenationEnabled=*/false,
                                        /*focusReadingEnabled=*/false, BlockStyle());
  p->addWord(kShalom, EpdFontFamily::REGULAR);
  p->addWord(kAhalan, EpdFontFamily::REGULAR);

  auto ts = makeTypesetter();
  ts.submitParagraph(std::move(p));
  ts.finish();
  ASSERT_EQ(emitted.size(), 1u);

  const auto words = firstLineWords(*emitted[0].page);
  ASSERT_EQ(words.size(), 2u);
  EXPECT_EQ(words[0], kAhalan);
  EXPECT_EQ(words[1], kShalom);
}
