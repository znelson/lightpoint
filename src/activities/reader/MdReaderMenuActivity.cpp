#include "MdReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

MdReaderMenuActivity::MdReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& title, const int currentPage, const int totalPages,
                                           const int progressPercent, const bool hasLinks)
    : Activity("MdReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasLinks)),
      title(title),
      currentPage(currentPage),
      totalPages(totalPages),
      progressPercent(progressPercent) {}

std::vector<MdReaderMenuActivity::MenuItem> MdReaderMenuActivity::buildMenuItems(bool hasLinks) {
  std::vector<MenuItem> items;
  items.reserve(2);
  if (hasLinks) {
    items.push_back({MenuAction::LINKS, StrId::STR_LINKS});
  }
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  return items;
}

void MdReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void MdReaderMenuActivity::onExit() { Activity::onExit(); }

void MdReaderMenuActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, menuItems.size());
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, menuItems.size());
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    // pendingOrientation field unused for Md (no rotation control here); pass 0.
    setResult(MenuResult{static_cast<int>(selectedAction), 0});
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{std::nullopt, 0};
    setResult(std::move(result));
    finish();
    return;
  }
}

void MdReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

  // Progress summary -- just a single line since Md has no chapter concept.
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::to_string(currentPage) + "/" + std::to_string(totalPages) +
                   std::string(tr(STR_PAGES_SEPARATOR)) + std::to_string(progressPercent) + "%";
  }
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());

  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, menuItems.size(), selectedIndex,
      [this](int index) { return I18N.get(menuItems[index].labelId); }, nullptr, nullptr, [](int) { return ""; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
