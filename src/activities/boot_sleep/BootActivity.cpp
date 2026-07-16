#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "images/Logo256.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  constexpr int logoSize = 256;
  const int logoX = (pageWidth - logoSize) / 2;
  const int logoY = (pageHeight - logoSize) / 2;
  const int logoBottom = logoY + logoSize;

  // Boot stays a single 1-bit pass: the grayscale refresh would add two 48KB
  // plane copies plus a custom-LUT gray refresh right when the user is
  // waiting for the device to come up. Threshold 2 renders light gray as
  // white, the best standalone 1-bit rendition of the artwork; the sleep
  // screen does the full 4-level render instead (see docs/boot-logo.md).
  renderer.clearScreen();
  renderer.drawImage2Bit(Logo256, logoX, logoY, logoSize, logoSize, 2);
  renderer.drawCenteredText(UI_10_FONT_ID, logoBottom + 10, tr(STR_LIGHTPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, logoBottom + 35, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, LIGHTPOINT_VERSION);
  renderer.displayBuffer();
}
