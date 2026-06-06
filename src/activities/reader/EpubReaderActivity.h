#pragma once
#include <Epub.h>
#include <Epub/SpineItem.h>
#include <Typesetter/LinkEntry.h>

#include <optional>
#include <vector>

#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<SpineItem> spineItem = nullptr;
  uint16_t currentSpineIndex = 0;
  uint16_t nextPageNumber = 0;
  // Set when navigating to a TOC entry in a different spine (chapter skip or chapter selector).
  // Cleared on the next render after the new spineItem loads and resolves it to a page.
  std::optional<uint16_t> pendingTocIndex;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new spineItem loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  uint16_t cachedSpineIndex = 0;
  uint16_t cachedChapterTotalPageCount = 0;
  // Signals that the next render should reposition within the newly loaded spine item
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool showBookmarkMessage = false;
  bool ignoreNextConfirmRelease = false;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  uint32_t bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;

  // Chapter-level page info aggregated across spine items sharing a TOC entry. `segments`
  // contains one Chapter per spine that the current chapter spans, in spine order. `tocIndex`
  // identifies which chapter the segments describe; nullopt means stale/uninitialized.
  // `title` is cached here so renderStatusBar doesn't have to call epub->getTocItem
  // (which does file I/O against BookMetadataCache) on every page. Use setChapter() to
  // mutate tocIndex+title atomically so they can't desync.
  struct ChapterPageInfo {
    std::optional<uint16_t> tocIndex;
    std::vector<Chapter> segments;
    std::string title;

    void setChapter(uint16_t newTocIndex, std::string newTitle) {
      tocIndex = newTocIndex;
      title = std::move(newTitle);
    }
  };
  ChapterPageInfo chapterPageInfo;

  // Footnote support
  // Interactive targets on the currently-rendered page. Sourced from
  // page->links; populated in render() after each loadPageFromSectionFile.
  // Footnote refs and any in-document anchor links share this vector
  // (parser-side classification doesn't matter to the picker).
  std::vector<LinkEntry> currentPageLinks;
  struct SavedPosition {
    uint16_t spineIndex;
    uint16_t pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(std::unique_ptr<Page> page, uint16_t orientedMarginTop, uint16_t orientedMarginRight,
                      uint16_t orientedMarginBottom, uint16_t orientedMarginLeft);
  void renderStatusBar() const;
  bool saveProgress(uint16_t spineIndex, uint16_t currentPage, uint16_t pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  // Load the current spineItem and build caches for all spine items in its TOC chapter.
  // Returns false if the current spineItem could not be loaded or built.
  bool prepareSpineItem(uint16_t viewportWidth, uint16_t viewportHeight);
  // Returns the chapter-relative page number for the current position. Computes a running
  // sum over chapterPageInfo.segments to find the segment matching currentSpineIndex, then
  // adds the in-spine offset. Falls back to spineItem->currentPage when no segment matches.
  uint16_t getChapterRelativePage() const;
  // Returns the total page count of the current chapter (sum of segment ranges), or the
  // current spineItem's pageCount when chapterPageInfo is empty (non-TOC spines, pre-load).
  uint16_t getChapterTotalPages() const;
  void pageTurn(bool isForwardTurn);
  void addBookmark();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
