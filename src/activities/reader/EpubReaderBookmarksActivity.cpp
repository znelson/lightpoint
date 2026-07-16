#include "EpubReaderBookmarksActivity.h"

#include <Epub/SpineItem.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <optional>
#include <string>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ENTER_DELETE_MODE_MS = 700;
constexpr int DELETE_MODE_OFF = 0;
constexpr int DELETE_MODE_DISPLAY = 1;
constexpr int DELETE_MODE_CONFIRM = 2;

constexpr int LINE_HEIGHT = 60;
}  // namespace

void EpubReaderBookmarksActivity::onEnter() {
  Activity::onEnter();
  if (!epub) return;

  totalCount = store.count();
  selectorIndex = 0;
  windowStart = 0;
  windowCount = 0;
  reloadWindow();
  LOG_DBG("EPB", "Loaded bookmark count=%u for: %s", totalCount, epubPath.c_str());
  requestUpdate();
}

int EpubReaderBookmarksActivity::getGutterBottom(const GfxRenderer& renderer) {
  const auto orientation = renderer.getOrientation();
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  return isPortrait ? 75 : 40;
}

int EpubReaderBookmarksActivity::getListHeight(const GfxRenderer& renderer) {
  const auto pageHeight = renderer.getScreenHeight();
  return pageHeight - getGutterBottom(renderer) - LINE_HEIGHT;
}

bool EpubReaderBookmarksActivity::inWindow(uint16_t logicalIndex) const {
  return logicalIndex >= windowStart && logicalIndex < windowStart + windowCount;
}

const EpubReaderBookmarksActivity::EntryView* EpubReaderBookmarksActivity::viewForIndex(uint16_t logicalIndex) const {
  if (!inWindow(logicalIndex)) return nullptr;
  const auto& view = window[logicalIndex - windowStart];
  return view.valid ? &view : nullptr;
}

EpubReaderBookmarksActivity::EntryView EpubReaderBookmarksActivity::resolveEntry(const BookmarkEntry& entry) const {
  EntryView view;
  view.bookmark = entry;
  view.valid = true;

  // Read-only load: never validates render params or removes the cache, so
  // browsing bookmarks can't disturb the spine caches the reader relies on.
  SpineItem spine(epub, entry.spineIndex, renderer);
  if (!spine.loadForQuery()) {
    // Cache missing or stale: fall back to a best-effort spine-level title and
    // leave the page/percent readout suppressed (spinePageCount stays 0).
    const auto tocIdx = epub->getTocIndexForSpineIndex(entry.spineIndex);
    view.chapterTitle = tocIdx ? epub->getTocItem(*tocIdx).title : "";
    return view;
  }
  view.spinePageCount = spine.getPageCount();

  // Resolve paragraphIndex (with optional liIndex refinement) to a spine page.
  // liIndex first-appearance can land on a later page than the paragraph's
  // first-appearance when the bookmark sits inside a list that spans pages;
  // taking max keeps the resolution from regressing past the paragraph start.
  uint16_t spinePage = spine.getPageForParagraphIndex(entry.paragraphIndex).value_or(0);
  if (entry.liIndex != BookmarkEntry::NO_LI_INDEX) {
    if (const auto liPage = spine.getPageForListItemIndex(entry.liIndex)) {
      spinePage = std::max(spinePage, *liPage);
    }
  }
  view.spinePage = spinePage;

  // Page-accurate chapter title: getTocIndexForPage walks this spine's
  // anchor-derived boundaries, so a bookmark in a later sub-chapter of a
  // multi-chapter spine resolves to that sub-chapter, not the spine's first.
  const auto tocIdx = spine.getTocIndexForPage(spinePage);
  view.chapterTitle = tocIdx ? epub->getTocItem(*tocIdx).title : "";

  // Chapter-relative page readout. getPageRangeForTocIndex gives the chapter's
  // span within this spine: for single-spine chapters it matches the reader's
  // status bar exactly. For a chapter spanning multiple spines it reflects only
  // this spine's portion -- resolving the full span would mean loading every
  // sibling spine, too much SD I/O for a list that resolves a window at a time.
  const auto range = tocIdx ? spine.getPageRangeForTocIndex(*tocIdx) : std::nullopt;
  if (range && range->endPage > range->startPage && spinePage >= range->startPage) {
    view.chapterPage = spinePage - range->startPage;
    view.chapterPageCount = range->endPage - range->startPage;
  } else {
    view.chapterPage = spinePage;
    view.chapterPageCount = view.spinePageCount;
  }
  return view;
}

void EpubReaderBookmarksActivity::reloadWindow() {
  if (totalCount == 0) {
    windowStart = 0;
    windowCount = 0;
    for (auto& v : window) v.valid = false;
    return;
  }

  // Center the window on the current selector position, clamped to the
  // bookmark count. Falls back to a window starting at 0 when totalCount
  // is below kMaxVisible.
  const uint16_t half = kMaxVisible / 2;
  uint16_t start = (selectorIndex > half) ? selectorIndex - half : 0;
  if (start + kMaxVisible > totalCount) {
    start = (totalCount > kMaxVisible) ? totalCount - kMaxVisible : 0;
  }
  const uint16_t end = std::min<uint16_t>(start + kMaxVisible, totalCount);

  windowStart = start;
  windowCount = end - start;
  for (uint16_t i = 0; i < windowCount; ++i) {
    if (auto entry = store.entryAt(start + i)) {
      window[i] = resolveEntry(*entry);
    } else {
      window[i].valid = false;
    }
  }
  for (uint16_t i = windowCount; i < kMaxVisible; ++i) {
    window[i].valid = false;
  }
}

void EpubReaderBookmarksActivity::loop() {
  // Delete confirmation mode.
  if (confirmingDelete >= DELETE_MODE_DISPLAY) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (confirmingDelete == DELETE_MODE_DISPLAY) {
        confirmingDelete = DELETE_MODE_CONFIRM;
        requestUpdate();
        return;
      }
      if (!store.eraseAt(selectorIndex)) {
        LOG_ERR("EPB", "Failed to delete bookmark at %u", selectorIndex);
      }
      totalCount = store.count();
      if (selectorIndex >= totalCount && selectorIndex > 0) selectorIndex--;
      reloadWindow();
      confirmingDelete = DELETE_MODE_OFF;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      confirmingDelete = DELETE_MODE_OFF;
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (totalCount == 0) return;
    const auto* view = viewForIndex(selectorIndex);
    if (!view) {
      LOG_ERR("EPB", "Confirm: selector %u outside window [%u,%u)", selectorIndex, windowStart,
              static_cast<unsigned>(windowStart + windowCount));
      return;
    }
    setResult(PositionResult{view->bookmark.spineIndex, view->spinePage});
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() > ENTER_DELETE_MODE_MS) {
    if (totalCount == 0) return;
    confirmingDelete = DELETE_MODE_DISPLAY;
    requestUpdate();
  }

  // Navigation: track selectorIndex and only reload when it leaves the window.
  const auto onSelectorChanged = [this] {
    if (!inWindow(selectorIndex)) reloadWindow();
    requestUpdate();
  };

  buttonNavigator.onNextRelease([this, &onSelectorChanged] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalCount);
    onSelectorChanged();
  });
  buttonNavigator.onPreviousRelease([this, &onSelectorChanged] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalCount);
    onSelectorChanged();
  });
  buttonNavigator.onNextContinuous([this, &onSelectorChanged] {
    selectorIndex =
        ButtonNavigator::nextPageIndex(selectorIndex, totalCount, GUI.getListPageItems(getListHeight(renderer), true));
    onSelectorChanged();
  });
  buttonNavigator.onPreviousContinuous([this, &onSelectorChanged] {
    selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalCount,
                                                       GUI.getListPageItems(getListHeight(renderer), true));
    onSelectorChanged();
  });
}

void EpubReaderBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const bool isPortrait = orientation == GfxRenderer::Orientation::Portrait;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 40 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int hintGutterBottom = getGutterBottom(renderer);
  const int contentY = hintGutterHeight;
  const int listY = contentY + LINE_HEIGHT;
  const int listHeight = getListHeight(renderer);
  const int numBookmarks = static_cast<int>(totalCount);

  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  const auto getBookmarkTitle = [this](int index) -> std::string {
    const uint16_t target = (confirmingDelete >= DELETE_MODE_DISPLAY) ? selectorIndex : index;
    const auto* view = viewForIndex(target);
    return view ? view->bookmark.summary : "";
  };
  const auto getBookmarkSubtitle = [this](int index) -> std::string {
    const uint16_t target = (confirmingDelete >= DELETE_MODE_DISPLAY) ? selectorIndex : index;
    const auto* view = viewForIndex(target);
    if (!view) return "";
    const std::string title = view->chapterTitle.empty() ? std::string(tr(STR_UNNAMED)) : view->chapterTitle;
    if (view->spinePageCount == 0) return title;
    const int percent = static_cast<int>(
        epub->calculateProgressForPage(view->bookmark.spineIndex, view->spinePage, view->spinePageCount) * 100.0f +
        0.5f);
    return std::to_string(percent) + "% - " + std::to_string(view->chapterPage + 1) + "/" +
           std::to_string(view->chapterPageCount) + " - " + title;
  };
  const auto getBookmarkIcon = [isPortrait](int /*index*/) { return isPortrait ? UIIcon::Bookmark : UIIcon::None; };

  if (numBookmarks > 0) {
    if (confirmingDelete >= DELETE_MODE_DISPLAY) {
      GUI.drawHelpText(renderer, Rect{0, pageHeight / 2 - LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                       tr(STR_CONFIRM_DELETE_BOOKMARK));
      GUI.drawList(renderer, Rect{contentX, pageHeight / 2, contentWidth, LINE_HEIGHT}, 1, 0, getBookmarkTitle,
                   getBookmarkSubtitle, getBookmarkIcon);
    } else {
      GUI.drawList(renderer, Rect{contentX, listY, contentWidth, listHeight}, numBookmarks, selectorIndex,
                   getBookmarkTitle, getBookmarkSubtitle, getBookmarkIcon);
      GUI.drawHelpText(renderer, Rect{contentX, pageHeight - hintGutterBottom, contentWidth, LINE_HEIGHT},
                       tr(STR_HOLD_CONFIRM_TO_DELETE));
    }
  } else {
    GUI.drawHelpText(renderer, Rect{contentX, LINE_HEIGHT * 2, contentWidth, LINE_HEIGHT},
                     tr(STR_BOOKMARK_INSTRUCTIONS));
  }

  const auto backLabel = confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_CANCEL) : tr(STR_BACK);
  const auto confirmLabel =
      totalCount > 0 ? (confirmingDelete >= DELETE_MODE_DISPLAY ? tr(STR_DELETE) : tr(STR_OPEN)) : "";
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
