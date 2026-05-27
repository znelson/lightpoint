#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include "Page.h"

namespace {

// Header layout: each offset is the file position of the field. Computed at
// compile time from the size of preceding fields; a static_assert in
// writeHeader checks the running total against kSize.
namespace header {
constexpr uint32_t kVersion = 0;
constexpr uint32_t kFontId = kVersion + sizeof(uint8_t);
constexpr uint32_t kLineCompression = kFontId + sizeof(int);
constexpr uint32_t kExtraParagraphSpacing = kLineCompression + sizeof(float);
constexpr uint32_t kParagraphAlign = kExtraParagraphSpacing + sizeof(bool);
constexpr uint32_t kViewportWidth = kParagraphAlign + sizeof(uint8_t);
constexpr uint32_t kViewportHeight = kViewportWidth + sizeof(uint16_t);
constexpr uint32_t kHyphenation = kViewportHeight + sizeof(uint16_t);
constexpr uint32_t kEmbeddedStyle = kHyphenation + sizeof(bool);
constexpr uint32_t kImageRendering = kEmbeddedStyle + sizeof(bool);
constexpr uint32_t kFocusReading = kImageRendering + sizeof(uint8_t);
constexpr uint32_t kPageCount = kFocusReading + sizeof(bool);
constexpr uint32_t kPageLut = kPageCount + sizeof(uint16_t);
constexpr uint32_t kAnchorMap = kPageLut + sizeof(uint32_t);
constexpr uint32_t kParagraphLut = kAnchorMap + sizeof(uint32_t);
constexpr uint32_t kListItemLut = kParagraphLut + sizeof(uint32_t);
constexpr uint32_t kSize = kListItemLut + sizeof(uint32_t);
}  // namespace header

}  // namespace

// --- Write API ------------------------------------------------------------

bool Section::openForWrite() {
  if (!halStorage.openFileForWrite("SCT", filePath_, file_)) {
    return false;
  }
  pageCount = 0;
  return true;
}

void Section::writeHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                          const uint8_t paragraphAlignment, const uint16_t viewportWidth, const uint16_t viewportHeight,
                          const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                          const bool focusReadingEnabled) {
  if (!file_) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(header::kSize == sizeof(FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                     sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) +
                                     sizeof(viewportWidth) + sizeof(viewportHeight) + sizeof(pageCount) +
                                     sizeof(hyphenationEnabled) + sizeof(embeddedStyle) + sizeof(imageRendering) +
                                     sizeof(focusReadingEnabled) + sizeof(uint32_t) + sizeof(uint32_t) +
                                     sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file_, FILE_VERSION);
  serialization::writePod(file_, fontId);
  serialization::writePod(file_, lineCompression);
  serialization::writePod(file_, extraParagraphSpacing);
  serialization::writePod(file_, paragraphAlignment);
  serialization::writePod(file_, viewportWidth);
  serialization::writePod(file_, viewportHeight);
  serialization::writePod(file_, hyphenationEnabled);
  serialization::writePod(file_, embeddedStyle);
  serialization::writePod(file_, imageRendering);
  serialization::writePod(file_, focusReadingEnabled);
  serialization::writePod(file_, pageCount);                 // Placeholder; patched by finalize.
  serialization::writePod(file_, static_cast<uint32_t>(0));  // Page LUT offset placeholder.
  serialization::writePod(file_, static_cast<uint32_t>(0));  // Anchor map offset placeholder.
  serialization::writePod(file_, static_cast<uint32_t>(0));  // Paragraph LUT offset placeholder.
  serialization::writePod(file_, static_cast<uint32_t>(0));  // List-item LUT offset placeholder.
}

uint32_t Section::writePage(std::unique_ptr<Page> page) {
  if (!file_) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }
  const uint32_t position = file_.position();
  if (!page->serialize(file_)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);
  pageCount++;
  return position;
}

bool Section::finalize(const std::vector<PageLutEntry>& lut,
                       const std::vector<std::pair<std::string, uint16_t>>& anchors) {
  if (!file_) {
    LOG_ERR("SCT", "File not open for finalize");
    return false;
  }

  // Page LUT: one uint32_t file offset per page.
  const uint32_t lutOffset = file_.position();
  for (const auto& entry : lut) {
    if (entry.fileOffset == 0) {
      LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
      file_.close();
      halStorage.remove(filePath_.c_str());
      return false;
    }
    serialization::writePod(file_, entry.fileOffset);
  }

  // Anchor map: uint16_t count + (length-prefixed string, uint16_t page) entries.
  const uint32_t anchorMapOffset = file_.position();
  serialization::writePod(file_, static_cast<uint16_t>(anchors.size()));
  for (const auto& [anchor, page] : anchors) {
    serialization::writeString(file_, anchor);
    serialization::writePod(file_, page);
  }

  // Paragraph LUT: uint16_t count + paragraphIndex per page.
  const uint32_t paragraphLutOffset = file_.position();
  serialization::writePod(file_, static_cast<uint16_t>(lut.size()));
  for (const auto& entry : lut) {
    serialization::writePod(file_, entry.paragraphIndex);
  }

  // List-item LUT: listItemIndex per page (count shared with paragraph LUT).
  const uint32_t liLutFileOffset = file_.position();
  for (const auto& entry : lut) {
    serialization::writePod(file_, entry.listItemIndex);
  }

  // Patch header placeholders.
  file_.seek(header::kPageCount);
  serialization::writePod(file_, pageCount);
  serialization::writePod(file_, lutOffset);
  serialization::writePod(file_, anchorMapOffset);
  serialization::writePod(file_, paragraphLutOffset);
  serialization::writePod(file_, liLutFileOffset);

  // Explicit close: file_ is a member, destructor would close on Section
  // destruction but we want the cache file flushed to disk now.
  file_.close();
  return true;
}

// --- Read API -------------------------------------------------------------

bool Section::loadHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                         const uint8_t paragraphAlignment, const uint16_t viewportWidth, const uint16_t viewportHeight,
                         const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                         const bool focusReadingEnabled) {
  if (!halStorage.openFileForRead("SCT", filePath_, file_)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file_, version);
  if (version != FILE_VERSION) {
    file_.close();
    LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
    clearCache();
    return false;
  }

  int fileFontId;
  uint16_t fileViewportWidth, fileViewportHeight;
  float fileLineCompression;
  bool fileExtraParagraphSpacing;
  uint8_t fileParagraphAlignment;
  bool fileHyphenationEnabled;
  bool fileEmbeddedStyle;
  uint8_t fileImageRendering;
  bool fileFocusReadingEnabled;
  serialization::readPod(file_, fileFontId);
  serialization::readPod(file_, fileLineCompression);
  serialization::readPod(file_, fileExtraParagraphSpacing);
  serialization::readPod(file_, fileParagraphAlignment);
  serialization::readPod(file_, fileViewportWidth);
  serialization::readPod(file_, fileViewportHeight);
  serialization::readPod(file_, fileHyphenationEnabled);
  serialization::readPod(file_, fileEmbeddedStyle);
  serialization::readPod(file_, fileImageRendering);
  serialization::readPod(file_, fileFocusReadingEnabled);

  if (fontId != fileFontId || lineCompression != fileLineCompression ||
      extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
      viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
      hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle ||
      imageRendering != fileImageRendering || focusReadingEnabled != fileFocusReadingEnabled) {
    file_.close();
    LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
    clearCache();
    return false;
  }

  serialization::readPod(file_, pageCount);
  file_.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

std::unique_ptr<Page> Section::loadPage(const int pageIndex) {
  if (!halStorage.openFileForRead("SCT", filePath_, file_)) {
    return nullptr;
  }

  file_.seek(header::kPageLut);
  uint32_t lutOffset;
  serialization::readPod(file_, lutOffset);
  uint32_t pagePos;
  file_.seek(lutOffset + sizeof(pagePos) * pageIndex);
  serialization::readPod(file_, pagePos);
  file_.seek(pagePos);

  auto page = Page::deserialize(file_);
  file_.close();
  return page;
}

bool Section::forEachAnchor(FunctionRef<bool(uint32_t keyLen)> predicate,
                            FunctionRef<bool(const std::string& key, uint16_t page)> consumer) const {
  HalFile f;
  if (!halStorage.openFileForRead("SCT", filePath_, f)) {
    return false;
  }

  const uint32_t fileSize = f.size();
  f.seek(header::kAnchorMap);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return false;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    uint32_t keyLen;
    serialization::readPod(f, keyLen);
    if (!predicate(keyLen)) {
      f.seek(f.position() + keyLen + sizeof(uint16_t));  // skip key + page
      continue;
    }
    std::string key(keyLen, '\0');
    f.read(reinterpret_cast<uint8_t*>(&key[0]), keyLen);
    uint16_t page;
    serialization::readPod(f, page);
    if (!consumer(key, page)) {
      return true;  // caller asked to stop
    }
  }
  return true;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  std::optional<uint16_t> result;
  forEachAnchor([&anchor](uint32_t keyLen) { return keyLen == anchor.size(); },
                [&](const std::string& key, uint16_t page) {
                  if (key == anchor) {
                    result = page;
                    return false;  // stop iteration
                  }
                  return true;
                });
  return result;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  HalFile f;
  if (!halStorage.openFileForRead("SCT", filePath_, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(header::kParagraphLut);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  uint16_t pagePIdx;
  const uint32_t lutEnd = paragraphLutOffset + sizeof(count) + count * sizeof(pagePIdx);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    serialization::readPod(f, pagePIdx);
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  HalFile f;
  if (!halStorage.openFileForRead("SCT", filePath_, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(header::kParagraphLut);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  uint16_t pIdx;
  const uint32_t entryEnd = paragraphLutOffset + sizeof(count) + (page + 1) * sizeof(pIdx);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset + sizeof(count) + page * sizeof(pIdx));
  serialization::readPod(f, pIdx);
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  HalFile f;
  if (!halStorage.openFileForRead("SCT", filePath_, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  f.seek(header::kListItemLut);
  uint32_t liLutOffset;
  serialization::readPod(f, liLutOffset);
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The list item LUT shares count with the paragraph LUT; read count from paragraphLutOffset.
  f.seek(header::kParagraphLut);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  uint16_t pageLiIdx;
  const uint32_t lutEnd = liLutOffset + count * sizeof(pageLiIdx);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(liLutOffset);
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    serialization::readPod(f, pageLiIdx);
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

bool Section::clearCache() const {
  if (!halStorage.exists(filePath_.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!halStorage.remove(filePath_.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}
