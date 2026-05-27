#include "Typesetter.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

Typesetter::Typesetter(GfxRenderer& renderer, int fontId, float lineCompression, bool extraParagraphSpacing,
                       uint16_t viewportWidth, uint16_t viewportHeight, PageCompleteFn completePageFn)
    : renderer(renderer),
      fontId(fontId),
      lineCompression(lineCompression),
      extraParagraphSpacing(extraParagraphSpacing),
      viewportWidth(viewportWidth),
      viewportHeight(viewportHeight),
      completePageFn(completePageFn) {}

void Typesetter::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (!currentPage) {
    currentPage = makeUniqueNoThrow<Page>();
    if (!currentPage) {
      LOG_ERR("TYP", "OOM allocating initial page for line");
      return;
    }
    currentPageNextY = 0;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage = makeUniqueNoThrow<Page>();
    if (!currentPage) {
      LOG_ERR("TYP", "OOM allocating new page after line-fit break");
      return;
    }
    currentPageNextY = 0;
  }

  // Track cumulative words to attach per-page links to the page containing their anchor word.
  wordsExtractedInBlock += line->wordCount();
  auto linkIt = pendingLinks.begin();
  while (linkIt != pendingLinks.end() && linkIt->first <= wordsExtractedInBlock) {
    currentPage->addLink(linkIt->second.label, linkIt->second.href);
    ++linkIt;
  }
  pendingLinks.erase(pendingLinks.begin(), linkIt);

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY += lineHeight;
}

void Typesetter::submitParagraph(std::unique_ptr<ParsedText> paragraph) {
  if (!paragraph) {
    LOG_ERR("TYP", "Null paragraph submitted");
    return;
  }

  if (!currentPage) {
    currentPage = makeUniqueNoThrow<Page>();
    if (!currentPage) {
      LOG_ERR("TYP", "OOM allocating initial page for paragraph");
      return;
    }
    currentPageNextY = 0;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = paragraph->getBlockStyle();
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  paragraph->layoutAndExtractLines(renderer, fontId, effectiveWidth,
                                   [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });

  // Fallback: transfer any remaining pending links to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a link's word index equals the exact block size.
  if (!pendingLinks.empty() && currentPage) {
    for (const auto& [idx, link] : pendingLinks) {
      currentPage->addLink(link.label, link.href);
    }
    pendingLinks.clear();
  }

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

void Typesetter::partialFlush(ParsedText& block) {
  const int horizontalInset = block.getBlockStyle().totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  block.layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); }, false);
}

void Typesetter::submitImage(std::shared_ptr<ImageBlock> imageBlock, const int16_t marginTop,
                             const int16_t marginBottom) {
  if (!imageBlock) {
    LOG_ERR("TYP", "Null ImageBlock submitted");
    return;
  }

  const int16_t displayWidth = imageBlock->getWidth();
  const int16_t displayHeight = imageBlock->getHeight();

  // Page break only if the image plus its margins won't fit in the remaining space.
  if (currentPage && !currentPage->elements.empty() &&
      (currentPageNextY + marginTop + displayHeight + marginBottom > viewportHeight)) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage = makeUniqueNoThrow<Page>();
    if (!currentPage) {
      LOG_ERR("TYP", "OOM allocating new page after image-fit break");
      return;
    }
    currentPageNextY = 0;
  } else if (!currentPage) {
    currentPage = makeUniqueNoThrow<Page>();
    if (!currentPage) {
      LOG_ERR("TYP", "OOM allocating initial page for image");
      return;
    }
    currentPageNextY = 0;
  }

  currentPageNextY += marginTop;

  const int xPos = (viewportWidth - displayWidth) / 2;
  auto pageImage = std::make_shared<PageImage>(imageBlock, static_cast<int16_t>(xPos), currentPageNextY);
  if (!pageImage) {
    LOG_ERR("TYP", "Failed to create PageImage");
    return;
  }
  currentPage->elements.push_back(pageImage);
  currentPageNextY += displayHeight + marginBottom;
}

void Typesetter::submitHorizontalRule(const BlockStyle& blockStyle) {
  if (!currentPage) {
    currentPage = makeUniqueNoThrow<Page>();
    if (!currentPage) {
      LOG_ERR("TYP", "OOM allocating page for horizontal rule");
      return;
    }
    currentPageNextY = 0;
  }

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
  const int16_t defaultVerticalSpacing = static_cast<int16_t>(lineHeight / 2);
  const int16_t topSpacing =
      static_cast<int16_t>((blockStyle.marginTop > 0 ? blockStyle.marginTop : defaultVerticalSpacing) +
                           (blockStyle.paddingTop > 0 ? blockStyle.paddingTop : 0));
  const int16_t bottomSpacing =
      static_cast<int16_t>((blockStyle.marginBottom > 0 ? blockStyle.marginBottom : defaultVerticalSpacing) +
                           (blockStyle.paddingBottom > 0 ? blockStyle.paddingBottom : 0));
  constexpr uint8_t ruleThickness = 2;
  const int16_t availableWidth =
      std::max<int16_t>(1, static_cast<int16_t>(viewportWidth - blockStyle.totalHorizontalInset()));
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(availableWidth / 4));
  const int16_t xPos = static_cast<int16_t>(blockStyle.leftInset() + ((availableWidth - width) / 2));
  const int16_t totalHeight = static_cast<int16_t>(topSpacing + ruleThickness + bottomSpacing);

  if (!currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage = makeUniqueNoThrow<Page>();
    if (!currentPage) {
      LOG_ERR("TYP", "OOM allocating page after horizontal-rule break");
      return;
    }
    currentPageNextY = 0;
  }

  currentPageNextY += topSpacing;

  auto pageRule = makeUniqueNoThrow<PageHorizontalRule>(width, ruleThickness, xPos, currentPageNextY);
  if (!pageRule) {
    LOG_ERR("TYP", "OOM allocating PageHorizontalRule");
    return;
  }
  currentPage->elements.push_back(std::shared_ptr<PageHorizontalRule>(std::move(pageRule)));
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);
}

void Typesetter::forcePageBreak() {
  if (currentPage && !currentPage->elements.empty()) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage = makeUniqueNoThrow<Page>();
    if (!currentPage) {
      LOG_ERR("TYP", "OOM allocating new page after force break");
      return;
    }
    currentPageNextY = 0;
  }
}

void Typesetter::finish() {
  if (currentPage) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset();
  }
}
