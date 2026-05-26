#include "HalPlatform.h"

#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

HalPlatform halPlatform;  // Singleton instance

uint32_t HalPlatform::millis() const { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
uint32_t HalPlatform::micros() const { return static_cast<uint32_t>(esp_timer_get_time()); }

void HalPlatform::delay(uint32_t ms) const { vTaskDelay(pdMS_TO_TICKS(ms)); }

uint32_t HalPlatform::freeHeap() const { return esp_get_free_heap_size(); }
uint32_t HalPlatform::minFreeHeap() const { return esp_get_minimum_free_heap_size(); }
uint32_t HalPlatform::largestFreeBlock() const { return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT); }
uint32_t HalPlatform::totalHeap() const { return heap_caps_get_total_size(MALLOC_CAP_DEFAULT); }

void HalPlatform::hardRestart() { esp_restart(); }

uint32_t HalPlatform::randomU32() { return esp_random(); }

uint64_t HalPlatform::deviceId() const {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  return (static_cast<uint64_t>(mac[0]) << 40) | (static_cast<uint64_t>(mac[1]) << 32) |
         (static_cast<uint64_t>(mac[2]) << 24) | (static_cast<uint64_t>(mac[3]) << 16) |
         (static_cast<uint64_t>(mac[4]) << 8) | static_cast<uint64_t>(mac[5]);
}
