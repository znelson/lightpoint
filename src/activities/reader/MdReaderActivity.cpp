#include "MdReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Markdown/MarkdownParser.h>
#include <Typesetter.h>
#include <Typesetter/Page.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "MdReaderMenuActivity.h"
#include "ReaderLinkPickerActivity.h"
#include "ReaderPercentSelectionActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

void MdReaderActivity::onEnter() {
  Activity::onEnter();

  if (!md) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  md->setupCacheDir();

  auto filePath = md->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  requestUpdate();
}

void MdReaderActivity::onExit() {
  Activity::onExit();

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  md.reset();
}

void MdReaderActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(md ? md->getPath() : "");
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  // Confirm opens the reader menu. The menu dispatches to LinkPicker
  // (when the current page has interactive link targets) or the percent
  // selector. Direct activity bindings would work for a single-action
  // reader, but with two actions the menu makes the activity stack
  // explicit and leaves room for future items (rotation, etc.).
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int progressPercent =
        cache.pageCount > 0 ? static_cast<int>((currentPage + 1) * 100.0f / cache.pageCount + 0.5f) : 0;
    startActivityForResult(
        std::make_unique<MdReaderMenuActivity>(renderer, mappedInput, md->getTitle(), currentPage + 1, cache.pageCount,
                                               progressPercent, !currentPageLinks.empty()),
        [this](const ActivityResult& menuResult) { onMenuResult(menuResult); });
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < static_cast<int>(cache.pageCount) - 1) {
      currentPage++;
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

bool MdReaderActivity::buildSectionCache(uint16_t viewportWidth, uint16_t viewportHeight) {
  const int fontId = cachedFontId;
  const float lineCompression = SETTINGS.getReaderLineCompression();
  const bool extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  const uint8_t paragraphAlignment = SETTINGS.paragraphAlignment;
  const bool hyphenationEnabled = SETTINGS.hyphenationEnabled;
  const bool focusReadingEnabled = SETTINGS.focusReadingEnabled;

  if (!cache.openForWrite()) {
    return false;
  }
  cache.writeHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight,
                    hyphenationEnabled, false, 0, focusReadingEnabled);

  std::vector<PageLutEntry> lut;

  // Named lambda: Typesetter holds a FunctionRef to it, so it must outlive
  // the Typesetter. Hoisting prevents the temporary-in-ctor-args hazard.
  auto onPageComplete = [this, &lut](std::unique_ptr<Page> page, uint16_t paragraphIndex, uint16_t listItemIndex) {
    lut.push_back({cache.writePage(std::move(page)), paragraphIndex, listItemIndex});
  };
  Typesetter typesetter(renderer, fontId, lineCompression, extraParagraphSpacing, viewportWidth, viewportHeight,
                        onPageComplete);

  // Same lifetime story: MarkdownParser stores a FunctionRef to this read
  // adapter, which is local to this function -- safe for the parse() call.
  auto readFn = [this](uint8_t* buffer, size_t offset, size_t length) -> bool {
    return md->readContent(buffer, offset, length);
  };

  MarkdownParser parser(typesetter, readFn, md->getFileSize(), extraParagraphSpacing, hyphenationEnabled,
                        focusReadingEnabled, paragraphAlignment);
  if (!parser.parse()) {
    LOG_ERR("MDR", "MarkdownParser failed");
    cache.closeAndRemove();
    return false;
  }
  typesetter.finish();

  // Heading anchors collected by the parser become the on-disk anchor map
  // so getPageForAnchor("#slug") works for TOC navigation. Layer 2 will
  // also let [text](#slug) links resolve through the same map.
  if (!cache.finalize(lut, parser.getAnchors())) {
    LOG_ERR("MDR", "Failed to finalize section cache");
    return false;
  }

  LOG_DBG("MDR", "Built section cache: %d pages", cache.pageCount);
  return true;
}

void MdReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  cachedFontId = SETTINGS.getReaderFontId();
  const uint8_t screenMargin = SETTINGS.screenMargin;

  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += screenMargin;
  cachedOrientedMarginLeft += screenMargin;
  cachedOrientedMarginRight += screenMargin;
  cachedOrientedMarginBottom +=
      std::max(screenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  const uint16_t viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;

  if (!cache.loadHeader(cachedFontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                        SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                        /*embeddedStyle=*/false, /*imageRendering=*/0, SETTINGS.focusReadingEnabled)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));

    if (!buildSectionCache(viewportWidth, viewportHeight)) {
      LOG_ERR("MDR", "Failed to build section cache");
      initialized = true;
      return;
    }
  }

  loadProgress();
  initialized = true;
}

void MdReaderActivity::render(RenderLock&&) {
  if (!md) {
    return;
  }

  if (!initialized) {
    initializeReader();
  }

  if (cache.pageCount == 0) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (currentPage < 0) currentPage = 0;
  if (currentPage >= static_cast<int>(cache.pageCount)) currentPage = cache.pageCount - 1;

  auto page = cache.loadPage(currentPage);
  if (!page) {
    LOG_ERR("MDR", "Failed to load page %d", currentPage);
    return;
  }

  // Snapshot this page's interactive link targets for the Confirm-button
  // picker; the unique_ptr<Page> goes away after renderContents() consumes it.
  currentPageLinks = std::move(page->links);

  renderer.clearScreen();
  renderContents(std::move(page));

  saveProgress();
}

void MdReaderActivity::renderContents(std::unique_ptr<Page> page) {
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, cachedFontId, cachedOrientedMarginLeft, cachedOrientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();

  page->render(renderer, cachedFontId, cachedOrientedMarginLeft, cachedOrientedMarginTop);
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(
        renderer, [&]() { page->render(renderer, cachedFontId, cachedOrientedMarginLeft, cachedOrientedMarginTop); });
  }
}

void MdReaderActivity::renderStatusBar() const {
  const float progress = cache.pageCount > 0 ? (currentPage + 1) * 100.0f / cache.pageCount : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = md->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, cache.pageCount, title);
}

void MdReaderActivity::saveProgress() const {
  HalFile f;
  if (halStorage.openFileForWrite("MDR", md->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = 0;
    data[3] = 0;
    f.write(data, 4);
  }
}

void MdReaderActivity::loadProgress() {
  HalFile f;
  if (halStorage.openFileForRead("MDR", md->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] + (data[1] << 8);
      if (currentPage >= static_cast<int>(cache.pageCount)) {
        currentPage = cache.pageCount - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      LOG_DBG("MDR", "Loaded progress: page %d/%d", currentPage, cache.pageCount);
    }
  }
}

void MdReaderActivity::onMenuResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }
  const auto& menuResult = std::get<MenuResult>(result.data);
  if (!menuResult.action) {
    requestUpdate();
    return;
  }
  const auto action = static_cast<MdReaderMenuActivity::MenuAction>(*menuResult.action);
  switch (action) {
    case MdReaderMenuActivity::MenuAction::LINKS:
      startActivityForResult(
          std::make_unique<ReaderLinkPickerActivity>(renderer, mappedInput, currentPageLinks, tr(STR_LINKS)),
          [this](const ActivityResult& linkPickerResult) {
            if (!linkPickerResult.isCancelled) {
              const auto& linkResult = std::get<LinkResult>(linkPickerResult.data);
              if (!linkResult.href.empty() && linkResult.href[0] == '#') {
                const auto resolved = cache.getPageForAnchor(linkResult.href.substr(1));
                if (resolved) {
                  currentPage = *resolved;
                }
              }
            }
            requestUpdate();
          });
      return;
    case MdReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const int initialPercent =
          cache.pageCount > 0 ? static_cast<int>((currentPage + 1) * 100.0f / cache.pageCount + 0.5f) : 0;
      startActivityForResult(std::make_unique<ReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
                             [this](const ActivityResult& percentResult) {
                               if (!percentResult.isCancelled) {
                                 jumpToPercent(std::get<PercentResult>(percentResult.data).percent);
                               }
                               requestUpdate();
                             });
      return;
    }
  }
}

void MdReaderActivity::jumpToPercent(int percent) {
  // Single-file format: percent maps directly to a page within the section
  // cache. Round to the nearest page index, clamped to [0, pageCount-1].
  if (cache.pageCount == 0) return;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  const int targetPage = static_cast<int>(percent * (cache.pageCount - 1) / 100.0f + 0.5f);
  currentPage = std::clamp(targetPage, 0, static_cast<int>(cache.pageCount) - 1);
}

ScreenshotInfo MdReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (md) {
    const std::string t = md->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = cache.pageCount;
  info.progressPercent =
      cache.pageCount > 0 ? static_cast<int>((currentPage + 1) * 100.0f / cache.pageCount + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
