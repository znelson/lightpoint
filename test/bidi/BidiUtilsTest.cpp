#include <BidiUtils.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "minibidi.h"
}

namespace {

// UTF-8 Hebrew literals (U+05D0..U+05EA, 2 bytes each). lib/MiniBidi/bidiclasses.t
// is intentionally scoped to Hebrew + Latin/Cyrillic — Arabic, CJK, etc. fall
// through to ON (neutral) per the firmware's "no fonts for those scripts" stance.
constexpr const char* kShalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";  // shin lamed vav mem-final
constexpr const char* kAhalan = "\xD7\x90\xD7\x94\xD7\x9C\xD7\x9F";  // alef he lamed nun-final
constexpr const char* kArabicAlef = "\xD8\xA3";  // U+0623 ALEF WITH HAMZA

}  // namespace

// --- startsWithRtl --------------------------------------------------------

TEST(BidiUtilsStartsWithRtl, NullInputIsFalse) {
  EXPECT_FALSE(BidiUtils::startsWithRtl(nullptr));
}

TEST(BidiUtilsStartsWithRtl, EmptyStringIsFalse) {
  EXPECT_FALSE(BidiUtils::startsWithRtl(""));
}

TEST(BidiUtilsStartsWithRtl, PureAsciiIsFalse) {
  EXPECT_FALSE(BidiUtils::startsWithRtl("hello world"));
}

TEST(BidiUtilsStartsWithRtl, LeadingHebrewIsTrue) {
  EXPECT_TRUE(BidiUtils::startsWithRtl(kShalom));
}

TEST(BidiUtilsStartsWithRtl, ArabicIsTreatedAsNeutral) {
  // Documents scope: lib/MiniBidi/bidiclasses.t omits Arabic by design (no
  // Arabic glyphs in the bundled fonts), so U+0623 falls through to ON and
  // is NOT classified as RTL. If Arabic support is ever added, this test
  // will start failing as a signal to revisit RTL handling end-to-end.
  EXPECT_FALSE(BidiUtils::startsWithRtl(kArabicAlef));
}

TEST(BidiUtilsStartsWithRtl, LeadingNeutralThenHebrewIsTrue) {
  // Leading neutrals (quote, paren) shouldn't fool the probe.
  const std::string s = std::string("\"") + kShalom;
  EXPECT_TRUE(BidiUtils::startsWithRtl(s.c_str()));
}

TEST(BidiUtilsStartsWithRtl, LeadingLatinThenHebrewIsFalse) {
  // First strong char is L; verdict is LTR even if Hebrew follows.
  const std::string s = std::string("a") + kShalom;
  EXPECT_FALSE(BidiUtils::startsWithRtl(s.c_str()));
}

// --- detectParagraphLevel -------------------------------------------------

TEST(BidiUtilsDetectParagraphLevel, NullReturnsFallback) {
  EXPECT_EQ(BidiUtils::detectParagraphLevel(nullptr, /*fallbackLevel=*/0), 0);
  EXPECT_EQ(BidiUtils::detectParagraphLevel(nullptr, /*fallbackLevel=*/1), 1);
}

TEST(BidiUtilsDetectParagraphLevel, AsciiIsLevel0) {
  EXPECT_EQ(BidiUtils::detectParagraphLevel("hello"), 0);
}

TEST(BidiUtilsDetectParagraphLevel, HebrewIsLevel1) {
  EXPECT_EQ(BidiUtils::detectParagraphLevel(kShalom), 1);
}

TEST(BidiUtilsDetectParagraphLevel, NeutralOnlyReturnsFallback) {
  // No strong chars at all -> fallback.
  EXPECT_EQ(BidiUtils::detectParagraphLevel("\"\".", /*fallbackLevel=*/1), 1);
  EXPECT_EQ(BidiUtils::detectParagraphLevel("\"\".", /*fallbackLevel=*/0), 0);
}

// --- computeVisualWordOrder -----------------------------------------------

TEST(BidiUtilsComputeVisualWordOrder, EmptyReturnsFalse) {
  std::vector<std::string> words;
  std::vector<uint16_t> order = {99};  // pre-populated to assert it gets cleared
  EXPECT_FALSE(BidiUtils::computeVisualWordOrder(words, /*paragraphIsRtl=*/false, order));
  EXPECT_TRUE(order.empty());
}

TEST(BidiUtilsComputeVisualWordOrder, SingleWordReturnsFalse) {
  std::vector<std::string> words = {"hello"};
  std::vector<uint16_t> order;
  EXPECT_FALSE(BidiUtils::computeVisualWordOrder(words, /*paragraphIsRtl=*/false, order));
}

TEST(BidiUtilsComputeVisualWordOrder, PureLtrInLtrParagraphReturnsFalse) {
  // No reordering needed -> false (caller takes the standard LTR path).
  std::vector<std::string> words = {"the", "quick", "brown", "fox"};
  std::vector<uint16_t> order;
  EXPECT_FALSE(BidiUtils::computeVisualWordOrder(words, /*paragraphIsRtl=*/false, order));
}

TEST(BidiUtilsComputeVisualWordOrder, PureLtrInRtlParagraphReturnsIdentity) {
  // LTR words inside an RTL paragraph: identity order, but caller still uses
  // the willReorder (left-to-right) positioning path.
  std::vector<std::string> words = {"hello", "world"};
  std::vector<uint16_t> order;
  ASSERT_TRUE(BidiUtils::computeVisualWordOrder(words, /*paragraphIsRtl=*/true, order));
  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], 0u);
  EXPECT_EQ(order[1], 1u);
}

TEST(BidiUtilsComputeVisualWordOrder, PureRtlInRtlParagraphReversesWords) {
  std::vector<std::string> words = {kShalom, kAhalan};
  std::vector<uint16_t> order;
  ASSERT_TRUE(BidiUtils::computeVisualWordOrder(words, /*paragraphIsRtl=*/true, order));
  ASSERT_EQ(order.size(), 2u);
  // Visual order is reversed: second logical word appears first visually.
  EXPECT_EQ(order[0], 1u);
  EXPECT_EQ(order[1], 0u);
}

TEST(BidiUtilsComputeVisualWordOrder, MixedRunsProduceNonIdentityOrder) {
  // Hebrew Latin Hebrew in an RTL paragraph: the L run preserves its internal
  // order but the Hebrew words flip relative to it.
  std::vector<std::string> words = {kShalom, "english", kAhalan};
  std::vector<uint16_t> order;
  ASSERT_TRUE(BidiUtils::computeVisualWordOrder(words, /*paragraphIsRtl=*/true, order));
  ASSERT_EQ(order.size(), 3u);
  // Not the identity permutation.
  const bool isIdentity = order[0] == 0 && order[1] == 1 && order[2] == 2;
  EXPECT_FALSE(isIdentity);
  // All indices present exactly once.
  std::vector<bool> seen(3, false);
  for (uint16_t idx : order) {
    ASSERT_LT(idx, 3u);
    EXPECT_FALSE(seen[idx]) << "Index " << idx << " appears twice";
    seen[idx] = true;
  }
}

TEST(BidiUtilsComputeVisualWordOrder, OverflowReturnsFalse) {
  // nWords > BIDI_MAX_LINE -> false (no reordering attempted).
  std::vector<std::string> words(BIDI_MAX_LINE + 1, "a");
  std::vector<uint16_t> order;
  EXPECT_FALSE(BidiUtils::computeVisualWordOrder(words, /*paragraphIsRtl=*/true, order));
}

// --- applyOrderInPlace (cycle decomposition) ------------------------------

TEST(BidiUtilsApplyOrderInPlace, EmptyIsNoOp) {
  std::vector<int> data;
  std::vector<uint16_t> order;
  BidiUtils::applyOrderInPlace(data, order);
  EXPECT_TRUE(data.empty());
}

TEST(BidiUtilsApplyOrderInPlace, IdentityLeavesDataUnchanged) {
  std::vector<int> data = {10, 20, 30, 40};
  std::vector<uint16_t> order = {0, 1, 2, 3};
  BidiUtils::applyOrderInPlace(data, order);
  EXPECT_EQ(data, (std::vector<int>{10, 20, 30, 40}));
}

TEST(BidiUtilsApplyOrderInPlace, SimpleSwap) {
  std::vector<int> data = {10, 20};
  std::vector<uint16_t> order = {1, 0};
  BidiUtils::applyOrderInPlace(data, order);
  EXPECT_EQ(data, (std::vector<int>{20, 10}));
}

TEST(BidiUtilsApplyOrderInPlace, ThreeCycle) {
  // Visual position 0 <- logical 2, 1 <- 0, 2 <- 1.
  std::vector<int> data = {10, 20, 30};
  std::vector<uint16_t> order = {2, 0, 1};
  BidiUtils::applyOrderInPlace(data, order);
  EXPECT_EQ(data, (std::vector<int>{30, 10, 20}));
}

TEST(BidiUtilsApplyOrderInPlace, TwoIndependentTwoCycles) {
  // Two disjoint swaps: (0<->1) and (2<->3).
  std::vector<int> data = {10, 20, 30, 40};
  std::vector<uint16_t> order = {1, 0, 3, 2};
  BidiUtils::applyOrderInPlace(data, order);
  EXPECT_EQ(data, (std::vector<int>{20, 10, 40, 30}));
}

TEST(BidiUtilsApplyOrderInPlace, ReverseAllElements) {
  std::vector<int> data = {10, 20, 30, 40, 50};
  std::vector<uint16_t> order = {4, 3, 2, 1, 0};
  BidiUtils::applyOrderInPlace(data, order);
  EXPECT_EQ(data, (std::vector<int>{50, 40, 30, 20, 10}));
}

TEST(BidiUtilsApplyOrderInPlace, WorksWithUint8Type) {
  // Mirrors production usage: EpdFontFamily::Style is uint8_t.
  std::vector<uint8_t> data = {1, 2, 3, 4};
  std::vector<uint16_t> order = {3, 2, 1, 0};
  BidiUtils::applyOrderInPlace(data, order);
  EXPECT_EQ(data, (std::vector<uint8_t>{4, 3, 2, 1}));
}
