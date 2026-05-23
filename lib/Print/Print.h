#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Minimal Print interface replacing Arduino's Print.h.
// HalFile and MySerialImpl inherit from this; PngToBmpConverter and
// readFileToStream accept Print& as a write-only byte sink.
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t b) = 0;
  virtual size_t write(const uint8_t* buf, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < size; ++i) n += write(buf[i]);
    return n;
  }
  size_t write(const char* str) {
    if (!str) return 0;
    return write(reinterpret_cast<const uint8_t*>(str), strlen(str));
  }
  virtual void flush() {}
  size_t print(const char* str) { return write(str); }
};
