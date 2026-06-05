#pragma once

#include <FunctionRef.h>
#include <Typesetter/LinkEntry.h>
#include <Typesetter/Page.h>
#include <Typesetter/ParsedText.h>
#include <Typesetter/blocks/BlockStyle.h>
#include <Typesetter/blocks/ImageBlock.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

class GfxRenderer;

// Format-agnostic layout engine. Consumes paragraphs / images / horizontal
// rules / page-break requests from a parser, lays them out within the given
// viewport, and emits completed Page objects via the page-completion callback.
//
// Stateful: holds the current page being filled, the running y-cursor, the
// queued footnotes for the current page, and word/xpath counters that the
// parser coordinates against for anchor recording. The parser drives input
// via submit*/finish; it observes state via the small accessor surface near
// the bottom of the class.
class Typesetter {
 public:
  using PageCompleteFn = FunctionRef<void(std::unique_ptr<Page>, uint16_t, uint16_t)>;

  Typesetter(GfxRenderer& renderer, int fontId, float lineCompression, bool extraParagraphSpacing,
             uint16_t viewportWidth, uint16_t viewportHeight, PageCompleteFn completePageFn);
  ~Typesetter() = default;

  // Lay out a paragraph: applies top/bottom margins, breaks into lines that
  // fit the effective width, emits pages as they fill. Consumes `paragraph`.
  void submitParagraph(std::unique_ptr<ParsedText> paragraph);

  // Lay out a paragraph in progress without finalizing it; consumes all
  // produced lines, leaving the last unfinished line in the block. Used for
  // very long blocks (>750 words) to keep memory bounded.
  void partialFlush(ParsedText& block);

  // Place an image with the given vertical margins. Forces a page break if
  // the image plus its margins won't fit in the remaining viewport.
  void submitImage(std::shared_ptr<ImageBlock> imageBlock, int16_t marginTop, int16_t marginBottom);

  // Emit a horizontal rule with surrounding spacing derived from `blockStyle`.
  // Width is fixed at a quarter of the effective viewport width, centered.
  void submitHorizontalRule(const BlockStyle& blockStyle);

  // Force the current page to complete if it has any content. Used by the
  // parser at TOC chapter boundaries so chapters start on a fresh page.
  void forcePageBreak();

  // Emit any in-progress page. Call once after all submit* calls; subsequent
  // input would build a new initial page.
  void finish();

  // Parser-side coordination accessors. The parser tracks xpath indices and
  // queues footnotes; Typesetter consumes them when laying out lines.
  void incrementXpathParagraphIndex() { ++xpathParagraphIndex; }
  void incrementXpathListItemIndex() { ++xpathListItemIndex; }
  // Queue a per-page link target with the word index at which it appears
  // within the current block. submitParagraph drains entries whose index
  // has been emitted, attaching them to the page that contains that word.
  // Used identically by EPUB (footnote refs) and Markdown (inline links).
  void addPendingLink(size_t wordIndex, const LinkEntry& entry) { pendingLinks.push_back({wordIndex, entry}); }
  size_t getWordsExtractedInBlock() const { return wordsExtractedInBlock; }
  size_t getCompletedPageCount() const { return completedPageCount; }

 private:
  // Resets per-paragraph word counter at the end of submitParagraph. Not
  // exposed: parsers should treat submit/partialFlush as the only block
  // lifecycle hooks; the typesetter owns its bookkeeping.
  void resetWordsExtractedInBlock() { wordsExtractedInBlock = 0; }

  GfxRenderer& renderer;
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint16_t viewportWidth;
  uint16_t viewportHeight;

  std::unique_ptr<Page> currentPage;
  int16_t currentPageNextY = 0;
  size_t completedPageCount = 0;

  PageCompleteFn completePageFn;

  std::vector<std::pair<size_t, LinkEntry>> pendingLinks;
  size_t wordsExtractedInBlock = 0;
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;

  void addLineToPage(std::shared_ptr<TextBlock> line);
};
