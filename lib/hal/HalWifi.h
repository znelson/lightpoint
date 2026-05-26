#pragma once

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include <cstddef>
#include <cstdint>

class HalWifi {
 public:
  static constexpr int16_t SCAN_RUNNING = -1;
  static constexpr int16_t SCAN_FAILED = -2;

  enum class ConnectionState { Idle, Connecting, Connected, Failed };

  // Initialize WiFi driver (idempotent). Returns false on fatal error.
  bool init();

  // Full teardown: stops driver, deinits, destroys netif. Resets initialized state
  // so init() can be called again. Use after a WiFi session to reclaim heap.
  void deinit();

  // Disconnect and stop the WiFi driver without full deinit. Use before deep sleep.
  void stop();

  // Returns true if the WiFi driver has been initialized and not yet deinit'd.
  bool isActive() const { return initialized_; }

  // Copy the STA MAC address into mac[6]. Returns true on success.
  bool getMacAddress(uint8_t mac[6]) const;

  // Start an async network scan. Resets scan state and disconnects first.
  bool startScan();

  // Stop any in-progress scan.
  void stopScan();

  // SCAN_RUNNING while in progress; SCAN_FAILED on error; AP count on completion.
  int16_t scanStatus() const { return scanCount_; }

  // Copy scan results into caller-provided buffer. count is in/out. Returns true on success.
  bool getScanResults(wifi_ap_record_t* records, uint16_t& count);

  // Set the STA interface hostname.
  void setHostname(const char* hostname);

  // Begin a connection attempt. password may be nullptr for open networks.
  void connect(const char* ssid, const char* password);

  // Disconnect from the current AP (intentional -- does not trigger Failed state).
  void disconnect();

  ConnectionState getConnectionState() const { return connectionState_; }
  uint8_t getLastDisconnectReason() const { return lastDisconnectReason_; }

  // Write the current IP as "a.b.c.d" into buf. Returns true on success.
  bool getIpAddress(char* buf, size_t len) const;

  // Enable or disable WiFi modem power saving.
  void setPowerSave(bool enable);

  // Returns true if currently associated to an AP.
  bool isConnected() const;

 private:
  bool initialized_ = false;
  esp_netif_t* staNetif_ = nullptr;
  esp_event_handler_instance_t wifiEventHandle_ = nullptr;
  esp_event_handler_instance_t ipEventHandle_ = nullptr;

  volatile ConnectionState connectionState_ = ConnectionState::Idle;
  volatile int16_t scanCount_ = SCAN_RUNNING;
  volatile bool intentionalDisconnect_ = false;
  volatile uint8_t lastDisconnectReason_ = 0;

  static void wifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
  static void ipEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
};

extern HalWifi halWifi;  // Singleton
