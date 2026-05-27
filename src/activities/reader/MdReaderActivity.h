#pragma once

#include <Txt.h>
#include <Typesetter/LinkEntry.h>
#include <Typesetter/Section.h>

#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

class Page;

// MdReaderActivity -- sibling of TxtReaderActivity for Markdown files.
//
// Reuses the Txt class as the file-read abstraction (`.md` files are still
// plain byte streams) and the same Typesetter + Section pipeline; the only
// substantive difference is the build phase: MarkdownParser interprets
// headings, emphasis, lists, code blocks instead of the chunked-text
// paragraph extractor in TxtReaderActivity.
//
// Kept as a separate activity rather than fanning Markdown into
// TxtReaderActivity by extension. Markdown's format-specific UX (heading
// TOC anchors, inline images, possibly per-format settings) is going to
// diverge over time; the codebase already pairs each format with its own
// activity (Epub, Txt, Xtc).
class MdReaderActivity final : public Activity {
  std::unique_ptr<Txt> md;

  int currentPage = 0;
  int pagesUntilFullRefresh = 0;
  bool initialized = false;

  Section cache;

  int cachedFontId = 0;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  // Interactive link targets for the currently-rendered page. Populated
  // from page->links after each loadPage; consumed by the Confirm-button
  // handler to invoke ReaderLinkPickerActivity. Picker result resolves
  // #anchor hrefs via cache.getPageForAnchor(...) to a page jump.
  std::vector<LinkEntry> currentPageLinks;

  void renderContents(std::unique_ptr<Page> page);
  void renderStatusBar() const;

  void initializeReader();
  bool buildSectionCache(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress() const;
  void loadProgress();

 public:
  explicit MdReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> md)
      : Activity("MdReader", renderer, mappedInput),
        md(std::move(md)),
        cache(this->md->getCachePath() + "/section.bin") {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
