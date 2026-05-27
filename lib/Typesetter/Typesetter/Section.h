#pragma once

#include <FunctionRef.h>
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Page;

// Page-LUT entry: file offset of a serialized page plus the paragraph and
// list-item counters at the start of that page. Used by SpineItem to wire
// the page-completion callback through Typesetter and accumulated into the
// LUT that `finalize` writes at the end of the section file.
struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
};

// Section: format-agnostic binary cache for paginated content.
//
// Owns the on-disk `.bin` file that persists laid-out pages, the page LUT,
// the anchor map, and the paragraph / list-item LUTs. Knows nothing about
// EPUB / HTML / CSS -- any parser+layout pipeline can use it to persist
// and retrieve Page objects keyed by index, anchor string, paragraph
// index, or list-item index.
//
// Binary layout (all little-endian, see header offsets below):
//   [Header]       version + render params + placeholders for offsets
//   [Pages]        serialized Page objects, back-to-back
//   [Page LUT]     uint32_t file offsets for each page
//   [Anchor Map]   uint16_t count + (string key, uint16_t page) entries
//   [Paragraph LUT] uint16_t count + uint16_t paragraphIndex per page
//   [List-item LUT] uint16_t listItemIndex per page (count shared above)
//
// Write lifecycle:
//   openForWrite -> writeHeader -> writePage* -> finalize
//
// Read lifecycle:
//   loadHeader (sets pageCount) -> loadPage / get... queries
//
// File handle is opened lazily per call for query methods so multiple
// queries don't keep the SD file open.
class Section {
 public:
  // Bump when binary layout changes. Mismatching versions cause loadHeader
  // to fail and the cache is regenerated from source.
  static constexpr uint8_t FILE_VERSION = 25;

  // Number of pages persisted to the file. Public field so SpineItem can
  // expose `pageCount` as a reference to this without an extra accessor.
  // Written by loadHeader (reads back from file) and incremented by
  // writePage during build.
  uint16_t pageCount = 0;

  explicit Section(std::string filePath) : filePath_(std::move(filePath)) {}
  ~Section() = default;

  const std::string& getFilePath() const { return filePath_; }

  // --- Write API --------------------------------------------------------
  // Begin a fresh write. Opens (or truncates) the file. Subsequent
  // writeHeader / writePage calls operate on this handle. finalize closes.
  bool openForWrite();

  // Write the header (version + render params + offset placeholders patched
  // by finalize). Must be called after openForWrite, before any writePage.
  void writeHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                   uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                   uint8_t imageRendering, bool focusReadingEnabled);

  // Serialize one page to the file. Returns the file offset where the page
  // begins (or 0 on failure). Increments pageCount.
  uint32_t writePage(std::unique_ptr<Page> page);

  // Finalize: writes page LUT, anchor map, paragraph LUT, and list-item LUT
  // after the last page, then patches the header offsets and closes the
  // file. Returns false on I/O failure (file is removed in that case).
  bool finalize(const std::vector<PageLutEntry>& lut, const std::vector<std::pair<std::string, uint16_t>>& anchors);

  // --- Read API ---------------------------------------------------------
  // Open the file and validate the header against `params`. On success,
  // pageCount is populated and the file is closed. Returns false if the
  // file is missing, the version doesn't match, or the params differ; on
  // version/param mismatch the cache file is also removed.
  bool loadHeader(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                  uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                  uint8_t imageRendering, bool focusReadingEnabled);

  // Read a serialized page from disk by index. Opens/closes the file
  // internally. Returns nullptr if the index is out of range, the LUT is
  // missing, or deserialization fails.
  std::unique_ptr<Page> loadPage(int pageIndex);

  // Iterate the on-disk anchor map. Length-aware so callers can skip
  // allocating the key string for entries that can't match anything they
  // care about.
  //   predicate(keyLen) -> false : skip this entry without reading the key
  //   predicate(keyLen) -> true  : read the key, then call consumer
  //   consumer(key, page) -> false : stop iteration early
  //   consumer(key, page) -> true  : continue
  // Returns true if iteration completed (or stopped via consumer).
  // Returns false if the anchor map is missing or corrupt.
  bool forEachAnchor(FunctionRef<bool(uint32_t keyLen)> predicate,
                     FunctionRef<bool(const std::string& key, uint16_t page)> consumer) const;

  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  // --- File management --------------------------------------------------
  // Remove the cache file from disk. Returns true if the file was removed
  // or didn't exist; false on filesystem error. Assumes no write handle
  // is currently open on the file; see closeAndRemove for the mid-write
  // abandon case.
  bool clearCache() const;

  // Abandon an in-progress write: close the internal file handle (if open)
  // and remove the cache file. Used when a build (parse/layout) fails
  // partway through openForWrite -> writeHeader -> writePage* and the
  // partial cache must not be left readable on disk. Returns false if the
  // remove call failed.
  bool closeAndRemove();

 private:
  std::string filePath_;
  HalFile file_;
};
