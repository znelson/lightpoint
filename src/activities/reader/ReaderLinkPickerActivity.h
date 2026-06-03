#pragma once

#include <Typesetter/LinkEntry.h>

#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Format-agnostic list-picker for on-page navigation targets. Renders a
// scrollable list of LinkEntry labels and returns the selected entry's
// href via LinkResult so the parent activity can navigate. Used by EPUB
// (footnotes) and Markdown (inline links). The `title` is caller-supplied
// so the activity can display "Footnotes" or "Links" without rebuilding.
//
// Caller contract: invoke ONLY when entries is non-empty. The EPUB menu
// disables its footnote item when there are none; MdReaderActivity gates
// the Confirm-button trigger the same way. The picker therefore omits an
// empty-state branch.
//
// `title` is a pointer to a translated string owned by the I18n system;
// its storage is static so the activity holds it by raw pointer across
// its lifetime.
class ReaderLinkPickerActivity final : public Activity {
 public:
  explicit ReaderLinkPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                    const std::vector<LinkEntry>& entries, const char* title)
      : Activity("ReaderLinkPicker", renderer, mappedInput), entries(entries), title(title) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const std::vector<LinkEntry>& entries;
  const char* title;
  int selectedIndex = 0;
  int scrollOffset = 0;
  ButtonNavigator buttonNavigator;
};
