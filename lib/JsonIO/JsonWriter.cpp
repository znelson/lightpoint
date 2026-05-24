#include "JsonWriter.h"

#include <cstdio>

JsonWriter::JsonWriter(std::string& out) : out_(out), depth_(0), justEmittedKey_(false) {}

void JsonWriter::beginObject() {
  prepareForValue();
  out_.push_back('{');
  if (depth_ < MAX_DEPTH) stack_[depth_++] = {true, true};
}

void JsonWriter::endObject() {
  out_.push_back('}');
  if (depth_ > 0) --depth_;
}

void JsonWriter::beginArray() {
  prepareForValue();
  out_.push_back('[');
  if (depth_ < MAX_DEPTH) stack_[depth_++] = {false, true};
}

void JsonWriter::endArray() {
  out_.push_back(']');
  if (depth_ > 0) --depth_;
}

void JsonWriter::key(std::string_view name) {
  // A key is itself a "child" of the enclosing object: it needs the same
  // comma handling as a sibling value would.
  if (depth_ > 0) {
    Frame& top = stack_[depth_ - 1];
    if (!top.firstChild) {
      out_.push_back(',');
    } else {
      top.firstChild = false;
    }
  }
  writeStringRaw(name);
  out_.push_back(':');
  justEmittedKey_ = true;
}

void JsonWriter::valueString(std::string_view s) {
  prepareForValue();
  writeStringRaw(s);
}

void JsonWriter::valueBool(bool b) {
  prepareForValue();
  out_.append(b ? "true" : "false");
}

void JsonWriter::valueInt(int64_t n) {
  prepareForValue();
  char buf[24];
  int len = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
  if (len > 0) out_.append(buf, static_cast<size_t>(len));
}

void JsonWriter::valueUInt(uint64_t n) {
  prepareForValue();
  char buf[24];
  int len = snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(n));
  if (len > 0) out_.append(buf, static_cast<size_t>(len));
}

void JsonWriter::valueNull() {
  prepareForValue();
  out_.append("null");
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
    out_.push_back(',');
  } else {
    top.firstChild = false;
  }
}

void JsonWriter::writeStringRaw(std::string_view s) {
  out_.push_back('"');
  for (char c : s) {
    unsigned char uc = static_cast<unsigned char>(c);
    switch (uc) {
      case '"':
        out_.append("\\\"");
        break;
      case '\\':
        out_.append("\\\\");
        break;
      case '\b':
        out_.append("\\b");
        break;
      case '\f':
        out_.append("\\f");
        break;
      case '\n':
        out_.append("\\n");
        break;
      case '\r':
        out_.append("\\r");
        break;
      case '\t':
        out_.append("\\t");
        break;
      default:
        if (uc < 0x20) {
          // Other C0 controls require \u00XX per RFC 8259.
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04X", uc);
          out_.append(buf, 6);
        } else {
          // ASCII printable or UTF-8 continuation/lead byte: pass through.
          out_.push_back(c);
        }
        break;
    }
  }
  out_.push_back('"');
}
