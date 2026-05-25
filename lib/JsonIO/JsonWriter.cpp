#include "JsonWriter.h"

#include <cstdio>

JsonWriter::JsonWriter(JsonSink& sink) : sink_(sink), depth_(0), justEmittedKey_(false) {}

void JsonWriter::beginObject() {
  prepareForValue();
  writeChar('{');
  if (depth_ < MAX_DEPTH) stack_[depth_++] = {true, true};
}

void JsonWriter::endObject() {
  writeChar('}');
  if (depth_ > 0) --depth_;
}

void JsonWriter::beginArray() {
  prepareForValue();
  writeChar('[');
  if (depth_ < MAX_DEPTH) stack_[depth_++] = {false, true};
}

void JsonWriter::endArray() {
  writeChar(']');
  if (depth_ > 0) --depth_;
}

void JsonWriter::key(std::string_view name) {
  // A key is itself a "child" of the enclosing object: it needs the same
  // comma handling as a sibling value would.
  if (depth_ > 0) {
    Frame& top = stack_[depth_ - 1];
    if (!top.firstChild) {
      writeChar(',');
    } else {
      top.firstChild = false;
    }
  }
  writeStringRaw(name);
  writeChar(':');
  justEmittedKey_ = true;
}

void JsonWriter::valueString(std::string_view s) {
  prepareForValue();
  writeStringRaw(s);
}

void JsonWriter::valueBool(bool b) {
  prepareForValue();
  if (b)
    writeLiteral("true", 4);
  else
    writeLiteral("false", 5);
}

void JsonWriter::valueInt(int64_t n) {
  prepareForValue();
  char buf[24];
  int len = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
  if (len > 0) sink_.write(buf, static_cast<size_t>(len));
}

void JsonWriter::valueUInt(uint64_t n) {
  prepareForValue();
  char buf[24];
  int len = snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(n));
  if (len > 0) sink_.write(buf, static_cast<size_t>(len));
}

void JsonWriter::valueNull() {
  prepareForValue();
  writeLiteral("null", 4);
}

void JsonWriter::prepareForValue() {
  // A value immediately after a key consumes that key's slot -- no comma,
  // and the key already accounted for the firstChild flip on the parent.
  if (justEmittedKey_) {
    justEmittedKey_ = false;
    return;
  }
  if (depth_ == 0) return;  // top-level value -- nothing to bracket against
  Frame& top = stack_[depth_ - 1];
  if (!top.firstChild) {
    writeChar(',');
  } else {
    top.firstChild = false;
  }
}

void JsonWriter::writeStringRaw(std::string_view s) {
  writeChar('"');
  for (char c : s) {
    unsigned char uc = static_cast<unsigned char>(c);
    switch (uc) {
      case '"':
        writeLiteral("\\\"", 2);
        break;
      case '\\':
        writeLiteral("\\\\", 2);
        break;
      case '\b':
        writeLiteral("\\b", 2);
        break;
      case '\f':
        writeLiteral("\\f", 2);
        break;
      case '\n':
        writeLiteral("\\n", 2);
        break;
      case '\r':
        writeLiteral("\\r", 2);
        break;
      case '\t':
        writeLiteral("\\t", 2);
        break;
      default:
        if (uc < 0x20) {
          // Other C0 controls require \u00XX per RFC 8259.
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04X", uc);
          sink_.write(buf, 6);
        } else {
          // ASCII printable or UTF-8 continuation/lead byte: pass through.
          writeChar(c);
        }
        break;
    }
  }
  writeChar('"');
}
