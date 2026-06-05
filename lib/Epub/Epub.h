#pragma once

#include <Print.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/Chapter.h"
#include "Epub/css/CssParser.h"

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  void parseCssFiles() const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(uint16_t spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(uint16_t tocIndex) const;
  uint16_t getSpineItemsCount() const;
  uint16_t getTocItemsCount() const;
  // Returns the spine index for the given TOC entry, or nullopt if tocIndex is out of range
  // or the entry has no spine mapping.
  std::optional<uint16_t> getSpineIndexForTocIndex(uint16_t tocIndex) const;
  // Returns the TOC index for the given spine, or nullopt if the spine has no TOC entry
  // (pre-TOC orphans like the cover; post-TOC orphans inherit the previous spine's tocIndex
  // and return it here).
  std::optional<uint16_t> getTocIndexForSpineIndex(uint16_t spineIndex) const;
  // Contiguous spine range [first, last] (last inclusive) that belongs to the given TOC chapter.
  // Uses the next TOC entry's anchor to decide whether this chapter shares the next chapter's
  // first spine. For the last TOC entry, caps to its own spine to exclude post-TOC orphan
  // spines (appendices, copyright pages) from being lumped into the last chapter.
  // Returns nullopt if tocIndex is out of range or the TOC entry's spine is invalid.
  std::optional<SpineRange> getSpineRangeForTocIndex(uint16_t tocIndex) const;
  size_t getCumulativeSpineItemSize(uint16_t spineIndex) const;
  uint16_t getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(uint16_t currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  std::optional<uint16_t> resolveHrefToSpineIndex(const std::string& href) const;
};
