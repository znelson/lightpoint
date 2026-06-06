#include "ReaderLinkPickerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ReaderLinkPickerActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void ReaderLinkPickerActivity::onExit() { Activity::onExit(); }

void ReaderLinkPickerActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex < entries.size()) {
      setResult(LinkResult{entries[selectedIndex].href});
      finish();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    if (!entries.empty()) {
      selectedIndex = (selectedIndex + 1) % entries.size();
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (!entries.empty()) {
      selectedIndex = (selectedIndex - 1 + entries.size()) % entries.size();
      requestUpdate();
    }
  });
}

void ReaderLinkPickerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: reserve a horizontal gutter for button hints.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: reserve vertical space for hints at the top.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  // Landscape CW places hints on the left edge; CCW keeps them on the right.
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  // Manual centering to honor content gutters.
  const int titleX = contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, title, true, EpdFontFamily::BOLD);

  // Empty-entries case: callers contractually gate invocation. If we end
  // up here anyway, render the title + Back hint and let Back unwind.
  if (entries.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  constexpr int lineHeight = 36;
  const int screenWidth = renderer.getScreenWidth();
  const int marginLeft = contentX + 20;

  const uint16_t visibleCount = std::max(1, (renderer.getScreenHeight() - contentY) / lineHeight);
  if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
  if (selectedIndex >= scrollOffset + visibleCount) scrollOffset = selectedIndex - visibleCount + 1;

  for (size_t i = scrollOffset; i < entries.size() && i < scrollOffset + visibleCount; i++) {
    const int y = 60 + contentY + (i - scrollOffset) * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(0, y, screenWidth, lineHeight, true);
    }

    // Show the entry's label; fall back to a generic placeholder when the
    // label is empty (parser couldn't extract a visible text run, e.g.
    // an EPUB anchor that wraps non-text content).
    std::string label = entries[i].label;
    if (label.empty()) {
      label = tr(STR_LINK);
    }
    renderer.drawText(UI_10_FONT_ID, marginLeft, y + 4, label.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
