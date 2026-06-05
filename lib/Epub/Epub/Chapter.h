#pragma once

#include <cstdint>
#include <optional>

// A TOC chapter localized to a single spine page-range. Multi-spine chapters
// are represented as multiple Chapters sharing the same tocIndex; multi-TOC-
// per-spine layouts are multiple Chapters sharing the same spineIndex.
struct Chapter {
  uint16_t tocIndex;
  uint16_t spineIndex;
  uint16_t startPage;  // inclusive
  uint16_t endPage;    // exclusive
};

// A navigation target whose page range has not yet been resolved (the spine
// item has not been loaded, so its page count is unknown).
// Used for chapter-selector results and pending cross-spine navigation.
struct ChapterTarget {
  std::optional<uint16_t> tocIndex;
  uint16_t spineIndex;
};

// Contiguous spine range belonging to a TOC chapter. Both ends inclusive.
struct SpineRange {
  uint16_t first;
  uint16_t last;
};
