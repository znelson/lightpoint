#pragma once

#include <Txt.h>
#include <Typesetter/Section.h>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

class Page;

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int pagesUntilFullRefresh = 0;
  bool initialized = false;

  Section cache;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  ViewableMargins cachedMargins = {0, 0, 0, 0};

  void renderContents(std::unique_ptr<Page> page);
  void renderStatusBar() const;

  void initializeReader();
  bool buildSectionCache(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress() const;
  void loadProgress();

  // Maps a 0-100 file progress percentage to a page in `cache` and jumps to
  // it. Triggered by the Confirm-button → percent selector flow.
  void jumpToPercent(int percent);

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        cache(this->txt->getCachePath() + "/section.bin") {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
