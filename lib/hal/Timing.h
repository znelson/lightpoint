#pragma once

#include <esp_timer.h>

#include <cstdint>

inline uint32_t uptime_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
inline uint32_t uptime_us() { return static_cast<uint32_t>(esp_timer_get_time()); }
