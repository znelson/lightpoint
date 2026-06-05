#include "EpubReaderActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalPlatform.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Typesetter/Page.h>
#include <Typesetter/blocks/TextBlock.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <string_view>

#include "BookmarkEntry.h"
#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "MappedInputManager.h"
#include "ReaderLinkPickerActivity.h"
#include "ReaderPercentSelectionActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// Walk words on the current spine page and assemble a single-space-separated
// summary that fits within SUMMARY_MAX bytes. Stops as soon as the next word
// (with its leading space) would exceed the cap. Typesetter tokenization
// already strips whitespace from words, so no further sanitization is needed.
std::string buildBookmarkSummary(SpineItem& spineItem) {
  std::string out;
  out.reserve(BookmarkStore::SUMMARY_MAX);
  spineItem.forEachWordOnCurrentPage([&out](std::string_view word) {
    const size_t need = out.empty() ? word.size() : word.size() + 1;
    if (out.size() + need > BookmarkStore::SUMMARY_MAX) return false;
    if (!out.empty()) out += ' ';
    out.append(word);
    return true;
  });
  return out;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  halStorage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!halStorage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (halStorage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!halStorage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && halStorage.exists(oldCachePath.c_str())) {
    if (!halStorage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  HalFile f;
  if (halStorage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    const uint16_t textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %u", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  chapterPageInfo.tocIndex.reset();
  spineItem.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  if (showBookmarkMessage &&
      (halPlatform.millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      const int currentPage = spineItem ? getChapterRelativePage() + 1 : 0;
      const int totalPages = getChapterTotalPages();
      float bookProgress = 0.0f;
      if (epub->getBookSize() > 0 && spineItem && spineItem->getPageCount() > 0) {
        const float chapterProgress =
            static_cast<float>(spineItem->currentPage) / static_cast<float>(spineItem->getPageCount());
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, epub->getTitle(),
                                                                      currentPage, totalPages, bookProgressPercent,
                                                                      SETTINGS.orientation, !currentPageLinks.empty()),
                             [this](const ActivityResult& result) {
                               // Always apply orientation change even if the menu was cancelled
                               const auto& menu = std::get<MenuResult>(result.data);
                               applyOrientation(menu.orientation);
                               if (!result.isCancelled && menu.action.has_value()) {
                                 onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(*menu.action));
                               }
                             });
    }
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
    if (!showBookmarkMessage) {
      addBookmark();
      showBookmarkMessage = true;
      ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
      bookmarkMessageTime = halPlatform.millis();
      requestUpdate();
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (halGPIO.wasReleased(HalGPIO::BTN_POWER) && halGPIO.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  // Chapter skip navigates by TOC entries, not spine boundaries.
  // Spine items without their own TOC entry inherit the previous spine's tocIndex
  // (see BookMetadataCache), so they're treated as continuations of the last chapter.
  // At the boundaries: skipping forward past the last TOC entry jumps to end-of-book
  // (clamped in render()); skipping backward before the first TOC entry jumps to the
  // spine before the current chapter's first spine (clamped to 0 in render()).
  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    // We don't want to delete the spineItem mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      if (spineItem && spineItem->getPageCount() > 0) {
        const auto curTocIndex = spineItem->getTocIndexForPage(spineItem->currentPage);
        if (!curTocIndex) {
          // No TOC entry for this spine, fall back to spine-level skip
          nextPageNumber = 0;
          if (nextTriggered) {
            currentSpineIndex++;
          } else if (currentSpineIndex > 0) {
            currentSpineIndex--;
          }
          spineItem.reset();
        } else {
          const int nextTocIndex = nextTriggered ? *curTocIndex + 1 : *curTocIndex - 1;
          if (nextTocIndex >= 0 && nextTocIndex < epub->getTocItemsCount()) {
            const auto newSpineIndex = epub->getSpineIndexForTocIndex(nextTocIndex);
            if (newSpineIndex == currentSpineIndex) {
              if (const auto resolvedPage = spineItem->getPageForTocIndex(nextTocIndex)) {
                spineItem->currentPage = *resolvedPage;
              } else {
                LOG_DBG("ERS", "No page boundary for TOC %d in spine %d, staying on current page", nextTocIndex,
                        currentSpineIndex);
              }
            } else if (newSpineIndex) {
              pendingTocIndex = nextTocIndex;
              nextPageNumber = 0;
              currentSpineIndex = *newSpineIndex;
              spineItem.reset();
            }
          } else if (nextTriggered) {
            // Beyond last TOC entry, go to end of book
            nextPageNumber = 0;
            currentSpineIndex = epub->getSpineItemsCount();
            spineItem.reset();
          } else {
            // Before first TOC entry, skip to spine before the current chapter.
            // If the current TOC entry didn't resolve to a spine (malformed EPUB)
            // or its spine is already 0, fall back to spine 0 (start of book).
            nextPageNumber = 0;
            const auto curTocSpine = epub->getTocItem(*curTocIndex).spineIndex;
            currentSpineIndex = (curTocSpine && *curTocSpine > 0) ? *curTocSpine - 1 : 0;
            spineItem.reset();
          }
        }
      } else {
        nextPageNumber = 0;
        if (nextTriggered) {
          currentSpineIndex++;
        } else if (currentSpineIndex > 0) {
          currentSpineIndex--;
        }
        spineItem.reset();
      }
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current spineItem, attempt to rerender the book
  if (!spineItem) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the spineItem is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const uint16_t spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  uint16_t targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (uint16_t i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    chapterPageInfo.tocIndex.reset();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    spineItem.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const auto tocIdx = spineItem ? spineItem->getTocIndexForPage(spineItem->currentPage)
                                    : epub->getTocIndexForSpineIndex(currentSpineIndex);
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx, tocIdx),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            RenderLock lock(*this);
            const auto& chapter = std::get<ChapterTarget>(result.data);
            const bool sameSpine = chapter.spineIndex == currentSpineIndex && spineItem;
            auto resolvedPage =
                (chapter.tocIndex && sameSpine) ? spineItem->getPageForTocIndex(*chapter.tocIndex) : std::nullopt;
            if (resolvedPage) {
              spineItem->currentPage = *resolvedPage;
            } else if (sameSpine) {
              spineItem->currentPage = 0;
            } else {
              pendingTocIndex = chapter.tocIndex;
              currentSpineIndex = chapter.spineIndex;
              nextPageNumber = 0;
              spineItem.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(
          std::make_unique<ReaderLinkPickerActivity>(renderer, mappedInput, currentPageLinks, tr(STR_FOOTNOTES)),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& linkResult = std::get<LinkResult>(result.data);
              navigateToHref(linkResult.href, true);
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && spineItem && spineItem->getPageCount() > 0) {
        const float chapterProgress =
            static_cast<float>(spineItem->currentPage) / static_cast<float>(spineItem->getPageCount());
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(std::make_unique<ReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 jumpToPercent(std::get<PercentResult>(result.data).percent);
                               }
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && spineItem) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = spineItem->currentPage;
          uint16_t backupPageCount = spineItem->getPageCount();
          chapterPageInfo.tocIndex.reset();
          spineItem.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            const auto& pos = std::get<PositionResult>(result.data);
            if (currentSpineIndex != pos.spineIndex || (spineItem && spineItem->currentPage != pos.page)) {
              RenderLock lock(*this);
              currentSpineIndex = pos.spineIndex;
              nextPageNumber = pos.page;
              spineItem.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
      break;
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (spineItem) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = spineItem->getPageCount();
      nextPageNumber = spineItem->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset spineItem to force re-layout in the new orientation.
    chapterPageInfo.tocIndex.reset();
    spineItem.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    if (spineItem->currentPage < spineItem->getPageCount() - 1) {
      spineItem->currentPage++;
    } else {
      // We don't want to delete the spineItem mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        spineItem.reset();
      }
    }
  } else {
    if (spineItem->currentPage > 0) {
      spineItem->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the spineItem mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        spineItem.reset();
      }
    }
  }
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // based bounds of book, show end of book screen. currentSpineIndex is uint16_t
  // so cannot go below 0; navigation sites guard prev-from-0 explicitly.
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!spineItem) {
    if (!prepareSpineItem(viewportWidth, viewportHeight)) {
      LOG_ERR("ERS", "Failed to persist page data to SD");
      return;
    }

    const auto pageCount = spineItem->getPageCount();
    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= pageCount && pageCount > 0) {
        spineItem->currentPage = pageCount - 1;
      } else {
        spineItem->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      spineItem->currentPage = nextPageNumber;
      if (spineItem->currentPage >= pageCount && pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %u to %u", spineItem->currentPage, pageCount - 1);
        spineItem->currentPage = pageCount - 1;
      }
    }

    if (pendingTocIndex) {
      cachedChapterTotalPageCount = 0;
      if (const auto resolvedPage = spineItem->getPageForTocIndex(*pendingTocIndex)) {
        spineItem->currentPage = *resolvedPage;
      }
      pendingTocIndex.reset();
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = spineItem->getPageForAnchor(pendingAnchor)) {
        spineItem->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in spineItem %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(spineItem->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        spineItem->currentPage = static_cast<uint16_t>(progress * pageCount);
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && pageCount > 0) {
      // Apply the pending percent jump now that we know the new spineItem's page count.
      uint16_t newPage = static_cast<uint16_t>(pendingSpineProgress * static_cast<float>(pageCount));
      if (newPage >= pageCount) {
        newPage = pageCount - 1;
      }
      spineItem->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  // Update chapter page info when the current page's TOC index changes (e.g. navigating
  // between sub-chapters within the same spine via page turns or chapter skip).
  // prepareSpineItem uses the spine-level TOC index since the spineItem isn't loaded yet;
  // this check uses the per-page index once the spineItem is available.
  // spineItem is guaranteed non-null here: prepareSpineItem returns false (early exit above)
  // if it can't load the spineItem, and the else branch only runs when spineItem already exists.
  const auto pageTocIndex = spineItem->getTocIndexForPage(spineItem->currentPage);
  const auto pageCount = spineItem->getPageCount();
  if (pageTocIndex && chapterPageInfo.tocIndex != pageTocIndex) {
    // Recompute chapter info for the new sub-chapter. Since we're on the same spine,
    // no spineItem reload is needed, just update the page range aggregation.
    chapterPageInfo.setChapter(*pageTocIndex, epub->getTocItem(*pageTocIndex).title);
    chapterPageInfo.segments.clear();
    auto range = spineItem->getPageRangeForTocIndex(*pageTocIndex);
    if (!range) range = Chapter{*pageTocIndex, currentSpineIndex, 0, pageCount};
    chapterPageInfo.segments.push_back(*range);
  }

  renderer.clearScreen();

  if (pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    return;
  }

  if (spineItem->currentPage >= pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %u (max %u)", spineItem->currentPage, pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    return;
  }

  {
    auto p = spineItem->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing spineItem cache");
      spineItem->clearCache();
      spineItem.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      return;
    }

    // Collect footnotes from the loaded page
    currentPageLinks = std::move(p->links);

    [[maybe_unused]] const auto start = halPlatform.millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", halPlatform.millis() - start);
  }
  saveProgress(currentSpineIndex, spineItem->currentPage, pageCount);

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, tr(STR_BOOKMARK_ADDED));
  }
}

bool EpubReaderActivity::saveProgress(uint16_t spineIndex, uint16_t currentPage, uint16_t pageCount) {
  HalFile f;
  if (!halStorage.openFileForWrite("ERS", epub->getCachePath() + "/progress.bin", f)) {
    LOG_ERR("ERS", "Could not open progress file for write!");
    return false;
  }
  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = currentPage & 0xFF;
  data[3] = (currentPage >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving progress: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%u page=%u", spineIndex, currentPage);
  return true;
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  [[maybe_unused]] const auto t0 = halPlatform.millis();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  [[maybe_unused]] const auto tPrewarm = halPlatform.millis();

  // Force special handling for pages with images when anti-aliasing is on
  bool imagePageWithAA = page->hasImages() && SETTINGS.textAntiAliasing;

  page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  [[maybe_unused]] const auto tBwRender = halPlatform.millis();

  if (imagePageWithAA) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  [[maybe_unused]] const auto tDisplay = halPlatform.millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (SETTINGS.textAntiAliasing && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      [[maybe_unused]] const auto tGrayLsb = halPlatform.millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      [[maybe_unused]] const auto tGrayMsb = halPlatform.millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      [[maybe_unused]] const auto tGrayDisplay = halPlatform.millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      [[maybe_unused]] const auto tCleanup = halPlatform.millis();

      [[maybe_unused]] const auto tEnd = halPlatform.millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%ums bw_render=%ums display=%ums gray_lsb=%ums "
              "gray_msb=%ums gray_display=%ums cleanup=%ums total=%ums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (SETTINGS.textAntiAliasing) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      renderer.storeBwBuffer();
      [[maybe_unused]] const auto tBwStore = halPlatform.millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.copyGrayscaleLsbBuffers();
      [[maybe_unused]] const auto tGrayLsb = halPlatform.millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      page->render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop);
      renderer.copyGrayscaleMsbBuffers();
      [[maybe_unused]] const auto tGrayMsb = halPlatform.millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      [[maybe_unused]] const auto tGrayDisplay = halPlatform.millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      [[maybe_unused]] const auto tBwRestore = halPlatform.millis();

      [[maybe_unused]] const auto tEnd = halPlatform.millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%ums bw_render=%ums display=%ums bw_store=%ums "
              "gray_lsb=%ums gray_msb=%ums gray_display=%ums bw_restore=%ums total=%ums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No anti-aliasing: BW frame already displayed above, no grayscale to
      // render, so no save/restore.
      [[maybe_unused]] const auto tEnd = halPlatform.millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = spineItem->currentPage + 1;
  const float pageCount = spineItem->getPageCount();
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  // Use chapter-relative page counts when available
  const int chapterPage = getChapterRelativePage() + 1;
  const int chapterTotal = getChapterTotalPages();

  std::string title;

  int textYOffset = 0;

  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    // spineItem->getTocIndexForPage distinguishes pre-TOC orphans (nullopt -> "Unnamed")
    // from post-TOC orphans (inherited tocIndex -> show inheriting chapter's title).
    // chapterPageInfo.title is kept in sync by setChapter at the per-page TOC update
    // and prepareSpineItem, so we don't pay file I/O on the hot path here.
    const auto tocIndex = spineItem ? spineItem->getTocIndexForPage(spineItem->currentPage)
                                    : epub->getTocIndexForSpineIndex(currentSpineIndex);
    title = tocIndex ? chapterPageInfo.title : tr(STR_UNNAMED);
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, chapterPage, chapterTotal, title, 0, textYOffset);
}

bool EpubReaderActivity::prepareSpineItem(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  const auto filepath = epub->getSpineItem(currentSpineIndex).href;
  LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
  spineItem = std::make_unique<SpineItem>(epub, currentSpineIndex, renderer);

  const int fontId = SETTINGS.getReaderFontId();
  const float lineCompression = SETTINGS.getReaderLineCompression();
  const bool extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  const uint8_t paragraphAlignment = SETTINGS.paragraphAlignment;
  const bool hyphenationEnabled = SETTINGS.hyphenationEnabled;
  const bool embeddedStyle = SETTINGS.embeddedStyle;
  const uint8_t imageRendering = SETTINGS.imageRendering;
  const bool focusReadingEnabled = SETTINGS.focusReadingEnabled;

  const auto tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);

  // When entering a new TOC chapter, walk the full spine range in a single pass:
  // load or build each spine's cache, collecting page counts into chapterPageInfo.
  // For the current spine, load directly into `spineItem` so it's ready for rendering.
  // Skipped for same-chapter spine transitions (chapterPageInfo already populated)
  // and for spines not belonging to any TOC entry (tocIndex == nullopt).
  if (tocIndex && chapterPageInfo.tocIndex != tocIndex) {
    const auto spineRange = epub->getSpineRangeForTocIndex(*tocIndex);
    if (spineRange) {
      const uint16_t firstSpine = spineRange->first;
      const uint16_t lastSpine = spineRange->last;
      const uint16_t totalSpines = static_cast<uint16_t>(lastSpine - firstSpine + 1);

      chapterPageInfo.setChapter(*tocIndex, epub->getTocItem(*tocIndex).title);
      chapterPageInfo.segments.clear();
      chapterPageInfo.segments.reserve(totalSpines);

      uint16_t loopIndex = firstSpine;
      const auto popupFn = [this, &loopIndex, firstSpine, totalSpines]() {
        if (totalSpines == 1) {
          GUI.drawPopup(renderer, tr(STR_INDEXING));
        } else {
          char buf[48];
          snprintf(buf, sizeof(buf), "%s (%d/%d)", tr(STR_INDEXING), loopIndex - firstSpine + 1, totalSpines);
          GUI.drawPopup(renderer, buf);
        }
      };

      for (; loopIndex <= lastSpine; loopIndex++) {
        // For every spine, we need a loaded SpineItem to get accurate page ranges
        // via getPageRangeForTocIndex (which requires tocBoundaries).
        // The current spine loads into `spineItem`; siblings use a stack-allocated temporary.
        std::optional<SpineItem> tmp;
        SpineItem* spi;

        if (loopIndex == currentSpineIndex) {
          spi = spineItem.get();
        } else {
          tmp.emplace(epub, loopIndex, renderer);
          spi = &*tmp;
        }

        if (!spi->loadCacheFile(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                                viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering,
                                focusReadingEnabled)) {
          if (!spi->createCacheFile(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                                    viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering,
                                    focusReadingEnabled, popupFn)) {
            LOG_ERR("ERS", "Failed to build spineItem cache for spine %d", loopIndex);
            continue;
          }
        }

        // Collect page range for this spine's contribution to the chapter
        auto range = spi->getPageRangeForTocIndex(*tocIndex);
        if (!range) range = Chapter{*tocIndex, loopIndex, 0, spi->getPageCount()};
        chapterPageInfo.segments.push_back(*range);
      }

      [[maybe_unused]] const uint16_t totalPages =
          std::accumulate(chapterPageInfo.segments.begin(), chapterPageInfo.segments.end(), uint16_t{0},
                          [](uint16_t sum, const Chapter& ch) -> uint16_t { return sum + ch.endPage - ch.startPage; });
      LOG_DBG("ERS", "Chapter %d: %d spines (%d-%d), %d total pages", *tocIndex, totalSpines, firstSpine, lastSpine,
              totalPages);
    }
  }

  // If the chapter walk above loaded/built the current spine, we're done.
  if (spineItem->getPageCount() > 0) return true;

  // Fallback for same-chapter re-entry (chapterPageInfo already valid) or non-TOC
  // spines: just load or build this one spineItem.
  if (spineItem->loadCacheFile(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                               viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering,
                               focusReadingEnabled)) {
    return true;
  }

  const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };
  if (spineItem->createCacheFile(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                                 viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled,
                                 popupFn)) {
    return true;
  }

  spineItem.reset();
  return false;
}

uint16_t EpubReaderActivity::getChapterRelativePage() const {
  uint16_t chapterPagesBefore = 0;
  for (const auto& ch : chapterPageInfo.segments) {
    if (ch.spineIndex == currentSpineIndex) {
      return chapterPagesBefore + (spineItem->currentPage - ch.startPage);
    }
    chapterPagesBefore += ch.endPage - ch.startPage;
  }
  return spineItem ? spineItem->currentPage : 0;
}

uint16_t EpubReaderActivity::getChapterTotalPages() const {
  if (chapterPageInfo.segments.empty()) {
    return spineItem ? spineItem->getPageCount() : 0;
  }
  return std::accumulate(chapterPageInfo.segments.begin(), chapterPageInfo.segments.end(), uint16_t{0},
                         [](uint16_t sum, const Chapter& ch) -> uint16_t { return sum + ch.endPage - ch.startPage; });
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && spineItem && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, spineItem->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %u, page %u", footnoteDepth, currentSpineIndex, spineItem->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  std::optional<uint16_t> targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (!targetSpineIndex) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = *targetSpineIndex;
    nextPageNumber = 0;
    spineItem.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %u for href: %s", *targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %u, page %u", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    spineItem.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::addBookmark() {
  if (!spineItem || !epub) return;

  uint16_t paragraphIdx;
  uint16_t liIdx;
  {
    RenderLock lock(*this);
    const uint16_t currentPage = spineItem->currentPage;
    if (currentPage >= spineItem->getPageCount()) return;
    paragraphIdx = spineItem->getParagraphIndexForPage(currentPage).value_or(0);
    liIdx = spineItem->getListItemIndexForPage(currentPage).value_or(BookmarkEntry::NO_LI_INDEX);
  }

  BookmarkEntry entry;
  entry.spineIndex = currentSpineIndex;
  entry.paragraphIndex = paragraphIdx;
  entry.liIndex = liIdx;
  // Summary build reads the section cache file; safe outside the render lock
  // because we already captured the spine page index above and forEachWord
  // operates against that page only.
  entry.summary = buildBookmarkSummary(*spineItem);

  BookmarkStore store(epub->getPath());
  if (store.insert(entry)) {
    LOG_DBG("ERS", "Bookmark added (spine=%u para=%u li=%u)", entry.spineIndex, entry.paragraphIndex, entry.liIndex);
    showBookmarkMessage = true;
  } else {
    LOG_ERR("ERS", "Failed to save bookmark");
  }

  requestUpdate();
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (spineItem) {
    info.currentPage = spineItem->currentPage + 1;
    info.totalPages = spineItem->getPageCount();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(spineItem->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}
