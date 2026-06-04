#pragma once

#include <FunctionRef.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Typesetter;

// MarkdownParser -- streaming line-oriented Markdown parser that feeds a Typesetter.
//
// Supported syntax:
//   # / ## / ### / #### headings (bold, centered)
//   **bold** and __bold__
//   *italic* and _italic_
//   ***bold italic*** and ___bold italic___
//   - / * / + unordered list items (bullet prefix, left-aligned)
//   > blockquotes (left margin inset)
//   ``` fenced code blocks (rendered as-is, no wrapping)
//   --- / *** / ___ horizontal rules (blank paragraph spacing)
//
// The parser reads the file in chunks and emits ParsedText paragraphs to the
// Typesetter. It does not build a DOM or AST -- each line is classified and
// fed directly into the current paragraph context.
//
// Lifetime: the FunctionRef `readFn` is stored as a member; the underlying
// callable MUST outlive the MarkdownParser. In practice the parser is a
// local in the activity's build path and the lambda is in the same scope.
class MarkdownParser {
 public:
  using ReadFn = FunctionRef<bool(uint8_t* buffer, size_t offset, size_t length)>;

  MarkdownParser(Typesetter& typesetter, ReadFn readFn, size_t fileSize, bool extraParagraphSpacing,
                 bool hyphenationEnabled, bool focusReadingEnabled, uint8_t paragraphAlignment);

  // Out-of-line so the unique_ptr<ParagraphState> destructor sees the
  // complete type (defined only in the .cpp).
  ~MarkdownParser();

  bool parse();

  // Heading anchors collected during parse(). Each entry is
  // {slug, pageIndex} where pageIndex is the page on which the heading
  // landed (taken from typesetter.getCompletedPageCount() right after
  // the heading paragraph was submitted). Empty until parse() runs.
  // The caller passes these to Section::finalize() so Section's
  // getPageForAnchor() can resolve `[link](#slug)` to a page later.
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchors; }

 private:
  Typesetter& typesetter;
  ReadFn readFn;
  size_t fileSize;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  uint8_t paragraphAlignment;

  void processLine(const std::string& line);
  void flushCurrentParagraph();

  void emitHeading(const std::string& text, int level);
  void emitListItem(const std::string& text);
  void emitBlockquoteLine(const std::string& text);
  void emitCodeLine(const std::string& text);
  void emitBodyLine(const std::string& text);
  void addStyledWords(const std::string& text, uint8_t baseStyle);

  // Paragraph accumulation state
  struct ParagraphState;
  std::unique_ptr<ParagraphState> state;

  std::vector<std::pair<std::string, uint16_t>> anchors;
};
