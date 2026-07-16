#pragma once

#include <FunctionRef.h>
#include <Typesetter/Section.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Chapter.h"
#include "Epub.h"

class Page;
class GfxRenderer;

// SpineItem: EPUB-specific runtime object for one entry in the book's spine.
//
// Holds:
//   - a reference to the parent Epub (for spine/TOC metadata + content I/O),
//   - a Typesetter::Section that owns the on-disk cache file (.bin),
//   - per-spine TOC chapter boundaries built from anchor data,
//   - the user's current page within this spine.
//
// Distinct from `Epub`'s metadata view: a `BookMetadataCache::SpineEntry`
// is the persisted POD (href + cumulativeSize + tocIndex) and is read on
// demand from book.bin. `SpineItem` is the live runtime wrapper that
// orchestrates layout, caching, and TOC anchor resolution for one spine
// entry while the user is reading it.
class SpineItem {
  std::shared_ptr<Epub> epub;
  const uint16_t spineIndex;
  GfxRenderer& renderer;
  Section section_;

  // Chapter boundaries within this spine. Each entry has spineIndex == this
  // SpineItem's spineIndex. For 1:1 layouts (no in-spine anchors), this
  // stays empty and getTocIndexForPage falls back to
  // epub->getTocIndexForSpineIndex.
  std::vector<Chapter> tocBoundaries;

  void buildTocBoundaries(const std::vector<std::pair<std::string, uint16_t>>& anchors,
                          std::optional<uint16_t> startTocIndex, uint16_t totalEntries, uint16_t unresolvedCount);
  void buildTocBoundariesFromFile();

 public:
  uint16_t currentPage = 0;

  explicit SpineItem(const std::shared_ptr<Epub>& epub, const uint16_t spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        section_(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}
  ~SpineItem() = default;

  SpineItem(const SpineItem&) = delete;
  SpineItem& operator=(const SpineItem&) = delete;
  SpineItem(SpineItem&&) = delete;
  SpineItem& operator=(SpineItem&&) = delete;

  // High-level orchestration: validates the cache header against the given
  // render parameters, populates pageCount, and builds tocBoundaries by
  // scanning the on-disk anchor map. Returns false if the cache is missing,
  // stale, or has mismatched render parameters.
  bool loadCacheFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                     uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                     uint8_t imageRendering, bool focusReadingEnabled);

  // High-level orchestration: parses the spine's HTML via Epub, lays it out
  // through ChapterHtmlSlimParser + Typesetter, and persists the result via
  // Section. Returns false on parse / I/O failure.
  bool createCacheFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering, bool focusReadingEnabled, FunctionRef<void()> popupFn = nullptr);

  // Read-only load for TOC/page queries: populates pageCount and tocBoundaries
  // from the cache, without loadCacheFile's render-param validation (which
  // removes the cache on mismatch). False if the cache is missing or stale.
  bool loadForQuery();

  bool clearCache() const { return section_.clearCache(); }
  std::unique_ptr<Page> loadPageFromSectionFile();

  // --- TOC queries (in-memory, against tocBoundaries) -------------------
  // Given a page in this spine, return the TOC index for that page, or
  // nullopt for pre-TOC orphan spines (e.g. cover pages) where no chapter
  // applies.
  std::optional<uint16_t> getTocIndexForPage(uint16_t page) const;
  // Given a TOC index, return the start page in this spine. Returns nullopt
  // if the TOC index doesn't map to a boundary here (e.g. belongs to a
  // different spine).
  std::optional<uint16_t> getPageForTocIndex(uint16_t tocIndex) const;
  // Returns the Chapter (startPage inclusive, endPage exclusive) belonging
  // to the given TOC index within this spine.
  std::optional<Chapter> getPageRangeForTocIndex(uint16_t tocIndex) const;

  // --- Cache queries (delegated to Section) -----------------------------
  uint16_t getPageCount() const { return section_.getPageCount(); }
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const {
    return section_.getPageForAnchor(anchor);
  }
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const {
    return section_.getPageForParagraphIndex(pIndex);
  }
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const {
    return section_.getPageForListItemIndex(liIndex);
  }
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const {
    return section_.getParagraphIndexForPage(page);
  }
  std::optional<uint16_t> getListItemIndexForPage(uint16_t page) const {
    return section_.getListItemIndexForPage(page);
  }
  bool forEachWordOnCurrentPage(FunctionRef<bool(std::string_view)> visit) {
    return section_.forEachWordOnPage(currentPage, visit);
  }
};
