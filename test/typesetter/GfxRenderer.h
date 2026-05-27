#pragma once

// Test-only stub of GfxRenderer. Shadows lib/GfxRenderer/GfxRenderer.h via
// CMake include path priority in test/typesetter/CMakeLists.txt. Provides
// just enough surface to satisfy the linker when test code constructs a
// Typesetter and exercises its Tier 1 surface (forcePageBreak, finish,
// submitImage, xpath/footnote accessors). Methods are inline no-ops or
// fixed-return-value defaults so tests stay deterministic.
//
// Production GfxRenderer.h pulls in EpdFontFamily, HalDisplay, Bitmap, and a
// large surface of drawing methods. None of those are needed for Tier 1
// Typesetter tests, so the stub keeps the test build small.

class GfxRenderer {
 public:
  // Called by Typesetter::addLineToPage / submitParagraph / submitHorizontalRule.
  // Fixed value chosen so a deterministic number of lines fits in a known viewport.
  int getLineHeight(int /*fontId*/) const { return 20; }

  // PageHorizontalRule::render calls this; only matters if a Page is actually
  // rendered (Tier 1 tests do not invoke Page::render).
  void drawLine(int, int, int, int, int, bool) {}
  void drawLine(int, int, int, int, bool) {}

  // ImageBlock::render reads these; Tier 1 tests do not invoke it.
  int getScreenWidth() const { return 800; }
  int getScreenHeight() const { return 480; }
};
