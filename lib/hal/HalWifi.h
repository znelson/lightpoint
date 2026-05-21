#pragma once

#include <cstddef>
#include <cstdint>

class HalWifi {
  bool initialized_ = false;

 public:
  // Initialize WiFi driver (idempotent). Sets STA mode and starts the driver.
  void init();

  // Returns true if the WiFi driver is active (mode is not WIFI_MODE_NULL).
  bool isActive() const;

  // Disconnect and stop the WiFi driver. No-op if not active.
  void stop();

  // Copy the STA MAC address into mac[6]. Returns true on success.
  bool getMacAddress(uint8_t mac[6]) const;

  // Returns true if currently associated to an AP.
  bool isConnected() const;

  // Write the current IP as "a.b.c.d" into buf. Returns true on success.
  bool getIpAddress(char* buf, size_t len) const;

  // Set the STA interface hostname.
  void setHostname(const char* hostname);

  // Enable or disable WiFi modem power saving.
  void setPowerSave(bool enable);
};

extern HalWifi halWifi;
