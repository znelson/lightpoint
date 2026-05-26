#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "lib/Utf8/Utf8.h"

namespace {

std::string encode(uint32_t cp) {
  char buf[4];
  int n = utf8EncodeCodepoint(cp, buf);
  return std::string(buf, static_cast<size_t>(n));
}

int encodeLen(uint32_t cp) {
  char buf[4];
  return utf8EncodeCodepoint(cp, buf);
}

uint32_t decodeOne(const std::string& s) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.c_str());
  return utf8NextCodepoint(&p);
}

}  // namespace

// ---- utf8EncodeCodepoint -----------------------------------------------------

TEST(Utf8Encode, AsciiBoundaries) {
  // U+0000: NUL, smallest 1-byte codepoint.
  {
    char buf[4] = {1, 1, 1, 1};
    int n = utf8EncodeCodepoint(0x0000, buf);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0x00u);
  }
  // U+007F: largest 1-byte codepoint.
  {
    char buf[4];
    int n = utf8EncodeCodepoint(0x007F, buf);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0x7Fu);
  }
}

TEST(Utf8Encode, TwoByteBoundaries) {
  // U+0080: smallest 2-byte codepoint -> C2 80
  {
    char buf[4];
    int n = utf8EncodeCodepoint(0x0080, buf);
    ASSERT_EQ(n, 2);
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0xC2u);
    EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0x80u);
  }
  // U+00E9 (e-acute): C3 A9
  {
    auto s = encode(0x00E9);
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(s[0]), 0xC3u);
    EXPECT_EQ(static_cast<uint8_t>(s[1]), 0xA9u);
  }
  // U+07FF: largest 2-byte codepoint -> DF BF
  {
    char buf[4];
    int n = utf8EncodeCodepoint(0x07FF, buf);
    ASSERT_EQ(n, 2);
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0xDFu);
    EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0xBFu);
  }
}

TEST(Utf8Encode, ThreeByteBoundaries) {
  // U+0800: smallest 3-byte codepoint -> E0 A0 80
  {
    char buf[4];
    int n = utf8EncodeCodepoint(0x0800, buf);
    ASSERT_EQ(n, 3);
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0xE0u);
    EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0xA0u);
    EXPECT_EQ(static_cast<uint8_t>(buf[2]), 0x80u);
  }
  // U+20AC (euro sign): E2 82 AC
  {
    auto s = encode(0x20AC);
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(static_cast<uint8_t>(s[0]), 0xE2u);
    EXPECT_EQ(static_cast<uint8_t>(s[1]), 0x82u);
    EXPECT_EQ(static_cast<uint8_t>(s[2]), 0xACu);
  }
  // U+FFFD: replacement char -> EF BF BD
  {
    auto s = encode(0xFFFD);
    ASSERT_EQ(s.size(), 3u);
    EXPECT_EQ(static_cast<uint8_t>(s[0]), 0xEFu);
    EXPECT_EQ(static_cast<uint8_t>(s[1]), 0xBFu);
    EXPECT_EQ(static_cast<uint8_t>(s[2]), 0xBDu);
  }
  // U+FFFF: largest 3-byte codepoint -> EF BF BF
  {
    char buf[4];
    int n = utf8EncodeCodepoint(0xFFFF, buf);
    ASSERT_EQ(n, 3);
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0xEFu);
    EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0xBFu);
    EXPECT_EQ(static_cast<uint8_t>(buf[2]), 0xBFu);
  }
}

TEST(Utf8Encode, FourByteBoundaries) {
  // U+10000: smallest 4-byte codepoint -> F0 90 80 80
  {
    char buf[4];
    int n = utf8EncodeCodepoint(0x10000, buf);
    ASSERT_EQ(n, 4);
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0xF0u);
    EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0x90u);
    EXPECT_EQ(static_cast<uint8_t>(buf[2]), 0x80u);
    EXPECT_EQ(static_cast<uint8_t>(buf[3]), 0x80u);
  }
  // U+1F600 (grinning face emoji): F0 9F 98 80
  {
    auto s = encode(0x1F600);
    ASSERT_EQ(s.size(), 4u);
    EXPECT_EQ(static_cast<uint8_t>(s[0]), 0xF0u);
    EXPECT_EQ(static_cast<uint8_t>(s[1]), 0x9Fu);
    EXPECT_EQ(static_cast<uint8_t>(s[2]), 0x98u);
    EXPECT_EQ(static_cast<uint8_t>(s[3]), 0x80u);
  }
  // U+10FFFF: largest valid Unicode codepoint -> F4 8F BF BF
  {
    char buf[4];
    int n = utf8EncodeCodepoint(0x10FFFF, buf);
    ASSERT_EQ(n, 4);
    EXPECT_EQ(static_cast<uint8_t>(buf[0]), 0xF4u);
    EXPECT_EQ(static_cast<uint8_t>(buf[1]), 0x8Fu);
    EXPECT_EQ(static_cast<uint8_t>(buf[2]), 0xBFu);
    EXPECT_EQ(static_cast<uint8_t>(buf[3]), 0xBFu);
  }
}

TEST(Utf8Encode, InvalidProducesReplacement) {
  // Surrogate range U+D800-U+DFFF is invalid as a standalone codepoint.
  for (uint32_t cp : {0xD800u, 0xD801u, 0xDC00u, 0xDFFFu}) {
    auto s = encode(cp);
    EXPECT_EQ(s, encode(REPLACEMENT_GLYPH)) << "cp=" << std::hex << cp;
  }
  // Beyond U+10FFFF: out of Unicode range.
  for (uint32_t cp : {0x110000u, 0x200000u, 0xFFFFFFFFu}) {
    auto s = encode(cp);
    EXPECT_EQ(s, encode(REPLACEMENT_GLYPH)) << "cp=" << std::hex << cp;
  }
}

TEST(Utf8Encode, ByteCountMatchesLengthBuckets) {
  EXPECT_EQ(encodeLen(0x0000), 1);
  EXPECT_EQ(encodeLen(0x007F), 1);
  EXPECT_EQ(encodeLen(0x0080), 2);
  EXPECT_EQ(encodeLen(0x07FF), 2);
  EXPECT_EQ(encodeLen(0x0800), 3);
  EXPECT_EQ(encodeLen(0xFFFF), 3);
  EXPECT_EQ(encodeLen(0x10000), 4);
  EXPECT_EQ(encodeLen(0x10FFFF), 4);
  // Invalid -> U+FFFD is 3 bytes.
  EXPECT_EQ(encodeLen(0xD800), 3);
  EXPECT_EQ(encodeLen(0x110000), 3);
}

TEST(Utf8Encode, RoundTripsThroughDecoder) {
  // Every well-formed codepoint we encode should decode back to the same value
  // via utf8NextCodepoint, the existing decoder in this library.
  const uint32_t samples[] = {
      0x0001, 0x0041, 0x007F, 0x0080, 0x00E9, 0x07FF, 0x0800, 0x20AC, 0xFFFD, 0xFFFF, 0x10000, 0x1F600, 0x10FFFF,
  };
  for (uint32_t cp : samples) {
    auto s = encode(cp);
    EXPECT_EQ(decodeOne(s), cp) << "cp=" << std::hex << cp;
  }
}

// ---- utf8NextCodepoint -------------------------------------------------------

TEST(Utf8NextCodepoint, AsciiAdvancesByOne) {
  const unsigned char buf[] = "Az";
  const unsigned char* p = buf;
  EXPECT_EQ(utf8NextCodepoint(&p), 0x41u);
  EXPECT_EQ(p - buf, 1);
  EXPECT_EQ(utf8NextCodepoint(&p), 0x7Au);
  EXPECT_EQ(p - buf, 2);
}

TEST(Utf8NextCodepoint, NulTerminatorReturnsZero) {
  const unsigned char buf[] = {0x00};
  const unsigned char* p = buf;
  EXPECT_EQ(utf8NextCodepoint(&p), 0u);
  // Pointer is not advanced past a NUL.
  EXPECT_EQ(p, buf);
}

TEST(Utf8NextCodepoint, TwoByteSequence) {
  // U+00E9 (e-acute) = C3 A9
  const unsigned char buf[] = {0xC3, 0xA9, 0x00};
  const unsigned char* p = buf;
  EXPECT_EQ(utf8NextCodepoint(&p), 0x00E9u);
  EXPECT_EQ(p - buf, 2);
}

TEST(Utf8NextCodepoint, ThreeByteSequence) {
  // U+20AC (euro) = E2 82 AC
  const unsigned char buf[] = {0xE2, 0x82, 0xAC, 0x00};
  const unsigned char* p = buf;
  EXPECT_EQ(utf8NextCodepoint(&p), 0x20ACu);
  EXPECT_EQ(p - buf, 3);
}

TEST(Utf8NextCodepoint, FourByteSequence) {
  // U+1F600 (grinning face) = F0 9F 98 80
  const unsigned char buf[] = {0xF0, 0x9F, 0x98, 0x80, 0x00};
  const unsigned char* p = buf;
  EXPECT_EQ(utf8NextCodepoint(&p), 0x1F600u);
  EXPECT_EQ(p - buf, 4);
}

TEST(Utf8NextCodepoint, StrayContinuationByteYieldsReplacement) {
  // 0x80-0xBF as a lead byte is illegal; treat as one bad byte and advance.
  const unsigned char buf[] = {0x80, 0x41, 0x00};
  const unsigned char* p = buf;
  EXPECT_EQ(utf8NextCodepoint(&p), static_cast<uint32_t>(REPLACEMENT_GLYPH));
  EXPECT_EQ(p - buf, 1);
  // Recovery: the next byte parses normally.
  EXPECT_EQ(utf8NextCodepoint(&p), 0x41u);
  EXPECT_EQ(p - buf, 2);
}

TEST(Utf8NextCodepoint, MissingContinuationByteYieldsReplacement) {
  // E2 expects two continuation bytes; the second one (0x41) is not a
  // continuation byte (high two bits != 10). Decoder consumes only the bytes
  // it actually inspected before detecting the error -- here, 1 byte.
  const unsigned char buf[] = {0xE2, 0x41, 0x00};
  const unsigned char* p = buf;
  EXPECT_EQ(utf8NextCodepoint(&p), static_cast<uint32_t>(REPLACEMENT_GLYPH));
  EXPECT_EQ(p - buf, 1);
  // The 0x41 byte should still parse on the next call.
  EXPECT_EQ(utf8NextCodepoint(&p), 0x41u);
}

TEST(Utf8NextCodepoint, NulInMiddleOfMultibyteDoesNotOverrun) {
  // Regression for the bug fixed in 5c9c582: a NUL byte appearing where a
  // continuation byte is expected must be treated as an invalid continuation,
  // NOT consumed as part of the codepoint. The pointer must stop at the NUL
  // so the caller's end-of-string check still fires on the next iteration.
  //
  // Each case below has a valid lead byte followed by a NUL terminator before
  // the expected number of continuation bytes is reached.
  struct Case {
    const char* name;
    std::vector<unsigned char> buf;
    ptrdiff_t expectedAdvance;
  };
  const std::vector<Case> cases = {
      {"two-byte lead then NUL", {0xC3, 0x00}, 1},
      {"three-byte lead then NUL", {0xE2, 0x00}, 1},
      {"three-byte lead, one good continuation, then NUL", {0xE2, 0x82, 0x00}, 2},
      {"four-byte lead then NUL", {0xF0, 0x00}, 1},
      {"four-byte lead, two good continuations, then NUL", {0xF0, 0x9F, 0x98, 0x00}, 3},
  };
  for (const auto& c : cases) {
    const unsigned char* p = c.buf.data();
    EXPECT_EQ(utf8NextCodepoint(&p), static_cast<uint32_t>(REPLACEMENT_GLYPH)) << c.name;
    EXPECT_EQ(p - c.buf.data(), c.expectedAdvance) << c.name;
    // The terminating NUL must still be in place; a follow-up call returns 0
    // and does not advance further.
    const unsigned char* before = p;
    EXPECT_EQ(utf8NextCodepoint(&p), 0u) << c.name;
    EXPECT_EQ(p, before) << c.name;
  }
}

TEST(Utf8NextCodepoint, OverlongEncodingRejected) {
  // C0 80 encodes U+0000 in two bytes (overlong). Must be rejected per RFC 3629.
  {
    const unsigned char buf[] = {0xC0, 0x80, 0x00};
    const unsigned char* p = buf;
    EXPECT_EQ(utf8NextCodepoint(&p), static_cast<uint32_t>(REPLACEMENT_GLYPH));
  }
  // E0 80 80 is an overlong U+0000 in three bytes.
  {
    const unsigned char buf[] = {0xE0, 0x80, 0x80, 0x00};
    const unsigned char* p = buf;
    EXPECT_EQ(utf8NextCodepoint(&p), static_cast<uint32_t>(REPLACEMENT_GLYPH));
  }
  // F0 80 80 80 is an overlong U+0000 in four bytes.
  {
    const unsigned char buf[] = {0xF0, 0x80, 0x80, 0x80, 0x00};
    const unsigned char* p = buf;
    EXPECT_EQ(utf8NextCodepoint(&p), static_cast<uint32_t>(REPLACEMENT_GLYPH));
  }
}

TEST(Utf8NextCodepoint, SurrogateRangeRejected) {
  // ED A0 80 encodes U+D800 (a high surrogate). The decoder must reject it.
  const unsigned char buf[] = {0xED, 0xA0, 0x80, 0x00};
  const unsigned char* p = buf;
  EXPECT_EQ(utf8NextCodepoint(&p), static_cast<uint32_t>(REPLACEMENT_GLYPH));
}

TEST(Utf8NextCodepoint, OutOfRangeRejected) {
  // F4 90 80 80 would decode to U+110000, just past the Unicode maximum.
  const unsigned char buf[] = {0xF4, 0x90, 0x80, 0x80, 0x00};
  const unsigned char* p = buf;
  EXPECT_EQ(utf8NextCodepoint(&p), static_cast<uint32_t>(REPLACEMENT_GLYPH));
}

TEST(Utf8NextCodepoint, WalksMixedString) {
  // Roundtrip a multi-codepoint string -- ASCII, 2-byte, 3-byte, 4-byte.
  // "A" + U+00E9 + U+20AC + U+1F600
  const unsigned char buf[] = {0x41, 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80, 0x00};
  const unsigned char* p = buf;
  EXPECT_EQ(utf8NextCodepoint(&p), 0x41u);
  EXPECT_EQ(utf8NextCodepoint(&p), 0x00E9u);
  EXPECT_EQ(utf8NextCodepoint(&p), 0x20ACu);
  EXPECT_EQ(utf8NextCodepoint(&p), 0x1F600u);
  EXPECT_EQ(utf8NextCodepoint(&p), 0u);
}

// ---- utf8SafeTruncateBuffer --------------------------------------------------

TEST(Utf8SafeTruncate, ZeroOrNegativeLength) {
  const char buf[] = "hello";
  EXPECT_EQ(utf8SafeTruncateBuffer(buf, 0), 0);
  EXPECT_EQ(utf8SafeTruncateBuffer(buf, -1), 0);
}

TEST(Utf8SafeTruncate, AllAscii) {
  const char buf[] = "hello";
  EXPECT_EQ(utf8SafeTruncateBuffer(buf, 5), 5);
}

TEST(Utf8SafeTruncate, EndsAtCompleteMultibyte) {
  // "café" = 63 61 66 C3 A9 (5 bytes, last codepoint is 2-byte UTF-8)
  const char buf[] = {0x63, 0x61, 0x66, static_cast<char>(0xC3), static_cast<char>(0xA9)};
  EXPECT_EQ(utf8SafeTruncateBuffer(buf, 5), 5);
}

TEST(Utf8SafeTruncate, TrimsIncompleteTrailingTwoByte) {
  // "caf" + just the C3 (first byte of e-acute). The trailing C3 is incomplete.
  const char buf[] = {0x63, 0x61, 0x66, static_cast<char>(0xC3)};
  EXPECT_EQ(utf8SafeTruncateBuffer(buf, 4), 3);
}

TEST(Utf8SafeTruncate, TrimsIncompleteTrailingThreeByte) {
  // "a" + E2 82 (first two of three bytes for U+20AC).
  const char buf[] = {0x61, static_cast<char>(0xE2), static_cast<char>(0x82)};
  EXPECT_EQ(utf8SafeTruncateBuffer(buf, 3), 1);
}

TEST(Utf8SafeTruncate, TrimsIncompleteTrailingFourByte) {
  // "a" + F0 9F (first two of four bytes for U+1F600).
  const char buf[] = {0x61, static_cast<char>(0xF0), static_cast<char>(0x9F)};
  EXPECT_EQ(utf8SafeTruncateBuffer(buf, 3), 1);
}

// ---- utf8RemoveLastChar ------------------------------------------------------

TEST(Utf8RemoveLast, EmptyStringReturnsZero) {
  std::string s;
  EXPECT_EQ(utf8RemoveLastChar(s), 0u);
  EXPECT_EQ(s.size(), 0u);
}

TEST(Utf8RemoveLast, AsciiLastByte) {
  std::string s = "abc";
  EXPECT_EQ(utf8RemoveLastChar(s), 2u);
  EXPECT_EQ(s, "ab");
}

TEST(Utf8RemoveLast, TwoByteLastCodepoint) {
  // "ab" + U+00E9 (C3 A9)
  std::string s = std::string("ab") + "\xC3\xA9";
  EXPECT_EQ(utf8RemoveLastChar(s), 2u);
  EXPECT_EQ(s, "ab");
}

TEST(Utf8RemoveLast, FourByteLastCodepoint) {
  // "x" + U+1F600 (F0 9F 98 80)
  std::string s = std::string("x") + "\xF0\x9F\x98\x80";
  EXPECT_EQ(utf8RemoveLastChar(s), 1u);
  EXPECT_EQ(s, "x");
}

// ---- utf8TruncateChars -------------------------------------------------------

TEST(Utf8TruncateChars, ZeroNoOp) {
  std::string s = "abc";
  utf8TruncateChars(s, 0);
  EXPECT_EQ(s, "abc");
}

TEST(Utf8TruncateChars, RemovesAcrossMixedSizes) {
  // "a" + U+00E9 + U+20AC + "b" = 1+2+3+1 = 7 bytes
  std::string s = std::string("a") + "\xC3\xA9" + "\xE2\x82\xAC" + "b";
  ASSERT_EQ(s.size(), 7u);
  utf8TruncateChars(s, 2);  // drop "b" and U+20AC
  EXPECT_EQ(s, std::string("a") + "\xC3\xA9");
}

TEST(Utf8TruncateChars, OverlongTruncationEmptiesString) {
  std::string s = "abc";
  utf8TruncateChars(s, 100);
  EXPECT_TRUE(s.empty());
}

// ---- utf8IsCjkBreakable ------------------------------------------------------

TEST(Utf8IsCjkBreakable, NonCjkReturnsFalse) {
  EXPECT_FALSE(utf8IsCjkBreakable(0x0041));  // 'A'
  EXPECT_FALSE(utf8IsCjkBreakable(0x00E9));  // e-acute
  EXPECT_FALSE(utf8IsCjkBreakable(0x20AC));  // euro
  EXPECT_FALSE(utf8IsCjkBreakable(0x2FFF));  // just below CJK Symbols
}

TEST(Utf8IsCjkBreakable, KnownCjkSamples) {
  EXPECT_TRUE(utf8IsCjkBreakable(0x4E2D));   // CJK Ideograph 'middle'
  EXPECT_TRUE(utf8IsCjkBreakable(0x3042));   // Hiragana 'a'
  EXPECT_TRUE(utf8IsCjkBreakable(0x30A2));   // Katakana 'A'
  EXPECT_TRUE(utf8IsCjkBreakable(0xAC00));   // Hangul syllable 'ga'
  EXPECT_TRUE(utf8IsCjkBreakable(0x3000));   // CJK ideographic space
  EXPECT_TRUE(utf8IsCjkBreakable(0xFF01));   // Fullwidth exclamation mark
  EXPECT_TRUE(utf8IsCjkBreakable(0x20000));  // CJK Extension B sample
}

TEST(Utf8IsCjkBreakable, RangeEndpointsAreInRange) {
  // Each range's first and last codepoint must match -- this walks every
  // alternation arm of the predicate.
  struct Range {
    uint32_t lo, hi;
  };
  constexpr Range ranges[] = {
      {0x3000, 0x303F},    // CJK Symbols and Punctuation
      {0x3040, 0x309F},    // Hiragana
      {0x30A0, 0x30FF},    // Katakana
      {0x3400, 0x4DBF},    // CJK Extension A
      {0x4E00, 0x9FFF},    // CJK Unified Ideographs
      {0xAC00, 0xD7AF},    // Hangul Syllables
      {0xF900, 0xFAFF},    // CJK Compatibility Ideographs
      {0xFE30, 0xFE4F},    // CJK Compatibility Forms
      {0xFF01, 0xFF60},    // Fullwidth Latin / Punctuation
      {0xFF65, 0xFFEF},    // Halfwidth Katakana / Hangul
      {0x20000, 0x2A6DF},  // CJK Extension B
      {0x2A700, 0x2B73F},  // CJK Extension C
  };
  for (auto r : ranges) {
    EXPECT_TRUE(utf8IsCjkBreakable(r.lo)) << "lo=" << std::hex << r.lo;
    EXPECT_TRUE(utf8IsCjkBreakable(r.hi)) << "hi=" << std::hex << r.hi;
  }
}

TEST(Utf8IsCjkBreakable, GapCodepointsAreNotInRange) {
  // Probe the gaps between the predicate's ranges. Each codepoint here sits
  // in a gap and must miss; together they exercise every "between two arms"
  // transition without tripping on adjacent ranges.
  constexpr uint32_t gaps[] = {
      0x2FFF,   // just below CJK Symbols
      0x3100,   // gap between Katakana (..30FF) and Ext A (3400..)
      0x33FF,   // upper end of the same gap
      0x4DC0,   // gap between Ext A (..4DBF) and CJK Unified (4E00..)
      0xA000,   // gap between CJK Unified (..9FFF) and Hangul (AC00..)
      0xABFF,   // upper end of the same gap
      0xD7B0,   // gap between Hangul (..D7AF) and Compat Ideographs (F900..)
      0xE000,   // private use area, still in that gap
      0xFB00,   // gap between Compat Ideographs (..FAFF) and Compat Forms (FE30..)
      0xFE50,   // gap between Compat Forms (..FE4F) and Fullwidth (FF01..)
      0xFE5F,   // upper end of the same gap
      0xFF00,   // immediately below Fullwidth Latin range
      0xFF61,   // gap between Fullwidth (..FF60) and Halfwidth (FF65..)
      0xFF64,   // upper end of that small gap
      0xFFF0,   // gap between Halfwidth (..FFEF) and CJK Ext B (20000..)
      0x1FFFF,  // upper end of the big gap
      0x2A6E0,  // gap between Ext B (..2A6DF) and Ext C (2A700..)
      0x2B740,  // just above Ext C end
      0x30000,  // far above any included range
  };
  for (uint32_t cp : gaps) {
    EXPECT_FALSE(utf8IsCjkBreakable(cp)) << "cp=" << std::hex << cp;
  }
}

// ---- utf8IsCombiningMark -----------------------------------------------------

TEST(Utf8IsCombiningMark, NonMarkReturnsFalse) {
  EXPECT_FALSE(utf8IsCombiningMark(0x0041));  // 'A'
  EXPECT_FALSE(utf8IsCombiningMark(0x00E9));  // e-acute (precomposed)
  EXPECT_FALSE(utf8IsCombiningMark(0x4E2D));  // CJK ideograph
}

TEST(Utf8IsCombiningMark, KnownMarks) {
  EXPECT_TRUE(utf8IsCombiningMark(0x0301));  // combining acute accent
  EXPECT_TRUE(utf8IsCombiningMark(0x1DC0));  // combining dotted grave accent
  EXPECT_TRUE(utf8IsCombiningMark(0x20D0));  // combining left harpoon above
  EXPECT_TRUE(utf8IsCombiningMark(0xFE20));  // combining ligature left half
}

TEST(Utf8IsCombiningMark, RangeBoundaries) {
  struct Range {
    uint32_t lo, hi;
  };
  constexpr Range ranges[] = {
      {0x0300, 0x036F},  // Combining Diacritical Marks
      {0x1DC0, 0x1DFF},  // Supplement
      {0x20D0, 0x20FF},  // For Symbols
      {0xFE20, 0xFE2F},  // Half Marks
  };
  for (auto r : ranges) {
    EXPECT_TRUE(utf8IsCombiningMark(r.lo)) << "lo=" << std::hex << r.lo;
    EXPECT_TRUE(utf8IsCombiningMark(r.hi)) << "hi=" << std::hex << r.hi;
    if (r.lo > 0) EXPECT_FALSE(utf8IsCombiningMark(r.lo - 1)) << "lo-1=" << std::hex << (r.lo - 1);
    EXPECT_FALSE(utf8IsCombiningMark(r.hi + 1)) << "hi+1=" << std::hex << (r.hi + 1);
  }
}
