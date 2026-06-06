#pragma once
#include <Xtc.h>

#include <memory>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class XtcReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;
  ButtonNavigator buttonNavigator;
  uint32_t currentPage = 0;
  uint16_t selectorIndex = 0;

  uint16_t getPageItems() const;
  uint16_t findChapterIndexForPage(uint32_t page) const;

 public:
  explicit XtcReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Xtc>& xtc, uint32_t currentPage)
      : Activity("XtcReaderChapterSelection", renderer, mappedInput), xtc(xtc), currentPage(currentPage) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
