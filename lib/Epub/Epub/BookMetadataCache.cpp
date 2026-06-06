#include "BookMetadataCache.h"

#include <Logging.h>
#include <Serialization.h>
#include <ZipFile.h>

#include <deque>

#include "FsHelpers.h"

namespace {
constexpr uint8_t BOOK_CACHE_VERSION = 6;
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";

namespace header {
constexpr uint32_t kVersion = 0;
constexpr uint32_t kLutOffset = kVersion + sizeof(uint8_t);
constexpr uint32_t kSpineCount = kLutOffset + sizeof(uint32_t);
constexpr uint32_t kTocCount = kSpineCount + sizeof(uint16_t);
constexpr uint32_t kSize = kTocCount + sizeof(uint16_t);
}  // namespace header
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  LOG_DBG("BMC", "Entering write mode");
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  LOG_DBG("BMC", "Beginning content opf pass");

  // Open spine file for writing
  return halStorage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile);
}

bool BookMetadataCache::endContentOpfPass() {
  // Explicit close() required: member variable persists beyond function scope
  spineFile.close();
  return true;
}

bool BookMetadataCache::beginTocPass() {
  LOG_DBG("BMC", "Beginning toc pass");

  if (!halStorage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!halStorage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variable persists beyond function scope
    spineFile.close();
    return false;
  }

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    spineHrefIndex.clear();
    spineHrefIndex.resize(spineCount);
    spineFile.seek(0);
    for (uint16_t i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      SpineHrefIndexEntry idx;
      idx.hrefHash = Fnv1a::hash(entry.href);
      idx.hrefLen = entry.href.size();
      idx.spineIndex = i;
      spineHrefIndex[i] = idx;
    }
    std::sort(spineHrefIndex.begin(), spineHrefIndex.end(),
              [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
              });
    spineFile.seek(0);
    useSpineHrefIndex = true;
    LOG_DBG("BMC", "Using fast index for %u spine items", spineCount);
  } else {
    useSpineHrefIndex = false;
  }

  return true;
}

bool BookMetadataCache::endTocPass() {
  // Explicit close() required: member variables persist beyond function scope
  tocFile.close();
  spineFile.close();

  spineHrefIndex.clear();
  spineHrefIndex.shrink_to_fit();
  useSpineHrefIndex = false;

  return true;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  LOG_DBG("BMC", "Wrote %u spine, %u TOC entries", spineCount, tocCount);
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  // Open all three files, writing to meta, reading from spine and toc
  if (!halStorage.openFileForWrite("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  if (!halStorage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  if (!halStorage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variables persist beyond function scope
    bookFile.close();
    spineFile.close();
    return false;
  }

  static_assert(header::kSize == sizeof(BOOK_CACHE_VERSION) + sizeof(lutOffset) + sizeof(spineCount) + sizeof(tocCount),
                "BookMetadataCache header size mismatch");

  const uint32_t lutSize = sizeof(uint32_t) * (spineCount + tocCount);

  // Header
  serialization::writePod(bookFile, BOOK_CACHE_VERSION);
  serialization::writePod(bookFile, uint32_t{0});  // lutOffset placeholder, patched later
  serialization::writePod(bookFile, spineCount);
  serialization::writePod(bookFile, tocCount);
  // Metadata
  serialization::writeString(bookFile, metadata.title);
  serialization::writeString(bookFile, metadata.author);
  serialization::writeString(bookFile, metadata.language);
  serialization::writeString(bookFile, metadata.coverItemHref);
  serialization::writeString(bookFile, metadata.textReferenceHref);

  // Patch lutOffset with actual position
  const uint32_t lutOffset = bookFile.position();
  bookFile.seek(header::kLutOffset);
  serialization::writePod(bookFile, lutOffset);
  bookFile.seek(lutOffset);

  // Loop through spine entries, writing LUT positions
  spineFile.seek(0);
  for (uint16_t i = 0; i < spineCount; i++) {
    uint32_t pos = spineFile.position();
    skipSpineEntry(spineFile);
    serialization::writePod(bookFile, pos + lutOffset + lutSize);
  }

  const uint32_t spineDataSize = spineFile.position();

  // Loop through toc entries, writing LUT positions
  tocFile.seek(0);
  for (uint16_t i = 0; i < tocCount; i++) {
    uint32_t pos = tocFile.position();
    skipTocEntry(tocFile);
    serialization::writePod(bookFile, pos + lutOffset + lutSize + spineDataSize);
  }

  // LUTs complete
  // Loop through spines from spine file matching up TOC indexes, calculating cumulative size and writing to book.bin

  // Build spineIndex->tocIndex mapping in one pass (O(n) instead of O(n*m))
  std::deque<std::optional<uint16_t>> spineToTocIndex(spineCount);
  tocFile.seek(0);
  for (uint16_t j = 0; j < tocCount; j++) {
    auto tocEntry = readTocEntry(tocFile);
    if (tocEntry.spineIndex && *tocEntry.spineIndex < spineCount) {
      if (!spineToTocIndex[*tocEntry.spineIndex]) {
        spineToTocIndex[*tocEntry.spineIndex] = j;
      }
    }
  }

  ZipFile zip(epubPath);
  // Pre-open zip file to speed up size calculations
  if (!zip.open()) {
    LOG_ERR("BMC", "Could not open EPUB zip for size calculations");
    // Explicit close() required: member variables persist beyond function scope
    bookFile.close();
    spineFile.close();
    tocFile.close();
    return false;
  }
  // NOTE: We intentionally skip calling loadAllFileStatSlims() here.
  // For large EPUBs (2000+ chapters), pre-loading all ZIP central directory entries
  // into memory causes OOM crashes on ESP32-C3's limited ~380KB RAM.
  // Instead, for large books we use a one-pass batch lookup that scans the ZIP
  // central directory once and matches against spine targets using hash comparison.
  // This is O(n*log(m)) instead of O(n*m) while avoiding memory exhaustion.
  // See: https://github.com/crosspoint-reader/crosspoint-reader/issues/134

  std::deque<uint32_t> spineSizes;
  bool useBatchSizes = false;

  if (spineCount >= LARGE_SPINE_THRESHOLD) {
    LOG_DBG("BMC", "Using batch size lookup for %u spine items", spineCount);

    std::deque<ZipFile::SizeTarget> targets;
    targets.resize(spineCount);

    spineFile.seek(0);
    for (uint16_t i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      std::string path = FsHelpers::normalisePath(entry.href);

      ZipFile::SizeTarget t;
      t.hash = Fnv1a::hash(path);
      t.len = path.size();
      t.index = i;
      targets[i] = t;
    }

    std::sort(targets.begin(), targets.end(), [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
      return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
    });

    spineSizes.resize(spineCount, 0);
    [[maybe_unused]] const size_t matched = zip.fillUncompressedSizes(targets, spineSizes);
    LOG_DBG("BMC", "Batch lookup matched %zu/%u spine items", matched, spineCount);

    targets.clear();
    targets.shrink_to_fit();

    useBatchSizes = true;
  }

  uint32_t cumSize = 0;
  spineFile.seek(0);
  std::optional<uint16_t> lastSpineTocIndex;
  for (uint16_t i = 0; i < spineCount; i++) {
    auto spineEntry = readSpineEntry(spineFile);

    spineEntry.tocIndex = spineToTocIndex[i];

    // Not a huge deal if we don't fine a TOC entry for the spine entry, this is expected behaviour for EPUBs
    // Logging here is for debugging
    if (!spineEntry.tocIndex) {
      LOG_DBG("BMC", "Warning: Could not find TOC entry for spine item %u: %s, using title from last spine item", i,
              spineEntry.href.c_str());
      spineEntry.tocIndex = lastSpineTocIndex;
    }
    lastSpineTocIndex = spineEntry.tocIndex;

    size_t itemSize = 0;
    if (useBatchSizes) {
      itemSize = spineSizes[i];
      if (itemSize == 0) {
        const std::string path = FsHelpers::normalisePath(spineEntry.href);
        if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
          LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
        }
      }
    } else {
      const std::string path = FsHelpers::normalisePath(spineEntry.href);
      if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
        LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
      }
    }

    cumSize += itemSize;
    spineEntry.cumulativeSize = cumSize;

    // Write out spine data to book.bin
    writeSpineEntry(bookFile, spineEntry);
  }
  // Close opened zip file
  zip.close();

  // Loop through toc entries from toc file writing to book.bin
  tocFile.seek(0);
  for (uint16_t i = 0; i < tocCount; i++) {
    auto tocEntry = readTocEntry(tocFile);
    writeTocEntry(bookFile, tocEntry);
  }

  // Explicit close() required: member variables persist beyond function scope
  bookFile.close();
  spineFile.close();
  tocFile.close();

  LOG_DBG("BMC", "Successfully built book.bin");
  return true;
}

bool BookMetadataCache::cleanupTmpFiles() const {
  const auto spineBinFile = cachePath + tmpSpineBinFile;
  if (halStorage.exists(spineBinFile.c_str())) {
    halStorage.remove(spineBinFile.c_str());
  }
  const auto tocBinFile = cachePath + tmpTocBinFile;
  if (halStorage.exists(tocBinFile.c_str())) {
    halStorage.remove(tocBinFile.c_str());
  }
  return true;
}

uint32_t BookMetadataCache::writeSpineEntry(HalFile& file, const SpineEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.href);
  serialization::writePod(file, entry.cumulativeSize);
  // tocIndex is std::optional<uint16_t>; serialization::writePod's optional
  // overload encodes nullopt as UINT16_MAX. Bit-pattern compatible with the
  // prior int16_t-with-(-1) encoding for values [0, INT16_MAX].
  serialization::writePod(file, entry.tocIndex);
  return pos;
}

uint32_t BookMetadataCache::writeTocEntry(HalFile& file, const TocEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  // See writeSpineEntry: optional<uint16_t> with UINT16_MAX as nullopt.
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, std::nullopt);
  writeSpineEntry(spineFile, entry);
  spineCount++;
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }

  std::optional<uint16_t> spineIndex;

  if (useSpineHrefIndex) {
    size_t targetHash = Fnv1a::hash(href);
    uint16_t targetLen = href.size();

    auto it =
        std::lower_bound(spineHrefIndex.begin(), spineHrefIndex.end(), SpineHrefIndexEntry{targetHash, targetLen, 0},
                         [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                           return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
                         });

    while (it != spineHrefIndex.end() && it->hrefHash == targetHash && it->hrefLen == targetLen) {
      spineIndex = it->spineIndex;
      break;
    }

    if (!spineIndex) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  } else {
    spineFile.seek(0);
    for (uint16_t i = 0; i < spineCount; i++) {
      auto spineEntry = readSpineEntry(spineFile);
      if (spineEntry.href == href) {
        spineIndex = i;
        break;
      }
    }
    if (!spineIndex) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  }

  const TocEntry entry(title, href, anchor, level, spineIndex);
  writeTocEntry(tocFile, entry);
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::load() {
  if (!halStorage.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(bookFile, version);
  if (version != BOOK_CACHE_VERSION) {
    LOG_DBG("BMC", "Cache version mismatch: expected %d, got %d", BOOK_CACHE_VERSION, version);
    // Explicit close() required: member variable persists beyond function scope
    bookFile.close();
    return false;
  }

  serialization::readPod(bookFile, lutOffset);
  serialization::readPod(bookFile, spineCount);
  serialization::readPod(bookFile, tocCount);

  serialization::readString(bookFile, coreMetadata.title);
  serialization::readString(bookFile, coreMetadata.author);
  serialization::readString(bookFile, coreMetadata.language);
  serialization::readString(bookFile, coreMetadata.coverItemHref);
  serialization::readString(bookFile, coreMetadata.textReferenceHref);

  loaded = true;
  LOG_DBG("BMC", "Loaded cache data: %u spine, %u TOC entries", spineCount, tocCount);
  return true;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const uint16_t index) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineEntry called but cache not loaded");
    return {};
  }

  if (index >= spineCount) {
    LOG_ERR("BMC", "getSpineEntry index %u out of range", index);
    return {};
  }

  // Seek to spine LUT item, read from LUT and get out data
  uint32_t spineEntryPos;
  bookFile.seek(lutOffset + sizeof(spineEntryPos) * index);
  serialization::readPod(bookFile, spineEntryPos);
  bookFile.seek(spineEntryPos);
  return readSpineEntry(bookFile);
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const uint16_t index) {
  if (!loaded) {
    LOG_ERR("BMC", "getTocEntry called but cache not loaded");
    return {};
  }

  if (index >= tocCount) {
    LOG_ERR("BMC", "getTocEntry index %u out of range", index);
    return {};
  }

  // Seek to TOC LUT item, read from LUT and get out data
  uint32_t tocEntryPos;
  bookFile.seek(lutOffset + sizeof(tocEntryPos) * (spineCount + index));
  serialization::readPod(bookFile, tocEntryPos);
  bookFile.seek(tocEntryPos);
  return readTocEntry(bookFile);
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(HalFile& file) const {
  SpineEntry entry;
  serialization::readString(file, entry.href);
  serialization::readPod(file, entry.cumulativeSize);
  // tocIndex on disk: uint16_t with UINT16_MAX as the nullopt sentinel.
  // Bit-pattern compatible with the prior int16_t encoding (0xFFFF == -1 ==
  // UINT16_MAX) for the value range actually written by older firmware.
  serialization::readPod(file, entry.tocIndex);
  return entry;
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(HalFile& file) const {
  TocEntry entry;
  serialization::readString(file, entry.title);
  serialization::readString(file, entry.href);
  serialization::readString(file, entry.anchor);
  serialization::readPod(file, entry.level);
  // See readSpineEntry: uint16_t with UINT16_MAX as nullopt.
  serialization::readPod(file, entry.spineIndex);
  return entry;
}

void BookMetadataCache::skipSpineEntry(HalFile& file) const {
  uint32_t len;
  serialization::readPod(file, len);
  // tocIndex on disk is a plain uint16_t (see writeSpineEntry), not the
  // in-memory std::optional<uint16_t>.
  file.seek(file.position() + len + sizeof(SpineEntry::cumulativeSize) + sizeof(uint16_t));
}

void BookMetadataCache::skipTocEntry(HalFile& file) const {
  // 3: title, href, anchor
  for (int i = 0; i < 3; i++) {
    uint32_t len;
    serialization::readPod(file, len);
    file.seek(file.position() + len);
  }
  // spineIndex on disk is a plain uint16_t (see writeTocEntry).
  file.seek(file.position() + sizeof(TocEntry::level) + sizeof(uint16_t));
}
