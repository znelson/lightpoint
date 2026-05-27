// Behavioral tests for MarkdownParser. Uses a stub Typesetter (see
// Typesetter.h next to this file) that shadows the real header via
// include-path priority in CMakeLists.txt; submitted paragraphs are
// captured for inspection.
//
// Layout itself is not tested here -- that's TypesetterTest's job. These
// tests verify line classification (headings vs lists vs blockquotes vs
// code vs body), inline style toggles (* / _ / ** / __ / *** / ___),
// fenced-code mode switching, blank-line paragraph breaks, and anchor
// recording for headings (the Layer 1 feature).

#include <EpdFontFamily.h>
#include <Markdown/MarkdownParser.h>
#include <Typesetter.h>  // resolves to the stub in this directory
#include <Typesetter/ParsedText.h>
#include <Typesetter/TextAlign.h>
#include <Typesetter/blocks/BlockStyle.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

// Drive the parser with an in-memory string. The readFn copies a slice of
// `source` into the parser's buffer; matches the production semantics where
// `Txt::readContent` fills bytes at a given offset.
//
// NOTE: the lambda is bound to a named `auto` local, NOT directly to a
// FunctionRef. A FunctionRef stores a pointer to the underlying callable;
// assigning a temporary lambda to a FunctionRef would leave a dangling
// pointer once the full-expression ends. Passing the named lambda to the
// MarkdownParser ctor lets it construct its own FunctionRef pointing to
// our named local, which lives until parseMarkdown returns.
bool parseMarkdown(const std::string& source, Typesetter& typesetter, uint8_t paragraphAlignment = 0,
                   bool hyphenation = false, bool focusReading = false, bool extraParagraphSpacing = false) {
  auto readFn = [&source](uint8_t* buf, size_t offset, size_t length) -> bool {
    if (offset + length > source.size()) return false;
    std::memcpy(buf, source.data() + offset, length);
    return true;
  };
  MarkdownParser parser(typesetter, readFn, source.size(), extraParagraphSpacing, hyphenation, focusReading,
                        paragraphAlignment);
  return parser.parse();
}

// Helper: concatenate all words in a paragraph with single spaces. Loses
// information about word continuation, but adequate for "did the right text
// land here" assertions.
std::string joinWords(const ParsedText& p) {
  std::string out;
  for (size_t i = 0; i < p.size(); i++) {
    if (i > 0) out.push_back(' ');
    out += p.getWord(i);
  }
  return out;
}

bool hasFlag(EpdFontFamily::Style style, EpdFontFamily::Style flag) {
  return (static_cast<uint8_t>(style) & static_cast<uint8_t>(flag)) != 0;
}

}  // namespace

// ---- Headings ------------------------------------------------------------

TEST(MarkdownParser, AtxHeadingLevel1ProducesCenteredBoldParagraph) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("# Hello World\n", t));
  ASSERT_EQ(t.submitted.size(), 1u);
  const auto& p = *t.submitted[0];
  EXPECT_EQ(p.getBlockStyle().alignment, TextAlign::Center);
  EXPECT_TRUE(p.getBlockStyle().textAlignDefined);
  EXPECT_GT(p.getBlockStyle().marginTop, 0);
  EXPECT_GT(p.getBlockStyle().marginBottom, 0);
  EXPECT_EQ(joinWords(p), "Hello World");
  EXPECT_TRUE(hasFlag(p.getWordStyle(0), EpdFontFamily::BOLD));
}

TEST(MarkdownParser, HeadingLevelsScaleMarginsInversely) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("# One\n## Two\n### Three\n", t));
  ASSERT_EQ(t.submitted.size(), 3u);
  // Margin formula: max(4, 16 - level*2). Levels 1, 2, 3 -> 14, 12, 10.
  EXPECT_EQ(t.submitted[0]->getBlockStyle().marginTop, 14);
  EXPECT_EQ(t.submitted[1]->getBlockStyle().marginTop, 12);
  EXPECT_EQ(t.submitted[2]->getBlockStyle().marginTop, 10);
}

TEST(MarkdownParser, HeadingWithoutSpaceIsNotAHeading) {
  // "#Hello" (no space) is regular body text.
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("#Hello\n", t));
  ASSERT_EQ(t.submitted.size(), 1u);
  const auto& p = *t.submitted[0];
  EXPECT_NE(p.getBlockStyle().alignment, TextAlign::Center);
  EXPECT_EQ(p.getWord(0), "#Hello");
}

// ---- Emphasis ------------------------------------------------------------

TEST(MarkdownParser, BoldStarStarTogglesBoldOnAndOff) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("plain **bold** plain\n", t));
  ASSERT_EQ(t.submitted.size(), 1u);
  const auto& p = *t.submitted[0];
  ASSERT_EQ(p.size(), 3u);
  EXPECT_EQ(p.getWord(0), "plain");
  EXPECT_FALSE(hasFlag(p.getWordStyle(0), EpdFontFamily::BOLD));
  EXPECT_EQ(p.getWord(1), "bold");
  EXPECT_TRUE(hasFlag(p.getWordStyle(1), EpdFontFamily::BOLD));
  EXPECT_EQ(p.getWord(2), "plain");
  EXPECT_FALSE(hasFlag(p.getWordStyle(2), EpdFontFamily::BOLD));
}

TEST(MarkdownParser, ItalicStarTogglesItalicOnAndOff) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("a *b* c\n", t));
  ASSERT_EQ(t.submitted.size(), 1u);
  const auto& p = *t.submitted[0];
  ASSERT_EQ(p.size(), 3u);
  EXPECT_FALSE(hasFlag(p.getWordStyle(0), EpdFontFamily::ITALIC));
  EXPECT_TRUE(hasFlag(p.getWordStyle(1), EpdFontFamily::ITALIC));
  EXPECT_FALSE(hasFlag(p.getWordStyle(2), EpdFontFamily::ITALIC));
}

TEST(MarkdownParser, TripleStarSetsBoldAndItalic) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("***both***\n", t));
  ASSERT_EQ(t.submitted.size(), 1u);
  const auto& p = *t.submitted[0];
  ASSERT_EQ(p.size(), 1u);
  EXPECT_TRUE(hasFlag(p.getWordStyle(0), EpdFontFamily::BOLD));
  EXPECT_TRUE(hasFlag(p.getWordStyle(0), EpdFontFamily::ITALIC));
}

TEST(MarkdownParser, UnderscoreEquivalentToAsterisk) {
  // __bold__ and _italic_ behave the same as ** and *.
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("a __b__ _c_\n", t));
  ASSERT_EQ(t.submitted.size(), 1u);
  const auto& p = *t.submitted[0];
  ASSERT_EQ(p.size(), 3u);
  EXPECT_TRUE(hasFlag(p.getWordStyle(1), EpdFontFamily::BOLD));
  EXPECT_TRUE(hasFlag(p.getWordStyle(2), EpdFontFamily::ITALIC));
}

TEST(MarkdownParser, BacktickInlineCodeStripsStyle) {
  // Inside a `code` span, bold/italic context does not apply.
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("**x `code` y**\n", t));
  const auto& p = *t.submitted[0];
  // Tokens: x (bold), code (regular), y (bold)
  ASSERT_EQ(p.size(), 3u);
  EXPECT_EQ(p.getWord(1), "code");
  EXPECT_EQ(p.getWordStyle(1), EpdFontFamily::REGULAR);
  EXPECT_TRUE(hasFlag(p.getWordStyle(0), EpdFontFamily::BOLD));
  EXPECT_TRUE(hasFlag(p.getWordStyle(2), EpdFontFamily::BOLD));
}

// ---- Lists, blockquotes, rules, code blocks -----------------------------

TEST(MarkdownParser, UnorderedListEmitsBulletPrefix) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("- item one\n* item two\n+ item three\n", t));
  ASSERT_EQ(t.submitted.size(), 3u);
  // Each list item starts with the UTF-8 BULLET U+2022 (E2 80 A2)
  for (const auto& p : t.submitted) {
    ASSERT_GT(p->size(), 0u);
    EXPECT_EQ(p->getWord(0), "\xe2\x80\xa2");
    EXPECT_EQ(p->getBlockStyle().alignment, TextAlign::Left);
  }
}

TEST(MarkdownParser, BlockquoteDepthSetsMarginLeft) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("> level one\n>> level two\n", t));
  ASSERT_EQ(t.submitted.size(), 2u);
  // marginLeft = 16 * depth
  EXPECT_EQ(t.submitted[0]->getBlockStyle().marginLeft, 16);
  EXPECT_EQ(t.submitted[1]->getBlockStyle().marginLeft, 32);
}

TEST(MarkdownParser, HorizontalRuleFlushesWithoutNewParagraph) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("before\n---\nafter\n", t));
  // "before" and "after" each get their own paragraph; the rule itself
  // doesn't emit one (it only flushes any pending content).
  ASSERT_EQ(t.submitted.size(), 2u);
  EXPECT_EQ(t.submitted[0]->getWord(0), "before");
  EXPECT_EQ(t.submitted[1]->getWord(0), "after");
}

TEST(MarkdownParser, FencedCodeBlockSwitchesMode) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("```\nint x = 1;\nint y = 2;\n```\n", t));
  // Two code lines, two paragraphs.
  ASSERT_EQ(t.submitted.size(), 2u);
  EXPECT_EQ(joinWords(*t.submitted[0]), "int x = 1;");
  EXPECT_EQ(joinWords(*t.submitted[1]), "int y = 2;");
}

// ---- Links (Layer 2) ----------------------------------------------------

TEST(MarkdownParser, LinkProducesUnderlinedWord) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("see [docs](#chapter1) for details\n", t));
  ASSERT_EQ(t.submitted.size(), 1u);
  const auto& p = *t.submitted[0];
  // Words: "see", "docs", "for", "details"
  ASSERT_EQ(p.size(), 4u);
  EXPECT_EQ(p.getWord(1), "docs");
  EXPECT_TRUE(hasFlag(p.getWordStyle(1), EpdFontFamily::UNDERLINE));
  // Non-link words must NOT be underlined.
  EXPECT_FALSE(hasFlag(p.getWordStyle(0), EpdFontFamily::UNDERLINE));
  EXPECT_FALSE(hasFlag(p.getWordStyle(2), EpdFontFamily::UNDERLINE));
  EXPECT_FALSE(hasFlag(p.getWordStyle(3), EpdFontFamily::UNDERLINE));
}

TEST(MarkdownParser, MultiWordLinkAllWordsUnderlined) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("[click here for more](url)\n", t));
  ASSERT_EQ(t.submitted.size(), 1u);
  const auto& p = *t.submitted[0];
  ASSERT_EQ(p.size(), 4u);
  for (size_t i = 0; i < p.size(); i++) {
    EXPECT_TRUE(hasFlag(p.getWordStyle(i), EpdFontFamily::UNDERLINE)) << "word " << i << " (" << p.getWord(i) << ")";
  }
}

TEST(MarkdownParser, LinkInsideBoldContextInheritsBold) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("**before [link](url) after**\n", t));
  const auto& p = *t.submitted[0];
  // Tokens: before(B), link(B+U), after(B)
  ASSERT_EQ(p.size(), 3u);
  EXPECT_TRUE(hasFlag(p.getWordStyle(0), EpdFontFamily::BOLD));
  EXPECT_FALSE(hasFlag(p.getWordStyle(0), EpdFontFamily::UNDERLINE));
  EXPECT_TRUE(hasFlag(p.getWordStyle(1), EpdFontFamily::BOLD));
  EXPECT_TRUE(hasFlag(p.getWordStyle(1), EpdFontFamily::UNDERLINE));
  EXPECT_TRUE(hasFlag(p.getWordStyle(2), EpdFontFamily::BOLD));
  EXPECT_FALSE(hasFlag(p.getWordStyle(2), EpdFontFamily::UNDERLINE));
}

TEST(MarkdownParser, ImageSyntaxPreservedAsLiteral) {
  // ![alt](src) is the Markdown image form; not a link. Render literally.
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("![alt](src)\n", t));
  const auto& p = *t.submitted[0];
  ASSERT_EQ(p.size(), 1u);
  EXPECT_EQ(p.getWord(0), "![alt](src)");
  EXPECT_FALSE(hasFlag(p.getWordStyle(0), EpdFontFamily::UNDERLINE));
}

TEST(MarkdownParser, MalformedLinkNoClosingParenStaysLiteral) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("[oops](no close paren\n", t));
  const auto& p = *t.submitted[0];
  // Characters '[' ']' '(' all treated as regular chars; no underline,
  // no markup stripping.
  for (size_t i = 0; i < p.size(); i++) {
    EXPECT_FALSE(hasFlag(p.getWordStyle(i), EpdFontFamily::UNDERLINE));
  }
}

TEST(MarkdownParser, BracketsWithoutParensStayLiteral) {
  // "[text]" without "(target)" is not a link.
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("see [bracketed] words\n", t));
  const auto& p = *t.submitted[0];
  for (size_t i = 0; i < p.size(); i++) {
    EXPECT_FALSE(hasFlag(p.getWordStyle(i), EpdFontFamily::UNDERLINE));
  }
}

// ---- Backslash escapes ---------------------------------------------------

TEST(MarkdownParser, BackslashEscapeOfEmphasisMarkerRendersLiteral) {
  Typesetter t;
  // \*not italic\* should render literal asterisks with no italic styling.
  ASSERT_TRUE(parseMarkdown("\\*not italic\\*\n", t));
  const auto& p = *t.submitted[0];
  // Words: "*not", "italic*"
  ASSERT_EQ(p.size(), 2u);
  EXPECT_EQ(p.getWord(0), "*not");
  EXPECT_EQ(p.getWord(1), "italic*");
  for (size_t i = 0; i < p.size(); i++) {
    EXPECT_FALSE(hasFlag(p.getWordStyle(i), EpdFontFamily::ITALIC));
  }
}

TEST(MarkdownParser, BackslashEscapeOfBracketsPreventsLink) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("\\[not a link\\](url)\n", t));
  const auto& p = *t.submitted[0];
  // Whole sequence renders literally without underline.
  for (size_t i = 0; i < p.size(); i++) {
    EXPECT_FALSE(hasFlag(p.getWordStyle(i), EpdFontFamily::UNDERLINE));
  }
  EXPECT_EQ(p.getWord(0), "[not");
}

TEST(MarkdownParser, BackslashOfNonPunctuationStaysLiteral) {
  Typesetter t;
  // \n is not an escape (n is not ASCII punctuation); render both chars literally.
  ASSERT_TRUE(parseMarkdown("hello\\nworld\n", t));
  const auto& p = *t.submitted[0];
  ASSERT_EQ(p.size(), 1u);
  EXPECT_EQ(p.getWord(0), "hello\\nworld");
}

TEST(MarkdownParser, DoubleBackslashEmitsLiteralBackslashAndAllowsEmphasisAfter) {
  Typesetter t;
  // `\\` -> literal `\`; the subsequent `*` then triggers italic as normal.
  // The emphasis-marker flush separates the literal `\` from the italic run,
  // so we get two words. Visual continuity (no space between them) would
  // need attachToPrevious, which this parser doesn't currently set at
  // style boundaries.
  ASSERT_TRUE(parseMarkdown("\\\\*italic*\n", t));
  const auto& p = *t.submitted[0];
  ASSERT_EQ(p.size(), 2u);
  EXPECT_EQ(p.getWord(0), "\\");
  EXPECT_FALSE(hasFlag(p.getWordStyle(0), EpdFontFamily::ITALIC));
  EXPECT_EQ(p.getWord(1), "italic");
  EXPECT_TRUE(hasFlag(p.getWordStyle(1), EpdFontFamily::ITALIC));
}

TEST(MarkdownParser, BackslashHeadingAtLineStartIsNotHeading) {
  // \# at line start should not be a heading; the block-level classifier
  // looks at line[0] which is \\ (not #), so we fall through to body where
  // tryEscape strips the backslash.
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("\\# Not a heading\n", t));
  const auto& p = *t.submitted[0];
  EXPECT_NE(p.getBlockStyle().alignment, TextAlign::Center);
  EXPECT_EQ(p.getWord(0), "#");
}

// ---- Link target balanced parens + escapes ------------------------------

TEST(MarkdownParser, LinkTargetWithBalancedParensDoesNotClip) {
  Typesetter t;
  // [wiki](Foo_(disambig)) -- the inner () should not terminate the link
  // target; the outer ) closes it. Nothing trails the link.
  ASSERT_TRUE(parseMarkdown("[wiki](Foo_(disambig))\n", t));
  const auto& p = *t.submitted[0];
  ASSERT_EQ(p.size(), 1u);
  EXPECT_EQ(p.getWord(0), "wiki");
  EXPECT_TRUE(hasFlag(p.getWordStyle(0), EpdFontFamily::UNDERLINE));
}

TEST(MarkdownParser, LinkTargetWithEscapedParenNoClip) {
  Typesetter t;
  // Escaped close-paren inside target shouldn't end the link.
  ASSERT_TRUE(parseMarkdown("[x](a\\)b)\n", t));
  const auto& p = *t.submitted[0];
  ASSERT_EQ(p.size(), 1u);
  EXPECT_EQ(p.getWord(0), "x");
  EXPECT_TRUE(hasFlag(p.getWordStyle(0), EpdFontFamily::UNDERLINE));
}

TEST(MarkdownParser, LinkTextWithEscapedBracketKeepsLinkIntact) {
  Typesetter t;
  // [a\]b](url) -- the escaped ] inside link text should not terminate the
  // link, and the de-escape pass should produce a token "a]b".
  ASSERT_TRUE(parseMarkdown("[a\\]b](url)\n", t));
  const auto& p = *t.submitted[0];
  ASSERT_EQ(p.size(), 1u);
  EXPECT_EQ(p.getWord(0), "a]b");
  EXPECT_TRUE(hasFlag(p.getWordStyle(0), EpdFontFamily::UNDERLINE));
}

// ---- Paragraph flow ------------------------------------------------------

TEST(MarkdownParser, MultiLineBodyJoinsIntoOneParagraph) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("line one\nline two\nline three\n", t));
  ASSERT_EQ(t.submitted.size(), 1u);
  EXPECT_EQ(joinWords(*t.submitted[0]), "line one line two line three");
}

TEST(MarkdownParser, BlankLineSplitsParagraphs) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("first\n\nsecond\n", t));
  ASSERT_EQ(t.submitted.size(), 2u);
  EXPECT_EQ(t.submitted[0]->getWord(0), "first");
  EXPECT_EQ(t.submitted[1]->getWord(0), "second");
}

TEST(MarkdownParser, TrailingContentWithoutNewlineStillEmitted) {
  Typesetter t;
  ASSERT_TRUE(parseMarkdown("dangling", t));  // no trailing \n
  ASSERT_EQ(t.submitted.size(), 1u);
  EXPECT_EQ(t.submitted[0]->getWord(0), "dangling");
}

// ---- Anchor recording (Layer 1) -----------------------------------------

// Helper for tests that need access to the parser's anchor list. Mirrors
// parseMarkdown but exposes the parser for getAnchors().
template <typename TypesetterT>
std::vector<std::pair<std::string, uint16_t>> parseAndGetAnchors(const std::string& source, TypesetterT& typesetter) {
  auto readFn = [&source](uint8_t* buf, size_t offset, size_t length) -> bool {
    if (offset + length > source.size()) return false;
    std::memcpy(buf, source.data() + offset, length);
    return true;
  };
  MarkdownParser parser(typesetter, readFn, source.size(), false, false, false, 0);
  EXPECT_TRUE(parser.parse());
  return parser.getAnchors();
}

TEST(MarkdownParser, HeadingsRecordSlugAndPage) {
  Typesetter t;
  // Stub's page counter never advances on its own, so all anchors record
  // page 0 -- enough to verify the slug + accumulation order.
  const auto anchors = parseAndGetAnchors("# Intro\n\nbody\n\n## Setup\n\nmore body\n", t);
  ASSERT_EQ(anchors.size(), 2u);
  EXPECT_EQ(anchors[0].first, "intro");
  EXPECT_EQ(anchors[1].first, "setup");
  EXPECT_EQ(anchors[0].second, 0u);
  EXPECT_EQ(anchors[1].second, 0u);
}

// Custom typesetter that bumps completedPageCount on every submit so each
// heading sees a distinct "current page" value. Models a degenerate
// 1-paragraph-per-page world but pins down the timing semantic.
class CountingTypesetter : public Typesetter {
 public:
  void submitParagraph(std::unique_ptr<ParsedText> p) {
    Typesetter::submitParagraph(std::move(p));
    ++completedPageCount;
  }
};

TEST(MarkdownParser, AnchorsRespectPageCounterBetweenSubmits) {
  CountingTypesetter t;
  const auto anchors = parseAndGetAnchors("# One\n\n# Two\n\n# Three\n", t);
  ASSERT_EQ(anchors.size(), 3u);
  // After each heading submit, counter advances. Anchor pages recorded
  // after submit see the new value: 1, 2, 3.
  EXPECT_EQ(anchors[0].second, 1u);
  EXPECT_EQ(anchors[1].second, 2u);
  EXPECT_EQ(anchors[2].second, 3u);
}

TEST(MarkdownParser, SlugifyLowercasesAndDashes) {
  // The slug is derived from the heading text: lowercase, non-alphanumeric
  // run -> single dash, leading/trailing dashes trimmed.
  Typesetter t;
  const auto anchors = parseAndGetAnchors("# Hello, World!\n\n## section 1.2\n\n### A   B   C\n", t);
  ASSERT_EQ(anchors.size(), 3u);
  EXPECT_EQ(anchors[0].first, "hello-world");
  EXPECT_EQ(anchors[1].first, "section-1-2");
  EXPECT_EQ(anchors[2].first, "a-b-c");
}
