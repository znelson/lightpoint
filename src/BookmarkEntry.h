#pragma once
#include <cstdint>
#include <string>

// A single bookmark entry: a content-anchored position in a book.
//
// spineIndex + paragraphIndex are the stable identity -- both are content
// coordinates that survive reflow (font, size, viewport, orientation
// changes), unlike page numbers. liIndex narrows resolution within
// list-heavy content where one paragraph slot spans many on-screen
// positions; NO_LI_INDEX means "this bookmark is not inside a list."
//
// summary is the first ~70 characters of the bookmarked page, sanitized
// down to printable text. Used purely for human identification in the
// bookmark list -- never reparsed.
struct BookmarkEntry {
  static constexpr uint16_t NO_LI_INDEX = 0xFFFF;

  uint16_t spineIndex = 0;
  uint16_t paragraphIndex = 0;
  uint16_t liIndex = NO_LI_INDEX;
  std::string summary;
};
