#include "MarkdownParser.h"

#include <EpdFontFamily.h>
#include <HalPlatform.h>
#include <Logging.h>
#include <Memory.h>
#include <Typesetter.h>
#include <Typesetter/LinkEntry.h>
#include <Typesetter/ParsedText.h>
#include <Typesetter/TextAlign.h>
#include <Typesetter/blocks/BlockStyle.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading

bool isHorizontalRule(const std::string& line) {
  if (line.size() < 3) return false;
  char ruleChar = 0;
  int count = 0;
  for (char c : line) {
    if (c == ' ') continue;
    if (c == '-' || c == '*' || c == '_') {
      if (ruleChar == 0) ruleChar = c;
      if (c != ruleChar) return false;
      count++;
    } else {
      return false;
    }
  }
  return count >= 3;
}

// Count leading '>' characters and return the remaining text after stripping them and a trailing space.
int stripBlockquotePrefix(const std::string& line, std::string& out) {
  size_t pos = 0;
  int depth = 0;
  while (pos < line.size() && line[pos] == '>') {
    depth++;
    pos++;
    if (pos < line.size() && line[pos] == ' ') pos++;
  }
  out = line.substr(pos);
  return depth;
}

// Count leading '#' characters (1-6) and return heading level, or 0 if not a heading.
int getHeadingLevel(const std::string& line, std::string& out) {
  size_t level = 0;
  while (level < line.size() && line[level] == '#' && level < 6) {
    level++;
  }
  if (level == 0 || (level < line.size() && line[level] != ' ')) {
    return 0;
  }
  // Skip the space after #
  size_t textStart = level;
  if (textStart < line.size()) textStart++;
  // Strip trailing # characters and whitespace
  size_t end = line.size();
  while (end > textStart && line[end - 1] == '#') end--;
  while (end > textStart && line[end - 1] == ' ') end--;
  out = line.substr(textStart, end - textStart);
  return static_cast<int>(level);
}

// Derive a stable anchor slug from heading text. Lowercases ASCII letters,
// keeps digits, collapses any run of non-alphanumeric ASCII into a single
// '-', strips leading/trailing '-'. Non-ASCII bytes pass through unchanged
// (UTF-8-safe-ish: multi-byte sequences emit verbatim, which is correct
// enough for an anchor identifier).
std::string slugify(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool pendingDash = false;
  for (unsigned char c : text) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) {
      if (pendingDash && !out.empty()) out.push_back('-');
      pendingDash = false;
      out.push_back(static_cast<char>(c));
    } else if (c >= 'A' && c <= 'Z') {
      if (pendingDash && !out.empty()) out.push_back('-');
      pendingDash = false;
      out.push_back(static_cast<char>(c + ('a' - 'A')));
    } else {
      // Any other byte (space, punctuation, control) acts as a separator.
      pendingDash = true;
    }
  }
  return out;
}

// Check if line starts with a list marker (-, *, +) followed by a space.
bool isUnorderedListItem(const std::string& line, std::string& out) {
  size_t pos = 0;
  while (pos < line.size() && line[pos] == ' ') pos++;
  if (pos >= line.size()) return false;
  char marker = line[pos];
  if (marker != '-' && marker != '*' && marker != '+') return false;
  pos++;
  if (pos >= line.size() || line[pos] != ' ') return false;
  pos++;
  out = line.substr(pos);
  return true;
}

// CommonMark "ASCII punctuation" set -- the chars that backslash can escape.
// `\X` where X is in this set emits X literally; otherwise the backslash
// stays as a literal character.
bool isAsciiPunctuation(char c) {
  switch (c) {
    case '!':
    case '"':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case '-':
    case '.':
    case '/':
    case ':':
    case ';':
    case '<':
    case '=':
    case '>':
    case '?':
    case '@':
    case '[':
    case '\\':
    case ']':
    case '^':
    case '_':
    case '`':
    case '{':
    case '|':
    case '}':
    case '~':
      return true;
    default:
      return false;
  }
}

// Inline-text parser state shared between the addStyledWords dispatcher and
// the per-syntax helpers below. The helpers each consume some prefix of
// `text[pos..]`, mutate the state in place, and return true if they matched
// (the dispatcher then loops without further fall-through). Helpers must
// leave `pos` advanced past whatever they consumed.
struct InlineState {
  const std::string& text;
  size_t pos = 0;
  bool isBold = false;
  bool isItalic = false;
  std::string currentToken;
  ParsedText* block = nullptr;
  bool* hasPendingContent = nullptr;
  // Used by tryLink to register each [text](target) as a pending link via
  // typesetter->addPendingLink; the typesetter attaches it to the page
  // that ends up containing the link's first word during submitParagraph.
  Typesetter* typesetter = nullptr;

  EpdFontFamily::Style computeStyle() const {
    uint8_t s = EpdFontFamily::REGULAR;
    if (isBold) s |= EpdFontFamily::BOLD;
    if (isItalic) s |= EpdFontFamily::ITALIC;
    return static_cast<EpdFontFamily::Style>(s);
  }

  void flushToken() {
    if (!currentToken.empty()) {
      block->addWord(std::move(currentToken), computeStyle());
      *hasPendingContent = true;
      currentToken.clear();
    }
  }
};

// \X where X is ASCII punctuation: emit X as a literal char, swallow both.
// Otherwise (e.g. \n where n is a letter) the helper declines and the
// backslash falls through to literal-character accumulation. Must dispatch
// FIRST so `\*` doesn't trigger emphasis, `\[` doesn't trigger link parse,
// etc. Block-level escapes (`\#` not a heading, `\>` not a blockquote) work
// for free because the block-level classifiers only inspect `line[0]`,
// which is `\\` rather than the syntax character.
bool tryEscape(InlineState& s) {
  if (s.text[s.pos] != '\\') return false;
  if (s.pos + 1 >= s.text.size()) return false;
  const char next = s.text[s.pos + 1];
  if (!isAsciiPunctuation(next)) return false;
  s.currentToken += next;
  s.pos += 2;
  return true;
}

// *...*, _..._, **...**, __...__, ***...***, ___...___. Counts marker run
// length: 1 toggles italic, 2 toggles bold, >=3 toggles both. Flushes any
// pending token at the style boundary so each run gets a clean state.
bool tryEmphasis(InlineState& s) {
  char c = s.text[s.pos];
  if (c != '*' && c != '_') return false;
  char marker = c;
  size_t markerCount = 0;
  while (s.pos < s.text.size() && s.text[s.pos] == marker) {
    markerCount++;
    s.pos++;
  }
  s.flushToken();
  if (markerCount >= 3) {
    s.isBold = !s.isBold;
    s.isItalic = !s.isItalic;
  } else if (markerCount == 2) {
    s.isBold = !s.isBold;
  } else {
    s.isItalic = !s.isItalic;
  }
  return true;
}

// [text](target). Emits `text` words with underline=true; the target string
// is parsed but discarded (Layer 3 will add storage + activation UX). Images
// (![alt](src)) are handled by leaving the leading '!' in the token buffer:
// if the in-flight token ends with '!', the '[' is treated as literal so
// "![alt](src)" emits as one word. Returns false (no consume) if the
// bracket/paren pair is missing -- the '[' then falls through to literal
// character accumulation.
//
// Escape-aware scanning: `\]` inside link text and `\)` inside link target
// don't terminate. The link text is de-escaped before tokenization so
// `[a\]b](url)` emits a single underlined word "a]b".
//
// Balanced parens: the target scan tracks `(`/`)` depth so URLs like
// `Foo_(disambig)` don't clip at the inner `)`. Escaped parens (`\(` /
// `\)`) do not contribute to depth.
bool tryLink(InlineState& s) {
  if (s.text[s.pos] != '[') return false;
  if (!s.currentToken.empty() && s.currentToken.back() == '!') return false;  // image

  // Find closing ']' respecting backslash escapes inside the link text.
  size_t closeBracket = std::string::npos;
  for (size_t i = s.pos + 1; i < s.text.size(); i++) {
    if (s.text[i] == '\\' && i + 1 < s.text.size()) {
      i++;  // skip the escaped char
      continue;
    }
    if (s.text[i] == ']') {
      closeBracket = i;
      break;
    }
  }
  if (closeBracket == std::string::npos) return false;
  if (closeBracket + 1 >= s.text.size() || s.text[closeBracket + 1] != '(') return false;

  // Find the matching close-paren with depth tracking. Escapes are skipped
  // without affecting depth.
  size_t closeParen = std::string::npos;
  int depth = 0;
  for (size_t i = closeBracket + 2; i < s.text.size(); i++) {
    if (s.text[i] == '\\' && i + 1 < s.text.size()) {
      i++;
      continue;
    }
    char c = s.text[i];
    if (c == '(') {
      depth++;
    } else if (c == ')') {
      if (depth == 0) {
        closeParen = i;
        break;
      }
      depth--;
    }
  }
  if (closeParen == std::string::npos) return false;

  s.flushToken();

  // De-escape link text into a flat buffer, then tokenize on whitespace.
  std::string linkText;
  linkText.reserve(closeBracket - s.pos - 1);
  for (size_t i = s.pos + 1; i < closeBracket; i++) {
    if (s.text[i] == '\\' && i + 1 < closeBracket) {
      linkText.push_back(s.text[i + 1]);
      i++;
    } else {
      linkText.push_back(s.text[i]);
    }
  }

  // De-escape link target the same way (`\)` inside the URL renders as `)`).
  std::string linkTarget;
  linkTarget.reserve(closeParen - closeBracket - 2);
  for (size_t i = closeBracket + 2; i < closeParen; i++) {
    if (s.text[i] == '\\' && i + 1 < closeParen) {
      linkTarget.push_back(s.text[i + 1]);
      i++;
    } else {
      linkTarget.push_back(s.text[i]);
    }
  }

  // Register the link with the typesetter so it can be attached to the page
  // that contains its first word. Word index is the position the first link
  // word will land at within the paragraph -- block->size() reflects words
  // submitted so far, and the next addWord call lands at exactly that index.
  // Typesetter resets the per-paragraph word counter inside submitParagraph,
  // so this index is paragraph-local.
  if (s.typesetter != nullptr && !linkText.empty()) {
    LinkEntry entry;
    std::strncpy(entry.label, linkText.c_str(), sizeof(entry.label) - 1);
    entry.label[sizeof(entry.label) - 1] = '\0';
    std::strncpy(entry.href, linkTarget.c_str(), sizeof(entry.href) - 1);
    entry.href[sizeof(entry.href) - 1] = '\0';
    s.typesetter->addPendingLink(static_cast<int>(s.block->size()), entry);
  }

  size_t tp = 0;
  while (tp < linkText.size()) {
    while (tp < linkText.size() && (linkText[tp] == ' ' || linkText[tp] == '\t')) tp++;
    if (tp >= linkText.size()) break;
    size_t wordStart = tp;
    while (tp < linkText.size() && linkText[tp] != ' ' && linkText[tp] != '\t') tp++;
    std::string word = linkText.substr(wordStart, tp - wordStart);
    s.block->addWord(std::move(word), s.computeStyle(), /*underline=*/true);
    *s.hasPendingContent = true;
  }
  s.pos = closeParen + 1;
  return true;
}

// `code`. Emits one REGULAR-styled word for the content between backticks;
// surrounding bold/italic context does NOT apply (matches CommonMark).
// Unclosed code spans (no matching `) consume to end of line.
bool tryInlineCode(InlineState& s) {
  if (s.text[s.pos] != '`') return false;
  s.flushToken();
  s.pos++;
  size_t codeStart = s.pos;
  while (s.pos < s.text.size() && s.text[s.pos] != '`') s.pos++;
  std::string codeText = s.text.substr(codeStart, s.pos - codeStart);
  if (s.pos < s.text.size()) s.pos++;  // skip closing `
  if (!codeText.empty()) {
    s.block->addWord(std::move(codeText), EpdFontFamily::REGULAR);
    *s.hasPendingContent = true;
  }
  return true;
}

// Space or tab run: flushes the in-flight token and skips the whitespace.
// A run of multiple spaces collapses to a single token boundary.
bool tryWhitespace(InlineState& s) {
  char c = s.text[s.pos];
  if (c != ' ' && c != '\t') return false;
  s.flushToken();
  s.pos++;
  while (s.pos < s.text.size() && (s.text[s.pos] == ' ' || s.text[s.pos] == '\t')) s.pos++;
  return true;
}
}  // namespace

struct MarkdownParser::ParagraphState {
  enum class Context : uint8_t { Body, Blockquote, Code };

  Context context = Context::Body;
  std::unique_ptr<ParsedText> currentBlock;
  bool hasPendingContent = false;
  int blockquoteDepth = 0;

  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  TextAlign defaultAlign;

  ParagraphState(bool eps, bool hyph, bool focus, TextAlign align)
      : extraParagraphSpacing(eps), hyphenationEnabled(hyph), focusReadingEnabled(focus), defaultAlign(align) {}

  bool beginBlock(const BlockStyle& style) {
    currentBlock = makeUniqueNoThrow<ParsedText>(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled);
    if (!currentBlock) {
      LOG_ERR("MDP", "OOM: ParsedText");
      return false;
    }
    currentBlock->setBlockStyle(style);
    hasPendingContent = false;
    return true;
  }

  BlockStyle makeBodyStyle() const {
    BlockStyle style;
    style.alignment = defaultAlign;
    style.textAlignDefined = true;
    return style;
  }
};

MarkdownParser::~MarkdownParser() = default;

MarkdownParser::MarkdownParser(Typesetter& typesetter, ReadFn readFn, size_t fileSize, bool extraParagraphSpacing,
                               bool hyphenationEnabled, bool focusReadingEnabled, uint8_t paragraphAlignment)
    : typesetter(typesetter),
      readFn(readFn),
      fileSize(fileSize),
      extraParagraphSpacing(extraParagraphSpacing),
      hyphenationEnabled(hyphenationEnabled),
      focusReadingEnabled(focusReadingEnabled),
      paragraphAlignment(paragraphAlignment) {}

bool MarkdownParser::parse() {
  const auto userAlign = static_cast<TextAlign>(paragraphAlignment);
  const auto defaultAlign = (userAlign == TextAlign::None) ? TextAlign::Left : userAlign;

  state =
      makeUniqueNoThrow<ParagraphState>(extraParagraphSpacing, hyphenationEnabled, focusReadingEnabled, defaultAlign);
  if (!state) {
    LOG_ERR("MDP", "OOM: ParagraphState");
    return false;
  }
  if (!state->beginBlock(state->makeBodyStyle())) {
    return false;
  }

  auto buffer = makeUniqueNoThrow<uint8_t[]>(CHUNK_SIZE + 1);
  if (!buffer) {
    LOG_ERR("MDP", "Failed to allocate read buffer");
    return false;
  }

  // Line accumulation across chunk boundaries
  std::string lineBuffer;
  size_t offset = 0;

  while (offset < fileSize) {
    const size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
    if (!readFn(buffer.get(), offset, chunkSize)) {
      break;
    }
    buffer[chunkSize] = '\0';

    size_t pos = 0;
    while (pos < chunkSize) {
      // Skip carriage returns
      if (buffer[pos] == '\r') {
        pos++;
        continue;
      }

      if (buffer[pos] == '\n') {
        pos++;
        processLine(lineBuffer);
        lineBuffer.clear();
        continue;
      }

      // Accumulate into line buffer
      size_t lineEnd = pos;
      while (lineEnd < chunkSize && buffer[lineEnd] != '\n' && buffer[lineEnd] != '\r') {
        lineEnd++;
      }
      lineBuffer.append(reinterpret_cast<char*>(&buffer[pos]), lineEnd - pos);
      pos = lineEnd;
    }

    offset += chunkSize;

    // Yield to other tasks periodically
    if ((offset & 0xFFFF) == 0) {
      halPlatform.delay(1);
    }
  }

  // Process any remaining content in line buffer
  if (!lineBuffer.empty()) {
    processLine(lineBuffer);
  }

  // Flush final paragraph
  flushCurrentParagraph();

  state.reset();
  return true;
}

void MarkdownParser::flushCurrentParagraph() {
  if (state->currentBlock && !state->currentBlock->isEmpty()) {
    typesetter.submitParagraph(std::move(state->currentBlock));
  }
  state->beginBlock(state->makeBodyStyle());
  state->context = ParagraphState::Context::Body;
}

void MarkdownParser::processLine(const std::string& line) {
  // Handle fenced code block toggle
  if (line.size() >= 3 && line.substr(0, 3) == "```") {
    if (state->context == ParagraphState::Context::Code) {
      flushCurrentParagraph();
    } else {
      flushCurrentParagraph();
      state->context = ParagraphState::Context::Code;
      BlockStyle codeStyle;
      codeStyle.alignment = TextAlign::Left;
      codeStyle.textAlignDefined = true;
      state->beginBlock(codeStyle);
    }
    return;
  }

  if (state->context == ParagraphState::Context::Code) {
    emitCodeLine(line);
    return;
  }

  // Blank line ends current paragraph
  if (line.empty() || line.find_first_not_of(' ') == std::string::npos) {
    flushCurrentParagraph();
    return;
  }

  // Horizontal rule
  if (isHorizontalRule(line)) {
    flushCurrentParagraph();
    return;
  }

  // Heading
  std::string headingText;
  int headingLevel = getHeadingLevel(line, headingText);
  if (headingLevel > 0) {
    flushCurrentParagraph();
    emitHeading(headingText, headingLevel);
    return;
  }

  // Blockquote
  std::string bqText;
  int bqDepth = stripBlockquotePrefix(line, bqText);
  if (bqDepth > 0) {
    if (state->context != ParagraphState::Context::Blockquote || state->blockquoteDepth != bqDepth) {
      flushCurrentParagraph();
      state->context = ParagraphState::Context::Blockquote;
      state->blockquoteDepth = bqDepth;
    }
    emitBlockquoteLine(bqText);
    return;
  }

  // Unordered list item
  std::string listText;
  if (isUnorderedListItem(line, listText)) {
    flushCurrentParagraph();
    emitListItem(listText);
    return;
  }

  // Regular body text -- continuation within same paragraph
  if (state->context != ParagraphState::Context::Body) {
    flushCurrentParagraph();
  }
  emitBodyLine(line);
}

void MarkdownParser::emitHeading(const std::string& text, int level) {
  BlockStyle style;
  style.alignment = TextAlign::Center;
  style.textAlignDefined = true;
  // Add vertical spacing around headings, scaled by level
  const int16_t spacing = static_cast<int16_t>(std::max(4, 16 - level * 2));
  style.marginTop = spacing;
  style.marginBottom = spacing;

  if (!state->beginBlock(style)) return;

  // Headings are always bold
  addStyledWords(text, EpdFontFamily::BOLD);
  flushCurrentParagraph();

  // Record an anchor entry for the heading. Page is read AFTER the flush
  // so that if the heading triggered a page overflow (previous content
  // emitted and the heading itself moved to a fresh page),
  // getCompletedPageCount reflects the page the heading actually landed on.
  // Headings that overflow internally are rare; we accept the edge case.
  const std::string slug = slugify(text);
  if (!slug.empty()) {
    anchors.push_back({slug, static_cast<uint16_t>(typesetter.getCompletedPageCount())});
  }
}

void MarkdownParser::emitListItem(const std::string& text) {
  BlockStyle style = state->makeBodyStyle();
  style.alignment = TextAlign::Left;
  style.textAlignDefined = true;

  if (!state->beginBlock(style)) return;

  // Add bullet prefix (UTF-8 BULLET U+2022)
  state->currentBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR);

  addStyledWords(text, EpdFontFamily::REGULAR);
  flushCurrentParagraph();
}

void MarkdownParser::emitBlockquoteLine(const std::string& text) {
  if (!state->currentBlock || state->currentBlock->isEmpty()) {
    BlockStyle style = state->makeBodyStyle();
    style.marginLeft = static_cast<int16_t>(16 * state->blockquoteDepth);
    style.alignment = TextAlign::Left;
    style.textAlignDefined = true;
    if (!state->beginBlock(style)) return;
  }

  addStyledWords(text, EpdFontFamily::ITALIC);
}

void MarkdownParser::emitCodeLine(const std::string& text) {
  // Each code line is its own paragraph with no indent and left alignment
  BlockStyle style;
  style.alignment = TextAlign::Left;
  style.textAlignDefined = true;

  // No extra spacing, no hyphenation, no focus reading inside code blocks
  auto block = makeUniqueNoThrow<ParsedText>(false, false, false);
  if (!block) {
    LOG_ERR("MDP", "OOM: ParsedText (code line)");
    return;
  }
  block->setBlockStyle(style);

  if (text.empty()) {
    // Blank line in code block -- emit a space to preserve vertical spacing
    block->addWord(" ", EpdFontFamily::REGULAR);
  } else {
    // Split code line by spaces but preserve multiple spaces as part of indentation
    size_t pos = 0;
    while (pos < text.size()) {
      if (text[pos] == ' ' || text[pos] == '\t') {
        pos++;
        continue;
      }
      size_t tokenStart = pos;
      while (pos < text.size() && text[pos] != ' ' && text[pos] != '\t') {
        pos++;
      }
      std::string token = text.substr(tokenStart, pos - tokenStart);
      block->addWord(std::move(token), EpdFontFamily::REGULAR);
    }
  }

  typesetter.submitParagraph(std::move(block));
}

void MarkdownParser::emitBodyLine(const std::string& text) {
  state->context = ParagraphState::Context::Body;
  if (!state->currentBlock || state->currentBlock->isEmpty()) {
    if (!state->beginBlock(state->makeBodyStyle())) return;
  }
  addStyledWords(text, EpdFontFamily::REGULAR);
}

void MarkdownParser::addStyledWords(const std::string& text, uint8_t baseStyle) {
  if (!state->currentBlock) {
    if (!state->beginBlock(state->makeBodyStyle())) return;
  }

  InlineState s{
      .text = text,
      .pos = 0,
      .isBold = (baseStyle & EpdFontFamily::BOLD) != 0,
      .isItalic = (baseStyle & EpdFontFamily::ITALIC) != 0,
      .currentToken = {},
      .block = state->currentBlock.get(),
      .hasPendingContent = &state->hasPendingContent,
      .typesetter = &typesetter,
  };

  // Dispatch: each helper inspects text[pos], consumes some prefix, returns
  // true if it matched. tryEscape MUST go first so `\*` doesn't trigger
  // emphasis, `\[` doesn't trigger link parse, etc.; the rest are disjoint
  // by leading char so any order after that works.
  while (s.pos < s.text.size()) {
    if (tryEscape(s)) continue;
    if (tryEmphasis(s)) continue;
    if (tryLink(s)) continue;
    if (tryInlineCode(s)) continue;
    if (tryWhitespace(s)) continue;
    // Literal char: accumulate into the in-flight token.
    s.currentToken += s.text[s.pos];
    s.pos++;
  }

  s.flushToken();
}
