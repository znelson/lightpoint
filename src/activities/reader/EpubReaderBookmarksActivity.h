#pragma once
#include <Epub.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "BookmarkEntry.h"
#include "BookmarkStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Bookmark list activity.
//
// Memory profile: only the bookmark count and a small sliding window of
// already-resolved entries live in RAM. Scrolling past the window reloads
// it lazily from BookmarkStore via fixed-record random access. Even at
// hundreds of bookmarks per book the activity holds < 2 KB resident.
class EpubReaderBookmarksActivity final : public Activity {
 public:
  // Max bookmarks held in RAM at once. Sized comfortably above the most rows
  // any orientation can fit on screen so the visible page is always
  // window-resident without needing to peek at neighbours.
  static constexpr uint16_t kMaxVisible = 16;

  // Resolved view of one bookmark entry: the on-disk record plus the small
  // amount of derived state we want for display (chapter title, page within
  // chapter, percent through book). Computed once per window load via ad-hoc
  // Section reads against the spine's cache file.
  struct EntryView {
    BookmarkEntry bookmark;
    uint16_t resolvedPage = 0;      // 0-based page within the chapter
    uint16_t chapterPageCount = 0;  // total pages in chapter at last cache
    std::string chapterTitle;       // empty string when no TOC entry maps
    bool valid = false;
  };

  explicit EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::shared_ptr<Epub>& epub, const std::string& epubPath)
      : Activity("EpubReaderBookmarks", renderer, mappedInput), epub(epub), epubPath(epubPath), store(epubPath) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  BookmarkStore store;
  uint16_t totalCount = 0;
  int selectorIndex = 0;
  int confirmingDelete = 0;  // 0 = hide dialog, 1 = show dialog, 2 = allow confirmation to delete

  // Sliding window: window[i] holds the entry at logical index windowStart+i
  // for i in [0, windowCount). EntryView::valid is false for unfilled slots
  // (only relevant if the on-disk count shrank during the activity, which
  // can't happen in practice).
  std::array<EntryView, kMaxVisible> window;
  uint16_t windowStart = 0;
  uint16_t windowCount = 0;

  // Reload the window centered on selectorIndex. Called on entry, after a
  // delete, and whenever selectorIndex moves outside the current window.
  void reloadWindow();

  // True if logical index is within the current window range.
  bool inWindow(int logicalIndex) const;

  // Pointer to the resolved view for a logical index, or nullptr if it's
  // outside the window. drawList only requests visible rows, so this is
  // always non-null in practice for the rows we care about.
  const EntryView* viewForIndex(int logicalIndex) const;

  // Resolve display fields for a raw bookmark entry by reading the spine's
  // section cache. Best-effort: if the cache is missing or stale, the
  // returned view has chapterPageCount = 0 and resolvedPage = 0.
  EntryView resolveEntry(const BookmarkEntry& entry) const;

  // Calculate the vertical space to reserve for button hints based on orientation
  int getGutterBottom(const GfxRenderer& renderer);

  // Calculate the height available for the bookmark list based on orientation
  int getListHeight(const GfxRenderer& renderer);
};
