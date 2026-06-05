#pragma once

#include <FunctionRef.h>
#include <Typesetter.h>
#include <Typesetter/LinkEntry.h>
#include <Typesetter/ParsedText.h>
#include <Typesetter/blocks/ImageBlock.h>
#include <Typesetter/blocks/TextBlock.h>
#include <expat.h>

#include <climits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"

class Page;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  Typesetter typesetter;
  FunctionRef<void()> popupFn;  // Popup callback (lifetime: caller's frame; see SpineItem::createCacheFile)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int underlineUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<bool> underline;
    std::optional<CssTextDirection> direction;
    std::optional<bool> sup;
    std::optional<bool> sub;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  bool effectiveUnderline = false;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveSup = false;
  bool effectiveSub = false;
  int tableDepth = 0;
  int tableRowIndex = 0;
  int tableColIndex = 0;

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on.
  // Page counter and xpath indices live in `typesetter` -- query via accessors.
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;          // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  LinkEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;

  void updateEffectiveInlineStyle();
  void startNewTextBlock(const BlockStyle& blockStyle);
  void flushPendingAnchor();
  void flushPartWordBuffer();
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer,
                                 const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight, const bool hyphenationEnabled,
                                 const bool focusReadingEnabled,
                                 FunctionRef<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePageFn,
                                 const bool embeddedStyle, const std::string& contentBase,
                                 const std::string& imageBasePath, const uint8_t imageRendering = 0,
                                 std::vector<std::string> tocAnchors = {}, FunctionRef<void()> popupFn = nullptr,
                                 const CssParser* cssParser = nullptr)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        typesetter(renderer, fontId, lineCompression, extraParagraphSpacing, viewportWidth, viewportHeight,
                   completePageFn),
        popupFn(popupFn),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser() = default;
  bool parseAndBuildPages();
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }
};
