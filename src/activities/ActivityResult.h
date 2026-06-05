#pragma once

#include <Epub/Chapter.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

struct WifiResult {
  bool connected = false;
  std::string ssid;
  std::string ip;
};

struct KeyboardResult {
  std::string text;
};

struct MenuResult {
  std::optional<int> action;
  uint8_t orientation = 0;
};

struct PercentResult {
  int percent = 0;
};

struct IntervalResult {
  uint32_t value = 0;
};

struct PageResult {
  uint32_t page = 0;
};

struct PositionResult {
  uint16_t spineIndex = 0;
  uint16_t page = 0;
};

struct LinkResult {
  std::string href;
};

struct FilePathResult {
  std::string path;
};

using ResultVariant = std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterTarget, PercentResult,
                                   IntervalResult, PageResult, PositionResult, LinkResult, FilePathResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType>
    requires std::is_constructible_v<ResultVariant, ResultType&&>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
