#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Streaming JSON serializer that builds a document into a caller-owned
// std::string. Tracks object/array nesting on a fixed-size stack (no heap
// allocations beyond the caller's string buffer) and inserts commas
// automatically between siblings.
//
// Usage:
//   std::string out;
//   JsonWriter w(out);
//   w.beginObject();
//   w.key("name"); w.valueString("Ada");
//   w.key("scores"); w.beginArray();
//     w.valueInt(10);
//     w.valueInt(20);
//   w.endArray();
//   w.endObject();
//   // out is now: {"name":"Ada","scores":[10,20]}
class JsonWriter {
 public:
  static constexpr size_t MAX_DEPTH = 8;

  explicit JsonWriter(std::string& out);

  JsonWriter(const JsonWriter&) = delete;
  JsonWriter& operator=(const JsonWriter&) = delete;

  void beginObject();
  void endObject();
  void beginArray();
  void endArray();

  // Emit an object member name. Must be followed by exactly one value
  // (valueString / valueInt / ... / beginObject / beginArray). Calling
  // key() when not directly inside an object is undefined.
  void key(std::string_view name);

  void valueString(std::string_view s);
  void valueBool(bool b);
  void valueInt(int64_t n);
  void valueUInt(uint64_t n);
  void valueNull();

 private:
  struct Frame {
    bool inObject;    // true for object, false for array
    bool firstChild;  // no leading comma needed before the next child
  };

  // Insert a comma if the current container already has a sibling, mark the
  // current slot consumed, and clear the "just emitted a key" suppression.
  void prepareForValue();
  void writeStringRaw(std::string_view s);

  std::string& out_;
  Frame stack_[MAX_DEPTH];
  size_t depth_;
  bool justEmittedKey_;
};
