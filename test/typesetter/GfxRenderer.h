#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Test-only stub of GfxRenderer. Shadows lib/GfxRenderer/GfxRenderer.h via
// CMake include path priority in test/typesetter/CMakeLists.txt. Provides
// just enough surface to satisfy the linker AND yield deterministic layout
// results when test code drives Typesetter through ParsedText. Methods are
// inline so the linker resolves everything from this header alone -- no
// stub .cpp needed.
//
// Width model: kPxPerChar pixels per byte for ASCII strings, plus a fixed
// space advance. The model is intentionally simple so tests can predict line
// breaks from `viewportWidth / (wordLen * kPxPerChar + space)` and assert
// page counts without depending on real font metrics. Tests pick word
// lengths and viewport widths that make the arithmetic clean.

class GfxRenderer {
 public:
  // Width model constants. Public so tests can compute expectations.
  static constexpr int kPxPerChar = 10;
  static constexpr int kSpaceAdvance = 5;
  static constexpr int kLineHeight = 20;
  static constexpr int kAscender = 15;

  int getLineHeight([[maybe_unused]] int fontId) const { return kLineHeight; }
  int getFontAscenderSize([[maybe_unused]] int fontId) const { return kAscender; }

  // Width of `text` in pixels: kPxPerChar per byte.
  int getTextWidth([[maybe_unused]] int fontId, const char* text,
                   [[maybe_unused]] EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return text ? static_cast<int>(std::strlen(text)) * kPxPerChar : 0;
  }

  // Advance equals width for ASCII (no kerning).
  int getTextAdvanceX([[maybe_unused]] int fontId, const char* text,
                      [[maybe_unused]] EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return getTextWidth(0, text);
  }

  int getSpaceWidth([[maybe_unused]] int fontId,
                    [[maybe_unused]] EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return kSpaceAdvance;
  }

  int getSpaceAdvance([[maybe_unused]] int fontId, [[maybe_unused]] uint32_t leftCp, [[maybe_unused]] uint32_t rightCp,
                      [[maybe_unused]] EpdFontFamily::Style style) const {
    return kSpaceAdvance;
  }

  int getKerning([[maybe_unused]] int fontId, [[maybe_unused]] uint32_t leftCp, [[maybe_unused]] uint32_t rightCp,
                 [[maybe_unused]] EpdFontFamily::Style style) const {
    return 0;
  }

  // No SD-card fonts in the stub; lay-out code skips the pre-warm path.
  bool isSdCardFont([[maybe_unused]] int fontId) const { return false; }
  void ensureSdCardFontReady([[maybe_unused]] int fontId, [[maybe_unused]] const char* utf8Text,
                             [[maybe_unused]] uint8_t styleMask = 0x0F) const {}
  void ensureSdCardFontReady([[maybe_unused]] int fontId, [[maybe_unused]] const std::vector<std::string>& words,
                             [[maybe_unused]] bool includeHyphen, [[maybe_unused]] uint8_t styleMask = 0x0F) const {}

  // Drawing operations: no-ops. Tests inspect emitted-page metadata, not pixels.
  void drawText(int, int, int, const char*, bool, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {}
  void drawLine(int, int, int, int, int, bool) const {}
  void drawLine(int, int, int, int, bool) const {}

  // Viewport accessors: fixed values; ImageBlock::render reads these but
  // Tier 1/2 tests don't invoke it.
  int getScreenWidth() const { return 800; }
  int getScreenHeight() const { return 480; }
};
