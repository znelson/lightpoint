#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class GfxRenderer;
class SdCardFont;
struct SdCardFontFamilyInfo;

class SdCardFontManager {
 public:
  // Both defined out-of-line so members holding unique_ptr<SdCardFont>
  // instantiate where SdCardFont is complete
  SdCardFontManager();
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;
  SdCardFontManager(SdCardFontManager&&) = delete;
  SdCardFontManager& operator=(SdCardFontManager&&) = delete;

  // Load the font file matching fontSizeEnum (SMALL=0 .. EXTRA_LARGE=3) by
  // ordinal position in the family's sorted size list. Only one .cpfont file
  // is loaded; other sizes remain on disk. This keeps resident interval +
  // kern/ligature tables to one size's worth of memory.
  // Returns true on success.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t fontSizeEnum);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Look up the font ID for the loaded family. Returns 0 if nothing loaded
  // or familyName doesn't match.
  int getFontId(const std::string& familyName) const;

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

  // Point size that was actually loaded (closest match to targetPtSize).
  // 0 if nothing loaded.
  uint8_t currentPointSize() const { return loadedPointSize_; };

 private:
  struct LoadedFont {
    std::unique_ptr<SdCardFont> font;  // owned; the renderer's map holds a non-owning pointer
    int fontId;
    uint8_t size;
  };
  static int computeFontId(size_t contentHash, const char* familyName, uint8_t pointSize);

  std::string loadedFamilyName_;
  uint8_t loadedPointSize_ = 0;
  std::vector<LoadedFont> loaded_;
};
