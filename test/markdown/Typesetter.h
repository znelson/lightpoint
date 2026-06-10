#pragma once

// Test-only stub of Typesetter. Shadows lib/Typesetter/Typesetter.h via
// CMake include-path priority in test/markdown/CMakeLists.txt. Provides
// just the surface MarkdownParser invokes (`submitParagraph`,
// `getCompletedPageCount`, `finish`) without dragging in the real layout
// engine, GfxRenderer, Hyphenator, etc. Submitted paragraphs accumulate
// in `submitted` so tests can inspect them; `completedPageCount` is
// settable so anchor-recording tests can simulate pagination by bumping
// the counter between submits.

#include <Typesetter/LinkEntry.h>
#include <Typesetter/ParsedText.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

class Typesetter {
 public:
  // Test ctor: no GfxRenderer, no callback. Tests construct directly.
  Typesetter() = default;
  virtual ~Typesetter() = default;

  // virtual so tests can subclass to inject side effects (see CountingTypesetter
  // in MarkdownParserTest). The production Typesetter is non-virtual; this is
  // a test-only relaxation that doesn't affect production code.
  virtual void submitParagraph(std::unique_ptr<ParsedText> p) { submitted.push_back(std::move(p)); }

  void finish() { finished = true; }

  size_t getCompletedPageCount() const { return completedPageCount; }

  // Test-only knob: lets a test simulate "the page counter advanced because
  // this paragraph filled a page." Call between submits.
  void setCompletedPageCount(size_t n) { completedPageCount = n; }

  // Captures pending link registrations from MarkdownParser::tryLink so tests
  // can verify label, href, and word-index against expectations.
  struct CapturedLink {
    size_t wordIndex;
    LinkEntry entry;
  };
  void addPendingLink(size_t wordIndex, const LinkEntry& entry) { pendingLinks.push_back({wordIndex, entry}); }

  std::vector<std::unique_ptr<ParsedText>> submitted;
  std::vector<CapturedLink> pendingLinks;
  bool finished = false;
  size_t completedPageCount = 0;
};
