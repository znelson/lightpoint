#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Epub.h"
#include "SectionCache.h"

class Page;
class GfxRenderer;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  SectionCache cache;

 public:
  uint16_t& pageCount;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        cache(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin"),
        pageCount(cache.pageCount) {}
  ~Section() = default;

  bool loadSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                       uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                       uint8_t imageRendering, bool focusReadingEnabled);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, bool extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool embeddedStyle,
                         uint8_t imageRendering, bool focusReadingEnabled,
                         const std::function<void()>& popupFn = nullptr);
  std::unique_ptr<Page> loadPageFromSectionFile();

  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  SectionCache& getCache() { return cache; }
  const SectionCache& getCache() const { return cache; }
};
