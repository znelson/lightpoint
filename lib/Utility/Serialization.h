#pragma once
#include <HalStorage.h>

#include <iostream>
#include <limits>
#include <optional>
#include <type_traits>

namespace serialization {
template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
void readPod(HalFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

// std::optional<T> serialization for unsigned T. Uses the top of T's value
// range (std::numeric_limits<T>::max()) as the nullopt sentinel on disk;
// width is sizeof(T), identical to writing a plain T. Callers cannot
// represent the top value in memory -- in practice fine for index/count
// domains that don't approach the type's upper bound.
template <typename T>
  requires std::is_unsigned_v<T>
void writePod(std::ostream& os, const std::optional<T>& val) {
  const T v = val.value_or(std::numeric_limits<T>::max());
  writePod(os, v);
}

template <typename T>
  requires std::is_unsigned_v<T>
void writePod(HalFile& file, const std::optional<T>& val) {
  const T v = val.value_or(std::numeric_limits<T>::max());
  writePod(file, v);
}

template <typename T>
  requires std::is_unsigned_v<T>
void readPod(std::istream& is, std::optional<T>& out) {
  T v;
  readPod(is, v);
  out = (v == std::numeric_limits<T>::max()) ? std::nullopt : std::optional<T>(v);
}

template <typename T>
  requires std::is_unsigned_v<T>
void readPod(HalFile& file, std::optional<T>& out) {
  T v;
  readPod(file, v);
  out = (v == std::numeric_limits<T>::max()) ? std::nullopt : std::optional<T>(v);
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

inline void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  s.resize(len);
  is.read(&s[0], len);
}

inline void readString(HalFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  s.resize(len);
  file.read(&s[0], len);
}
}  // namespace serialization
