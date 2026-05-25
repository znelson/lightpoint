#include "StreamingJsonParser.h"

#include <Utf8.h>

#include <cstring>

StreamingJsonParser::StreamingJsonParser(const JsonCallbacks& callbacks) : cb(callbacks) { reset(); }

void StreamingJsonParser::reset() {
  tokenLen = 0;
  state = State::SCANNING;
  expectingValue = false;
  escaped = false;
  tokenOverflow = false;
  error = false;
  nestingDepth = 0;
  literalLen = 0;
  literalPos = 0;
  unicodeHexRemaining = 0;
  unicodeHexAccum = 0;
}

void StreamingJsonParser::feed(const char* data, size_t len) {
  for (size_t i = 0; i < len && !error; ++i) {
    char c = data[i];
    switch (state) {
      case State::SCANNING:
        handleScanning(c);
        break;
      case State::IN_STRING_KEY:
      case State::IN_STRING_VALUE:
        handleStringChar(c);
        break;
      case State::IN_NUMBER:
        handleNumber(c);
        break;
      case State::IN_LITERAL:
        handleLiteral(c);
        break;
      case State::SKIP_STRING:
        handleSkipString(c);
        break;
    }
  }
}

void StreamingJsonParser::handleScanning(char c) {
  switch (c) {
    case '"':
      tokenLen = 0;
      tokenOverflow = false;
      if (expectingValue || inArray()) {
        state = State::IN_STRING_VALUE;
      } else {
        state = State::IN_STRING_KEY;
      }
      break;
    case '{':
      if (nestingDepth < MAX_NESTING) {
        nestingStack[nestingDepth++] = Container::OBJECT;
      } else {
        error = true;
        return;
      }
      if (cb.onObjectStart) cb.onObjectStart(cb.ctx);
      expectingValue = false;
      break;
    case '}':
      if (cb.onObjectEnd) cb.onObjectEnd(cb.ctx);
      if (nestingDepth > 0) --nestingDepth;
      expectingValue = false;
      break;
    case '[':
      if (nestingDepth < MAX_NESTING) {
        nestingStack[nestingDepth++] = Container::ARRAY;
      } else {
        error = true;
        return;
      }
      if (cb.onArrayStart) cb.onArrayStart(cb.ctx);
      expectingValue = false;
      break;
    case ']':
      if (cb.onArrayEnd) cb.onArrayEnd(cb.ctx);
      if (nestingDepth > 0) --nestingDepth;
      expectingValue = false;
      break;
    case ':':
      expectingValue = true;
      break;
    case ',':
      expectingValue = false;
      break;
    case 't':
      if (expectingValue || inArray()) {
        memcpy(literalExpected, "true", 4);
        literalLen = 4;
        literalPos = 1;
        state = State::IN_LITERAL;
      }
      break;
    case 'f':
      if (expectingValue || inArray()) {
        memcpy(literalExpected, "false", 5);
        literalLen = 5;
        literalPos = 1;
        state = State::IN_LITERAL;
      }
      break;
    case 'n':
      if (expectingValue || inArray()) {
        memcpy(literalExpected, "null", 4);
        literalLen = 4;
        literalPos = 1;
        state = State::IN_LITERAL;
      }
      break;
    default:
      if ((expectingValue || inArray()) && (c == '-' || (c >= '0' && c <= '9'))) {
        tokenLen = 0;
        tokenOverflow = false;
        appendToken(c);
        state = State::IN_NUMBER;
      }
      break;
  }
}

void StreamingJsonParser::handleStringChar(char c) {
  if (unicodeHexRemaining > 0) {
    handleUnicodeHexDigit(c);
    return;
  }

  if (escaped) {
    escaped = false;
    if (c == 'u') {
      unicodeHexRemaining = 4;
      unicodeHexAccum = 0;
      return;
    }
    switch (c) {
      case '"':
      case '\\':
      case '/':
        appendToken(c);
        break;
      case 'b':
        appendToken('\b');
        break;
      case 'f':
        appendToken('\f');
        break;
      case 'n':
        appendToken('\n');
        break;
      case 'r':
        appendToken('\r');
        break;
      case 't':
        appendToken('\t');
        break;
      default:
        appendToken('\\');
        appendToken(c);
        break;
    }
    return;
  }

  if (c == '\\') {
    escaped = true;
    return;
  }

  if (c == '"') {
    emitToken();
    return;
  }

  appendToken(c);
}

void StreamingJsonParser::handleUnicodeHexDigit(char c) {
  int digit;
  if (c >= '0' && c <= '9')
    digit = c - '0';
  else if (c >= 'a' && c <= 'f')
    digit = c - 'a' + 10;
  else if (c >= 'A' && c <= 'F')
    digit = c - 'A' + 10;
  else {
    // Malformed escape: replace the in-flight code unit with U+FFFD, then
    // re-process the offending byte as a regular string char so the rest of
    // the document still parses.
    appendCodepointUtf8(REPLACEMENT_GLYPH);
    unicodeHexRemaining = 0;
    handleStringChar(c);
    return;
  }

  unicodeHexAccum = static_cast<uint16_t>((unicodeHexAccum << 4) | digit);
  --unicodeHexRemaining;
  if (unicodeHexRemaining > 0) return;

  // Hand the 16-bit value to the UTF-8 encoder. Values in the surrogate
  // range (U+D800-U+DFFF) are encoded as U+FFFD by utf8EncodeCodepoint,
  // which is the safe choice since we never see surrogate pairs in real
  // inputs (all our JSON sources emit raw UTF-8, not \u escapes).
  appendCodepointUtf8(unicodeHexAccum);
}

void StreamingJsonParser::appendCodepointUtf8(uint32_t cp) {
  char buf[4];
  int n = utf8EncodeCodepoint(cp, buf);
  for (int i = 0; i < n; ++i) appendToken(buf[i]);
}

void StreamingJsonParser::handleNumber(char c) {
  if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') {
    appendToken(c);
    return;
  }

  if (!tokenOverflow && cb.onNumber) {
    tokenBuf[tokenLen] = '\0';
    cb.onNumber(cb.ctx, tokenBuf, tokenLen);
  }
  state = State::SCANNING;
  expectingValue = false;

  handleScanning(c);
}

void StreamingJsonParser::handleLiteral(char c) {
  if (c == literalExpected[literalPos]) {
    ++literalPos;
    if (literalPos == literalLen) {
      if (literalExpected[0] == 't') {
        if (cb.onBool) cb.onBool(cb.ctx, true);
      } else if (literalExpected[0] == 'f') {
        if (cb.onBool) cb.onBool(cb.ctx, false);
      } else {
        if (cb.onNull) cb.onNull(cb.ctx);
      }
      state = State::SCANNING;
      expectingValue = false;
    }
  } else {
    error = true;
  }
}

void StreamingJsonParser::handleSkipString(char c) {
  if (escaped) {
    escaped = false;
    return;
  }
  if (c == '\\') {
    escaped = true;
    return;
  }
  if (c == '"') {
    state = State::SCANNING;
    expectingValue = false;
  }
}

void StreamingJsonParser::appendToken(char c) {
  if (tokenLen < TOKEN_BUF_SIZE - 1) {
    tokenBuf[tokenLen++] = c;
  } else {
    tokenOverflow = true;
  }
}

void StreamingJsonParser::emitToken() {
  if (state == State::IN_STRING_KEY) {
    if (!tokenOverflow && cb.onKey) {
      tokenBuf[tokenLen] = '\0';
      cb.onKey(cb.ctx, tokenBuf, tokenLen);
    }
    state = State::SCANNING;
  } else {
    if (!tokenOverflow && cb.onString) {
      tokenBuf[tokenLen] = '\0';
      cb.onString(cb.ctx, tokenBuf, tokenLen);
    }
    state = State::SCANNING;
    expectingValue = false;
  }
}
