#include "HalClock.h"

#include <HalPlatform.h>
#include <HalWifi.h>
#include <Logging.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

// DS3231 register layout (BCD encoded):
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

void HalClock::begin() {
  if (!halGPIO.deviceIsX3()) {
    _available = false;
    return;
  }

  // I2C bus is already initialised by halGPIO.begin() for X3.
  i2c_device_config_t devCfg = {};
  devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  devCfg.device_address = I2C_ADDR_DS3231;
  devCfg.scl_speed_hz = X3_I2C_FREQ;
  if (i2c_master_bus_add_device(halGPIO.getI2CBus(), &devCfg, &ds3231Dev) != ESP_OK) {
    LOG_ERR("CLK", "Failed to add DS3231 I2C device");
    _available = false;
    return;
  }

  // Probe: read 1 byte from seconds register to verify connectivity
  const uint8_t reg = DS3231_SEC_REG;
  uint8_t dummy;
  if (i2c_master_transmit_receive(ds3231Dev, &reg, 1, &dummy, 1, 10) != ESP_OK) {
    LOG_INF("CLK", "DS3231 RTC not found");
    i2c_master_bus_rm_device(ds3231Dev);
    ds3231Dev = nullptr;
    _available = false;
    return;
  }

  _available = true;
  LOG_INF("CLK", "DS3231 RTC found");

  // Prime the cache with an initial read
  uint8_t h, m;
  getTime(h, m);
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const uint32_t now = halPlatform.millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  // Read 3 bytes starting at register 0x00: seconds, minutes, hours
  const uint8_t reg = DS3231_SEC_REG;
  uint8_t data[3];
  if (i2c_master_transmit_receive(ds3231Dev, &reg, 1, data, 3, 10) != ESP_OK) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  const uint8_t rawMin = data[1];  // data[0] = seconds (unused)
  const uint8_t rawHour = data[2];

  _cachedMinute = bcdToDec(rawMin & 0x7F);
  // Handle 12/24h mode: bit 6 high = 12h mode
  if (rawHour & 0x40) {
    // 12h mode: bit 5 = PM, bits 4-0 = hours (1-12)
    uint8_t h12 = bcdToDec(rawHour & 0x1F);
    bool pm = rawHour & 0x20;
    if (h12 == 12) h12 = 0;
    _cachedHour = pm ? (h12 + 12) : h12;
  } else {
    // 24h mode: bits 5-0 = hours (0-23)
    _cachedHour = bcdToDec(rawHour & 0x3F);
  }
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second) {
  assert(hour < 24);
  assert(minute < 60);
  assert(second < 60);
  const uint8_t buf[4] = {DS3231_SEC_REG, decToBcd(second), decToBcd(minute), decToBcd(hour)};
  if (i2c_master_transmit(ds3231Dev, buf, 4, 10) != ESP_OK) {
    LOG_ERR("CLK", "Failed to write time to DS3231");
    return false;
  }

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = hour;
  _cachedMinute = minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (!halWifi.isConnected()) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  setenv("TZ", "UTC0", 1);
  tzset();
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_setservername(1, "time.nist.gov");
  if (!esp_sntp_enabled()) {
    esp_sntp_init();
  } else {
    esp_sntp_restart();
  }

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      if (writeTimeToRTC(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec)) {
        LOG_INF("CLK", "RTC set to %02d:%02d:%02d UTC", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        return true;
      }
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
