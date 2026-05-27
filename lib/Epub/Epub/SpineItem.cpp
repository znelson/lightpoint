#include "SpineItem.h"

#include <HalPlatform.h>
#include <HalStorage.h>
#include <Hyphenator.h>
#include <Logging.h>
#include <Typesetter/Page.h>

#include <algorithm>

#include "Epub/css/CssParser.h"
#include "parsers/ChapterHtmlSlimParser.h"

// --- Public API: load / create / TOC queries -----------------------------

bool SpineItem::loadCacheFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering, const bool focusReadingEnabled) {
  if (!section_.loadHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                           viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled)) {
    return false;
  }
  // Build TOC boundaries by scanning anchor data from the on-disk anchor map,
  // matching only the TOC anchors we need (avoids loading all anchors into memory).
  buildTocBoundariesFromFile();
  return true;
}

bool SpineItem::createCacheFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const uint8_t imageRendering, const bool focusReadingEnabled,
                                FunctionRef<void()> popupFn) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    halStorage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      halPlatform.delay(50);  // Brief delay before retry
    }

    // Remove any incomplete file from previous attempt before retrying
    if (halStorage.exists(tmpHtmlPath.c_str())) {
      halStorage.remove(tmpHtmlPath.c_str());
    }

    HalFile tmpHtml;
    if (!halStorage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    // Explicitly close() file before calling halStorage.remove()
    tmpHtml.close();

    // If streaming failed, remove the incomplete file immediately
    if (!success && halStorage.exists(tmpHtmlPath.c_str())) {
      halStorage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  if (!section_.openForWrite()) {
    return false;
  }
  section_.writeHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                       viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled);
  std::vector<PageLutEntry> lut = {};

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      if (!cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries.
  // Also track totalEntries and unresolvedCount here so buildTocBoundaries can skip re-deriving them.
  // unresolvedCount must be captured before tocAnchors is moved into the parser constructor.
  std::vector<std::string> tocAnchors;
  uint16_t tocTotalEntries = 0;
  const auto startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex) {
    for (int i = *startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      tocTotalEntries++;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }
  const uint16_t tocUnresolvedCount = static_cast<uint16_t>(tocAnchors.size());

  // Named local so the lambda outlives `visitor` -- ChapterHtmlSlimParser
  // stores a FunctionRef pointing at this closure, so it must live in this
  // frame, not as a temporary in the constructor call.
  auto onPageComplete = [this, &lut](std::unique_ptr<Page> page, const uint16_t paragraphIndex,
                                     const uint16_t listItemIndex) {
    lut.push_back({section_.writePage(std::move(page)), paragraphIndex, listItemIndex});
  };
  ChapterHtmlSlimParser visitor(epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing,
                                paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled,
                                focusReadingEnabled, onPageComplete, embeddedStyle, contentBase, imageBasePath,
                                imageRendering, std::move(tocAnchors), popupFn, cssParser);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  halStorage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    section_.clearCache();
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const auto& anchors = visitor.getAnchors();
  if (!section_.finalize(lut, anchors)) {
    return false;
  }

  if (cssParser) {
    cssParser->clear();
  }

  buildTocBoundaries(anchors, startTocIndex, tocTotalEntries, tocUnresolvedCount);
  return true;
}

std::unique_ptr<Page> SpineItem::loadPageFromSectionFile() { return section_.loadPage(currentPage); }

// --- TOC boundary building ----------------------------------------------

// Resolve TOC anchor-to-page mappings from the parser's in-memory anchor vector.
// Called after createCacheFile when anchors are already in memory.
// See buildTocBoundariesFromFile for the on-disk variant; the two are kept separate
// because the anchor resolution has fundamentally different iteration patterns
// (scan in-memory vector vs. stream from file with early exit).
void SpineItem::buildTocBoundaries(const std::vector<std::pair<std::string, uint16_t>>& anchors,
                                   const std::optional<int> startTocIndex, const uint16_t totalEntries,
                                   const uint16_t unresolvedCount) {
  // If no TOC entries have anchors, all chapters start at page 0 and
  // getTocIndexForPage falls back to epub->getTocIndexForSpineIndex,
  // so there's nothing to resolve and no value in storing boundaries.
  if (!startTocIndex || totalEntries == 0 || unresolvedCount == 0) return;

  tocBoundaries.reserve(totalEntries);
  for (int i = *startTocIndex; i < *startTocIndex + totalEntries; i++) {
    const auto entry = epub->getTocItem(i);
    uint16_t page = 0;
    if (!entry.anchor.empty()) {
      for (const auto& [key, val] : anchors) {
        if (key == entry.anchor) {
          page = val;
          break;
        }
      }
    }
    tocBoundaries.push_back({i, spineIndex, page, 0});  // endPage filled after sort
  }

  // Defensive sort in case TOC entries are out of document order in a malformed epub.
  // Tie-break on tocIndex so entries sharing a page (e.g. unresolved anchors at page 0)
  // remain in logical document order, making lookup results deterministic.
  std::sort(tocBoundaries.begin(), tocBoundaries.end(), [](const Chapter& a, const Chapter& b) {
    return a.startPage != b.startPage ? a.startPage < b.startPage : a.tocIndex < b.tocIndex;
  });

  // endPage of each entry is the next entry's startPage; the last entry runs to pageCount.
  for (size_t i = 0; i < tocBoundaries.size(); i++) {
    tocBoundaries[i].endPage = (i + 1 < tocBoundaries.size()) ? tocBoundaries[i + 1].startPage : pageCount;
  }
}

// Resolve TOC anchor-to-page mappings by scanning the section cache's on-disk
// anchor data via Section::forEachAnchor. Caches the small set of TOC anchor
// strings first (since getTocItem does file I/O to BookMetadataCache), then
// streams through on-disk anchors matching only those, stopping as soon as
// all are found.
void SpineItem::buildTocBoundariesFromFile() {
  const auto startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (!startTocIndex) return;

  // Count TOC entries for this spine, then reserve and populate
  const int tocCount = epub->getTocItemsCount();
  uint16_t totalEntries = 0;
  uint16_t unresolvedCount = 0;
  for (int i = *startTocIndex; i < tocCount; i++) {
    const auto entry = epub->getTocItem(i);
    if (entry.spineIndex != spineIndex) break;
    totalEntries++;
    if (!entry.anchor.empty()) unresolvedCount++;
  }

  // If no TOC entries have anchors, all chapters start at page 0 and
  // getTocIndexForPage falls back to epub->getTocIndexForSpineIndex,
  // so there's nothing to resolve and no value in storing boundaries.
  if (totalEntries == 0 || unresolvedCount == 0) return;

  // Cache TOC anchor strings before scanning disk, since getTocItem() does file I/O
  struct TocAnchorEntry {
    int tocIndex;
    std::string anchor;
  };
  std::vector<TocAnchorEntry> tocAnchorsToResolve;
  tocAnchorsToResolve.reserve(unresolvedCount);
  tocBoundaries.reserve(totalEntries);
  for (int i = *startTocIndex; i < *startTocIndex + totalEntries; i++) {
    const auto entry = epub->getTocItem(i);
    tocBoundaries.push_back({i, spineIndex, 0, 0});  // startPage/endPage filled after anchor resolution + sort
    if (!entry.anchor.empty()) {
      tocAnchorsToResolve.push_back({i, std::move(entry.anchor)});
    }
  }

  // Single pass through on-disk anchors via Section::forEachAnchor. The
  // length-aware predicate skips the string allocation for entries whose
  // length can't match any unresolved TOC anchor. The consumer matches the
  // key against the small TOC list, marks the slot resolved, and stops
  // iteration when all are accounted for.
  uint16_t remainingUnresolved = unresolvedCount;
  section_.forEachAnchor(
      [&tocAnchorsToResolve](uint32_t keyLen) {
        return std::any_of(tocAnchorsToResolve.begin(), tocAnchorsToResolve.end(), [keyLen](const TocAnchorEntry& e) {
          return !e.anchor.empty() && e.anchor.size() == keyLen;
        });
      },
      [&](const std::string& key, uint16_t page) {
        for (auto& tocAnchor : tocAnchorsToResolve) {
          if (!tocAnchor.anchor.empty() && key == tocAnchor.anchor) {
            tocBoundaries[tocAnchor.tocIndex - *startTocIndex].startPage = page;
            tocAnchor.anchor.clear();  // mark resolved
            remainingUnresolved--;
            break;
          }
        }
        return remainingUnresolved > 0;  // stop iterating once all anchors are placed
      });

  // Defensive sort in case TOC entries are out of document order in a malformed epub.
  // Tie-break on tocIndex so entries sharing a page (e.g. unresolved anchors at page 0)
  // remain in logical document order, making lookup results deterministic.
  std::sort(tocBoundaries.begin(), tocBoundaries.end(), [](const Chapter& a, const Chapter& b) {
    return a.startPage != b.startPage ? a.startPage < b.startPage : a.tocIndex < b.tocIndex;
  });

  // endPage of each entry is the next entry's startPage; the last entry runs to pageCount.
  for (size_t i = 0; i < tocBoundaries.size(); i++) {
    tocBoundaries[i].endPage = (i + 1 < tocBoundaries.size()) ? tocBoundaries[i + 1].startPage : pageCount;
  }
}

// --- TOC queries (in-memory) ---------------------------------------------

std::optional<int> SpineItem::getTocIndexForPage(const int page) const {
  if (tocBoundaries.empty()) {
    return epub->getTocIndexForSpineIndex(spineIndex);
  }

  // Find the first boundary AFTER page, then step back one
  auto it = std::upper_bound(tocBoundaries.begin(), tocBoundaries.end(), static_cast<uint16_t>(page),
                             [](uint16_t page, const Chapter& boundary) { return page < boundary.startPage; });
  if (it == tocBoundaries.begin()) {
    return tocBoundaries[0].tocIndex;
  }
  return std::prev(it)->tocIndex;
}

std::optional<int> SpineItem::getPageForTocIndex(const int tocIndex) const {
  for (const auto& boundary : tocBoundaries) {
    if (boundary.tocIndex == tocIndex) {
      return boundary.startPage;
    }
  }
  return std::nullopt;
}

std::optional<Chapter> SpineItem::getPageRangeForTocIndex(const int tocIndex) const {
  for (const auto& ch : tocBoundaries) {
    if (ch.tocIndex == tocIndex) {
      return ch;
    }
  }
  return std::nullopt;
}
