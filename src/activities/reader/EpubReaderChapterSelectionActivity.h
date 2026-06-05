#pragma once
#include <Epub.h>

#include <memory>
#include <optional>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  uint16_t currentSpineIndex = 0;
  std::optional<uint16_t> currentTocIndex;
  uint16_t selectorIndex = 0;

  // Number of items that fit on a page, derived from logical screen height.
  // This adapts automatically when switching between portrait and landscape.
  int getPageItems() const;

  // Total TOC items count
  uint16_t getTotalItems() const;

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                              const uint16_t currentSpineIndex,
                                              const std::optional<uint16_t> currentTocIndex)
      : Activity("EpubReaderChapterSelection", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentTocIndex(currentTocIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
