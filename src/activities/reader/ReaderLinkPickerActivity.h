#pragma once

#include <Typesetter/LinkEntry.h>

#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Format-agnostic list-picker for on-page navigation targets. Renders a
// scrollable list of LinkEntry labels and returns the selected entry's href
// via LinkResult so the parent activity can navigate. Currently used by
// EpubReaderActivity for footnotes; will be used by MdReaderActivity for
// inline links once Markdown Layer 3 lands. The title and empty-state
// messages are caller-supplied so the same activity can display
// "Footnotes" / "No footnotes" or "Links" / "No links" without rebuilding.
//
// `title` and `emptyMessage` are pointers to translated strings owned by
// the I18n system; their storage is static so the activity holds them by
// raw pointer across its lifetime.
class ReaderLinkPickerActivity final : public Activity {
 public:
  explicit ReaderLinkPickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                    const std::vector<LinkEntry>& entries, const char* title, const char* emptyMessage)
      : Activity("ReaderLinkPicker", renderer, mappedInput),
        entries(entries),
        title(title),
        emptyMessage(emptyMessage) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const std::vector<LinkEntry>& entries;
  const char* title;
  const char* emptyMessage;
  int selectedIndex = 0;
  int scrollOffset = 0;
  ButtonNavigator buttonNavigator;
};
