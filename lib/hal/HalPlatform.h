#pragma once

#include <cstdint>

class HalPlatform {
 public:
  // Uptime since boot
  uint32_t millis() const;
  uint32_t micros() const;

  // Block the current task for `ms` milliseconds.
  void delay(uint32_t ms) const;

  // Heap introspection
  uint32_t freeHeap() const;
  uint32_t minFreeHeap() const;
  uint32_t largestFreeBlock() const;
  uint32_t totalHeap() const;

  // Hard reset — no teardown, equivalent to esp_restart() on ESP32
  void hardRestart();

  // Platform RNG
  uint32_t randomU32();

  // Stable unique device identifier (lower 48 bits, backed by efuse MAC on ESP32)
  uint64_t deviceId() const;
};

extern HalPlatform halPlatform;  // Singleton
