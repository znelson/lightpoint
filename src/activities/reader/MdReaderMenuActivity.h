#pragma once

#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Reader menu shown when the user presses Confirm in MdReaderActivity.
// Lists the actions available on the current page. Sibling of
// EpubReaderMenuActivity but with a smaller surface -- Markdown reading
// doesn't have chapter navigation (anchor-based jumping is reached via
// the LINKS picker), screen rotation control, or per-book delete-cache
// affordances. Returns MenuResult{action} via setResult; caller maps the
// action enum back to the right sub-activity.
//
// LINKS is omitted from the menu when the current page has no interactive
// link targets -- same gating pattern EpubReaderMenuActivity uses for the
// FOOTNOTES item. With links absent, the menu only has GO_TO_PERCENT and
// the picker visit cost is unchanged.
class MdReaderMenuActivity final : public Activity {
 public:
  enum class MenuAction { LINKS, GO_TO_PERCENT };

  explicit MdReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                const int currentPage, const int totalPages, const int progressPercent,
                                const bool hasLinks);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems(bool hasLinks);

  const std::vector<MenuItem> menuItems;

  uint16_t selectedIndex = 0;
  ButtonNavigator buttonNavigator;
  std::string title;
  int currentPage = 0;
  int totalPages = 0;
  int progressPercent = 0;
};
