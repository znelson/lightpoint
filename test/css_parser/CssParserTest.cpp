#include <gtest/gtest.h>

#include <string>

#include "CssParser.h"
#include "CssStyle.h"
#include "HalStorageTestApi.h"

namespace {

// CssParser is non-copyable and non-movable, so we construct it in place and
// stream CSS source into it via a readable HalFile backed by the HAL stub.
void loadCss(CssParser& parser, const std::string& css) {
  HalFile file = test_stubs::makeReadableHalFile(css);
  parser.loadFromStream(file);
}

// Rule database used across the cascade / resolveStyle tests.
constexpr const char* CASCADE_CSS = R"(
p { text-align: left; margin-top: 1em; }
.highlight { text-align: center; font-weight: bold; }
p.highlight { text-align: right; }
em { font-style: italic; }
)";

}  // namespace

// ---- text-align (interpretAlignment via parseInlineStyle) --------------------

TEST(CssParser, AlignmentKeywords) {
  EXPECT_EQ(CssParser::parseInlineStyle("text-align: left").textAlign, CssTextAlign::Left);
  EXPECT_EQ(CssParser::parseInlineStyle("text-align: right").textAlign, CssTextAlign::Right);
  EXPECT_EQ(CssParser::parseInlineStyle("text-align: center").textAlign, CssTextAlign::Center);
  EXPECT_EQ(CssParser::parseInlineStyle("text-align: justify").textAlign, CssTextAlign::Justify);
  EXPECT_EQ(CssParser::parseInlineStyle("text-align: start").textAlign, CssTextAlign::Left);
  EXPECT_EQ(CssParser::parseInlineStyle("text-align: end").textAlign, CssTextAlign::Right);
}

TEST(CssParser, AlignmentCaseInsensitive) {
  EXPECT_EQ(CssParser::parseInlineStyle("text-align: LEFT").textAlign, CssTextAlign::Left);
  EXPECT_EQ(CssParser::parseInlineStyle("text-align: Center").textAlign, CssTextAlign::Center);
  EXPECT_EQ(CssParser::parseInlineStyle("text-align: JuStIfY").textAlign, CssTextAlign::Justify);
}

TEST(CssParser, AlignmentTrimsWhitespace) {
  EXPECT_EQ(CssParser::parseInlineStyle("text-align:   left  ").textAlign, CssTextAlign::Left);
  EXPECT_EQ(CssParser::parseInlineStyle("text-align:\tcenter\n").textAlign, CssTextAlign::Center);
}

TEST(CssParser, AlignmentUnknownDefaultsLeft) {
  // The declaration parser does still mark text-align as defined even for an
  // unrecognized keyword -- it just falls back to Left.
  const CssStyle s = CssParser::parseInlineStyle("text-align: nonsense");
  EXPECT_TRUE(s.hasTextAlign());
  EXPECT_EQ(s.textAlign, CssTextAlign::Left);
}

// ---- font-style --------------------------------------------------------------

TEST(CssParser, FontStyleItalicAndOblique) {
  EXPECT_EQ(CssParser::parseInlineStyle("font-style: italic").fontStyle, CssFontStyle::Italic);
  EXPECT_EQ(CssParser::parseInlineStyle("font-style: oblique").fontStyle, CssFontStyle::Italic);
  EXPECT_EQ(CssParser::parseInlineStyle("font-style: ITALIC").fontStyle, CssFontStyle::Italic);
}

TEST(CssParser, FontStyleNormalDefault) {
  EXPECT_EQ(CssParser::parseInlineStyle("font-style: normal").fontStyle, CssFontStyle::Normal);
  EXPECT_EQ(CssParser::parseInlineStyle("font-style: inherit").fontStyle, CssFontStyle::Normal);
}

// ---- font-weight -------------------------------------------------------------

TEST(CssParser, FontWeightNamed) {
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: bold").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: bolder").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: normal").fontWeight, CssFontWeight::Normal);
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: lighter").fontWeight, CssFontWeight::Normal);
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: Bold").fontWeight, CssFontWeight::Bold);
}

TEST(CssParser, FontWeightNumericThreshold) {
  // Implementation rule: >=700 maps to Bold, everything else to Normal.
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: 400").fontWeight, CssFontWeight::Normal);
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: 500").fontWeight, CssFontWeight::Normal);
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: 699").fontWeight, CssFontWeight::Normal);
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: 700").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: 900").fontWeight, CssFontWeight::Bold);
}

TEST(CssParser, FontWeightUnparseable) {
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: inherit").fontWeight, CssFontWeight::Normal);
  EXPECT_EQ(CssParser::parseInlineStyle("font-weight: 700abc").fontWeight, CssFontWeight::Normal);
}

// ---- text-decoration ---------------------------------------------------------

TEST(CssParser, DecorationUnderline) {
  EXPECT_EQ(CssParser::parseInlineStyle("text-decoration: underline").textDecoration, CssTextDecoration::Underline);
  EXPECT_EQ(CssParser::parseInlineStyle("text-decoration: UNDERLINE").textDecoration, CssTextDecoration::Underline);
}

TEST(CssParser, DecorationFindsUnderlineInMultiValue) {
  EXPECT_EQ(CssParser::parseInlineStyle("text-decoration: underline solid red").textDecoration,
            CssTextDecoration::Underline);
  EXPECT_EQ(CssParser::parseInlineStyle("text-decoration: solid red Underline").textDecoration,
            CssTextDecoration::Underline);
}

TEST(CssParser, DecorationNoneOrUnknown) {
  EXPECT_EQ(CssParser::parseInlineStyle("text-decoration: none").textDecoration, CssTextDecoration::None);
  EXPECT_EQ(CssParser::parseInlineStyle("text-decoration: line-through").textDecoration, CssTextDecoration::None);
}

TEST(CssParser, DecorationLineAlias) {
  // text-decoration-line is recognised as an alias for text-decoration.
  EXPECT_EQ(CssParser::parseInlineStyle("text-decoration-line: underline").textDecoration,
            CssTextDecoration::Underline);
}

// ---- length / units ---------------------------------------------------------

TEST(CssParser, LengthUnits) {
  // margin-top exercises the interpretLength path.
  CssStyle s = CssParser::parseInlineStyle("margin-top: 2em");
  EXPECT_FLOAT_EQ(s.marginTop.value, 2.0f);
  EXPECT_EQ(s.marginTop.unit, CssUnit::Em);

  s = CssParser::parseInlineStyle("margin-top: 1.5rem");
  EXPECT_FLOAT_EQ(s.marginTop.value, 1.5f);
  EXPECT_EQ(s.marginTop.unit, CssUnit::Rem);

  s = CssParser::parseInlineStyle("margin-top: 12pt");
  EXPECT_FLOAT_EQ(s.marginTop.value, 12.0f);
  EXPECT_EQ(s.marginTop.unit, CssUnit::Points);

  s = CssParser::parseInlineStyle("margin-top: 50%");
  EXPECT_FLOAT_EQ(s.marginTop.value, 50.0f);
  EXPECT_EQ(s.marginTop.unit, CssUnit::Percent);

  // Unitless -> Pixels (the default).
  s = CssParser::parseInlineStyle("margin-top: 8");
  EXPECT_FLOAT_EQ(s.marginTop.value, 8.0f);
  EXPECT_EQ(s.marginTop.unit, CssUnit::Pixels);
}

TEST(CssParser, LengthCaseInsensitiveUnits) {
  EXPECT_EQ(CssParser::parseInlineStyle("margin-top: 2EM").marginTop.unit, CssUnit::Em);
  EXPECT_EQ(CssParser::parseInlineStyle("margin-top: 1.5Rem").marginTop.unit, CssUnit::Rem);
}

TEST(CssParser, LengthSignedAndDecimal) {
  EXPECT_FLOAT_EQ(CssParser::parseInlineStyle("margin-top: -3em").marginTop.value, -3.0f);
  EXPECT_FLOAT_EQ(CssParser::parseInlineStyle("margin-top: +1.5em").marginTop.value, 1.5f);
  EXPECT_FLOAT_EQ(CssParser::parseInlineStyle("margin-top: .25em").marginTop.value, 0.25f);
  EXPECT_FLOAT_EQ(CssParser::parseInlineStyle("margin-top: 0").marginTop.value, 0.0f);
}

TEST(CssParser, LengthInternalWhitespaceFallsBackToPixels) {
  // CssParser preserves the pre-refactor behaviour: a space between number
  // and unit means the unit lookup misses and the length stays pixel-typed.
  const CssStyle s = CssParser::parseInlineStyle("margin-top: 2 em");
  EXPECT_FLOAT_EQ(s.marginTop.value, 2.0f);
  EXPECT_EQ(s.marginTop.unit, CssUnit::Pixels);
}

TEST(CssParser, LengthRejectsNonNumericForWidthHeight) {
  // width/height use tryInterpretLength and only set the defined flag on
  // successful numeric parse. auto/inherit/initial leave the field undefined.
  EXPECT_FALSE(CssParser::parseInlineStyle("width: auto").hasImageWidth());
  EXPECT_FALSE(CssParser::parseInlineStyle("width: inherit").hasImageWidth());
  EXPECT_FALSE(CssParser::parseInlineStyle("height: initial").hasImageHeight());
  // ... but a valid numeric does set them.
  const CssStyle s = CssParser::parseInlineStyle("width: 200px");
  EXPECT_TRUE(s.hasImageWidth());
  EXPECT_FLOAT_EQ(s.imageWidth.value, 200.0f);
}

// ---- parseInlineStyle declaration structure ---------------------------------

TEST(CssParser, ParseInlineStyleMultipleProperties) {
  const CssStyle s = CssParser::parseInlineStyle("text-align: right; font-weight: bold; margin-top: 2em");
  EXPECT_TRUE(s.hasTextAlign());
  EXPECT_EQ(s.textAlign, CssTextAlign::Right);
  EXPECT_TRUE(s.hasFontWeight());
  EXPECT_EQ(s.fontWeight, CssFontWeight::Bold);
  EXPECT_TRUE(s.hasMarginTop());
  EXPECT_FLOAT_EQ(s.marginTop.value, 2.0f);
}

TEST(CssParser, ParseInlineStyleSkipsMalformedDeclarations) {
  // Empty value, missing colon, leading + trailing semicolons should all be
  // tolerated without dropping the surrounding well-formed declarations.
  const CssStyle s = CssParser::parseInlineStyle(";text-align: ;font-style: italic;;notakvpair");
  EXPECT_FALSE(s.hasTextAlign());  // empty value -> ignored
  EXPECT_TRUE(s.hasFontStyle());
  EXPECT_EQ(s.fontStyle, CssFontStyle::Italic);
}

TEST(CssParser, ParseInlineStyleDisplayNone) {
  const CssStyle s = CssParser::parseInlineStyle("display: none");
  EXPECT_TRUE(s.hasDisplay());
  EXPECT_EQ(s.display, CssDisplay::None);
}

TEST(CssParser, ParseInlineStyleDisplayImportantStripped) {
  // !important is stripped before comparing the keyword.
  const CssStyle s = CssParser::parseInlineStyle("display: none !important");
  EXPECT_TRUE(s.hasDisplay());
  EXPECT_EQ(s.display, CssDisplay::None);
}

TEST(CssParser, ParseInlineStyleDisplayCaseInsensitive) {
  const CssStyle s = CssParser::parseInlineStyle("display: NONE");
  EXPECT_TRUE(s.hasDisplay());
  EXPECT_EQ(s.display, CssDisplay::None);
}

TEST(CssParser, ParseInlineStyleVerticalAlign) {
  const CssStyle sup = CssParser::parseInlineStyle("vertical-align: super");
  EXPECT_TRUE(sup.hasVerticalAlign());
  EXPECT_EQ(sup.verticalAlign, CssVerticalAlign::Super);

  const CssStyle sub = CssParser::parseInlineStyle("vertical-align: SUB");
  EXPECT_TRUE(sub.hasVerticalAlign());
  EXPECT_EQ(sub.verticalAlign, CssVerticalAlign::Sub);

  // Anything else leaves the field unset (baseline default).
  EXPECT_FALSE(CssParser::parseInlineStyle("vertical-align: top").hasVerticalAlign());
}

TEST(CssParser, ParseInlineStyleMarginShorthand) {
  // Single-value margin applies to all four sides.
  const CssStyle s1 = CssParser::parseInlineStyle("margin: 2em");
  EXPECT_FLOAT_EQ(s1.marginTop.value, 2.0f);
  EXPECT_FLOAT_EQ(s1.marginRight.value, 2.0f);
  EXPECT_FLOAT_EQ(s1.marginBottom.value, 2.0f);
  EXPECT_FLOAT_EQ(s1.marginLeft.value, 2.0f);

  // Four-value form maps top, right, bottom, left.
  const CssStyle s4 = CssParser::parseInlineStyle("margin: 1em 2em 3em 4em");
  EXPECT_FLOAT_EQ(s4.marginTop.value, 1.0f);
  EXPECT_FLOAT_EQ(s4.marginRight.value, 2.0f);
  EXPECT_FLOAT_EQ(s4.marginBottom.value, 3.0f);
  EXPECT_FLOAT_EQ(s4.marginLeft.value, 4.0f);
}

TEST(CssParser, ParseInlineStylePaddingShorthand) {
  const CssStyle s = CssParser::parseInlineStyle("padding: 1em 2em");
  // Two-value form: top/bottom = first, left/right = second.
  EXPECT_FLOAT_EQ(s.paddingTop.value, 1.0f);
  EXPECT_FLOAT_EQ(s.paddingRight.value, 2.0f);
  EXPECT_FLOAT_EQ(s.paddingBottom.value, 1.0f);
  EXPECT_FLOAT_EQ(s.paddingLeft.value, 2.0f);
}

// ---- loadFromStream + resolveStyle -------------------------------------------

TEST(CssParser, LoadFromStreamPopulatesDatabase) {
  CssParser parser("");
  loadCss(parser, "p { text-align: justify; }");
  EXPECT_FALSE(parser.empty());
  EXPECT_GT(parser.ruleCount(), 0u);
}

TEST(CssParser, ResolveStyleTagOnly) {
  CssParser parser("");
  loadCss(parser, CASCADE_CSS);
  const CssStyle s = parser.resolveStyle("p", "");
  EXPECT_TRUE(s.hasTextAlign());
  EXPECT_EQ(s.textAlign, CssTextAlign::Left);
  EXPECT_TRUE(s.hasMarginTop());
  EXPECT_FLOAT_EQ(s.marginTop.value, 1.0f);
  EXPECT_EQ(s.marginTop.unit, CssUnit::Em);
}

TEST(CssParser, ResolveStyleClassOnly) {
  // Tag with no matching rule, but the class matches.
  CssParser parser("");
  loadCss(parser, CASCADE_CSS);
  const CssStyle s = parser.resolveStyle("div", "highlight");
  EXPECT_TRUE(s.hasTextAlign());
  EXPECT_EQ(s.textAlign, CssTextAlign::Center);
  EXPECT_TRUE(s.hasFontWeight());
  EXPECT_EQ(s.fontWeight, CssFontWeight::Bold);
}

TEST(CssParser, ResolveStyleCascade) {
  // p { text-align: left } < .highlight { text-align: center } < p.highlight { text-align: right }
  CssParser parser("");
  loadCss(parser, CASCADE_CSS);
  const CssStyle s = parser.resolveStyle("p", "highlight");
  ASSERT_TRUE(s.hasTextAlign());
  EXPECT_EQ(s.textAlign, CssTextAlign::Right);  // p.highlight wins
  // font-weight only defined on .highlight; flows through.
  ASSERT_TRUE(s.hasFontWeight());
  EXPECT_EQ(s.fontWeight, CssFontWeight::Bold);
  // margin-top only on p; flows through.
  ASSERT_TRUE(s.hasMarginTop());
  EXPECT_FLOAT_EQ(s.marginTop.value, 1.0f);
}

TEST(CssParser, ResolveStyleNoMatchReturnsDefault) {
  CssParser parser("");
  loadCss(parser, CASCADE_CSS);
  const CssStyle s = parser.resolveStyle("article", "");
  EXPECT_FALSE(s.hasTextAlign());
  EXPECT_FALSE(s.hasFontWeight());
  EXPECT_FALSE(s.hasMarginTop());
}

TEST(CssParser, ResolveStyleMultiClass) {
  CssParser parser("");
  loadCss(parser, R"(
.a { font-style: italic; }
.b { font-weight: bold; }
)");
  const CssStyle s = parser.resolveStyle("span", "a b");
  EXPECT_EQ(s.fontStyle, CssFontStyle::Italic);
  EXPECT_EQ(s.fontWeight, CssFontWeight::Bold);
}

TEST(CssParser, ResolveStyleTagDotClassWinsOverClass) {
  // Same property defined in both .cls and tag.cls -- tag.cls (higher
  // specificity) is applied after .cls and therefore wins.
  CssParser parser("");
  loadCss(parser, R"(
.x { text-align: left; }
p.x { text-align: right; }
)");
  EXPECT_EQ(parser.resolveStyle("p", "x").textAlign, CssTextAlign::Right);
}

TEST(CssParser, ResolveStyleLooksUpCaseInsensitive) {
  // Selectors are stored normalized (lowercase); lookups must lowercase too.
  // This implicitly exercises the heterogeneous map lookup -- the lookup key
  // is built in resolveStyle's stack buffer (string_view) against stored
  // std::string keys.
  CssParser parser("");
  loadCss(parser, "p { text-align: center; } .CLS { font-weight: bold; }");
  EXPECT_EQ(parser.resolveStyle("P", "").textAlign, CssTextAlign::Center);
  EXPECT_EQ(parser.resolveStyle("div", "Cls").fontWeight, CssFontWeight::Bold);
}

TEST(CssParser, LoadFromStreamAccumulatesAcrossCalls) {
  CssParser parser("");
  HalFile a = test_stubs::makeReadableHalFile("p { text-align: justify; }");
  parser.loadFromStream(a);
  HalFile b = test_stubs::makeReadableHalFile(".bold { font-weight: bold; }");
  parser.loadFromStream(b);

  EXPECT_EQ(parser.ruleCount(), 2u);
  const CssStyle s = parser.resolveStyle("p", "bold");
  EXPECT_EQ(s.textAlign, CssTextAlign::Justify);
  EXPECT_EQ(s.fontWeight, CssFontWeight::Bold);
}

TEST(CssParser, LoadFromStreamIgnoresUnsupportedSelectors) {
  // Pseudo-classes, attribute selectors, descendant combinators, etc. are
  // silently skipped. Only the plain element rule should land in the database.
  CssParser parser("");
  loadCss(parser, R"(
p:first-child { text-align: right; }
[data-x] { font-weight: bold; }
p > em { font-style: italic; }
p { text-align: justify; }
)");
  EXPECT_EQ(parser.resolveStyle("p", "").textAlign, CssTextAlign::Justify);
}

TEST(CssParser, LoadFromStreamSkipsCommentsAndAtRules) {
  CssParser parser("");
  loadCss(parser, R"(
/* a comment with { braces } and ; semicolons */
@media screen { p { text-align: left; } }
@font-face { font-family: "Foo"; }
em { font-style: italic; }
)");
  // The @media block's nested p rule is part of the at-rule and gets skipped.
  EXPECT_FALSE(parser.resolveStyle("p", "").hasTextAlign());
  EXPECT_EQ(parser.resolveStyle("em", "").fontStyle, CssFontStyle::Italic);
}

TEST(CssParser, GroupedSelectorsApplyToAll) {
  CssParser parser("");
  loadCss(parser, "h1, h2, h3 { font-weight: bold; }");
  EXPECT_EQ(parser.resolveStyle("h1", "").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(parser.resolveStyle("h2", "").fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(parser.resolveStyle("h3", "").fontWeight, CssFontWeight::Bold);
}
