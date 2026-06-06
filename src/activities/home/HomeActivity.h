#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <Rect.h>

#include "./FileBrowserActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  uint16_t selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  std::unique_ptr<uint8_t[]> coverBuffer;  // Snapshot of the cover tile; non-null iff cover has been rendered
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;

  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onRecentsOpen();
  void onSettingsOpen();

  uint16_t getMenuItemCount() const;
  // Capture/restore the framebuffer region described by `rect`. The same rect must be
  // passed to both so the byte count matches the allocation.
  bool storeCoverBuffer(Rect rect);
  bool restoreCoverBuffer(Rect rect);
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
