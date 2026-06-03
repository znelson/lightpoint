#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "BookmarkEntry.h"

// Append-friendly binary store for one book's bookmarks.
//
// File layout (all little-endian):
//   [u8 version][u16 count][entry x count]
//   entry = [u16 spineIndex][u16 paragraphIndex][u16 liIndex]
//           [u8 summaryLen][u8 summary[71]]
//   ENTRY_SIZE = 78, HEADER_SIZE = 3
//
// Entries are stored in book order (sorted by spineIndex, paragraphIndex).
// Fixed-width records mean entryAt(i) is one seek + one 78 B read, so the
// list UI can window over a large bookmark file without keeping every
// entry resident in RAM. Insert and erase both rewrite the file via a
// .tmp + Storage.rename, which keeps the operation atomic without
// extending the HAL.
//
// File location: /.crosspoint/bookmarks/<std::hash(bookPath)>.bin. The
// hash matches the Epub cache-dir convention so bookmarks track the book
// across path-stable renames, and the separate /bookmarks/ root means a
// Clear Cache on the book directory leaves bookmark state intact.
class BookmarkStore {
 public:
  static constexpr uint8_t FILE_VERSION = 1;
  static constexpr size_t SUMMARY_MAX = 71;
  static constexpr size_t ENTRY_SIZE = sizeof(uint16_t) * 3 + sizeof(uint8_t) + SUMMARY_MAX;
  static constexpr size_t HEADER_SIZE = sizeof(uint8_t) + sizeof(uint16_t);

  explicit BookmarkStore(const std::string& bookPath);

  // Total bookmark count. 0 if the file is missing, has a bad version, or
  // can't be read. Cached after first call; insert / eraseAt update the
  // cache, so callers can call count() repeatedly without re-opening.
  uint16_t count();

  // Read a single entry by index. nullopt for out-of-range or I/O failure.
  std::optional<BookmarkEntry> entryAt(uint16_t index);

  // Insert into the sorted position. Always rewrites via temp file; safe
  // under crash (original file untouched until the rename succeeds).
  bool insert(const BookmarkEntry& entry);

  // Remove the entry at index, shifting later entries down by one. Rewrites
  // via temp file. Returns false on out-of-range index or I/O failure.
  bool eraseAt(uint16_t index);

 private:
  std::string path_;
  std::optional<uint16_t> cachedCount_;

  static bool sortsBefore(const BookmarkEntry& a, const BookmarkEntry& b);
};
