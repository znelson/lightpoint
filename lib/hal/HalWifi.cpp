#include "HalWifi.h"

#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

HalWifi halWifi;

bool HalWifi::init() {
  if (initialized_) return true;

  staNetif_ = esp_netif_create_default_wifi_sta();
  if (!staNetif_) {
    LOG_ERR("WIFI", "create sta netif failed");
    return false;
  }

  // Static RX buffers: pre-allocated as one contiguous block at init,
  // freed entirely in deinit() -- no heap fragmentation during WiFi operation.
  // 10 x ~1600 B = ~16 KB reserved up-front.
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.static_rx_buf_num = 10;
  cfg.dynamic_rx_buf_num = 0;
  if (esp_wifi_init(&cfg) != ESP_OK) {
    LOG_ERR("WIFI", "esp_wifi_init failed");
    esp_netif_destroy(staNetif_);
    staNetif_ = nullptr;
    return false;
  }

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler, this, &wifiEventHandle_);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ipEventHandler, this, &ipEventHandle_);

  // WIFI_STORAGE_RAM: credentials managed by WifiCredentialStore, not NVS.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();

  intentionalDisconnect_ = false;
  connectionState_ = ConnectionState::Idle;
  scanCount_ = SCAN_RUNNING;
  initialized_ = true;

  LOG_DBG("WIFI", "init -- free=%d largest=%d", esp_get_free_heap_size(),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  return true;
}

void HalWifi::deinit() {
  if (!initialized_) return;

  LOG_DBG("WIFI", "pre-deinit  free=%d largest=%d", esp_get_free_heap_size(),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

  intentionalDisconnect_ = true;
  esp_wifi_disconnect();
  esp_wifi_stop();

  if (wifiEventHandle_) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandle_);
    wifiEventHandle_ = nullptr;
  }
  if (ipEventHandle_) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ipEventHandle_);
    ipEventHandle_ = nullptr;
  }

  esp_wifi_deinit();

  if (staNetif_) {
    esp_netif_destroy(staNetif_);
    staNetif_ = nullptr;
  }

  initialized_ = false;
  intentionalDisconnect_ = false;
  connectionState_ = ConnectionState::Idle;
  scanCount_ = SCAN_RUNNING;

  LOG_DBG("WIFI", "post-deinit free=%d largest=%d", esp_get_free_heap_size(),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

void HalWifi::stop() {
  if (!initialized_) return;
  esp_wifi_disconnect();
  esp_wifi_stop();
}

bool HalWifi::getMacAddress(uint8_t mac[6]) const { return esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK; }

bool HalWifi::startScan() {
  // Disconnect before scanning; flag as intentional so the event handler
  // does not transition connectionState_ to Failed.
  intentionalDisconnect_ = true;
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(100));
  intentionalDisconnect_ = false;

  scanCount_ = SCAN_RUNNING;
  wifi_scan_config_t scanCfg = {};
  return esp_wifi_scan_start(&scanCfg, false) == ESP_OK;
}

void HalWifi::stopScan() { esp_wifi_scan_stop(); }

bool HalWifi::getScanResults(wifi_ap_record_t* records, uint16_t& count) {
  return esp_wifi_scan_get_ap_records(&count, records) == ESP_OK;
}

void HalWifi::setHostname(const char* hostname) {
  if (!staNetif_) {
    LOG_ERR("WIFI", "setHostname: netif not available");
    return;
  }
  esp_netif_set_hostname(staNetif_, hostname);
}

void HalWifi::connect(const char* ssid, const char* password) {
  // Disconnect any current session; flag as intentional so the pre-connect
  // disconnect event does not set connectionState_ to Failed.
  intentionalDisconnect_ = true;
  connectionState_ = ConnectionState::Idle;
  lastDisconnectReason_ = 0;
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(100));

  // Stale disconnect event has been flushed; arm the state machine.
  intentionalDisconnect_ = false;
  connectionState_ = ConnectionState::Connecting;

  wifi_config_t cfg = {};
  snprintf(reinterpret_cast<char*>(cfg.sta.ssid), sizeof(cfg.sta.ssid), "%s", ssid);
  if (password && *password) {
    snprintf(reinterpret_cast<char*>(cfg.sta.password), sizeof(cfg.sta.password), "%s", password);
  }
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
  esp_wifi_connect();
}

void HalWifi::disconnect() {
  intentionalDisconnect_ = true;
  esp_wifi_disconnect();
}

bool HalWifi::getIpAddress(char* buf, size_t len) const {
  if (!staNetif_) return false;
  esp_netif_ip_info_t ipInfo = {};
  if (esp_netif_get_ip_info(staNetif_, &ipInfo) != ESP_OK) return false;
  snprintf(buf, len, "%d.%d.%d.%d", static_cast<int>((ipInfo.ip.addr >> 0) & 0xFF),
           static_cast<int>((ipInfo.ip.addr >> 8) & 0xFF), static_cast<int>((ipInfo.ip.addr >> 16) & 0xFF),
           static_cast<int>((ipInfo.ip.addr >> 24) & 0xFF));
  return true;
}

void HalWifi::setPowerSave(bool enable) { esp_wifi_set_ps(enable ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE); }

bool HalWifi::isConnected() const {
  wifi_ap_record_t apInfo;
  return esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK;
}

void HalWifi::wifiEventHandler(void* arg, esp_event_base_t, int32_t id, void* data) {
  auto* self = static_cast<HalWifi*>(arg);
  if (id == WIFI_EVENT_SCAN_DONE) {
    const auto* evt = static_cast<wifi_event_sta_scan_done_t*>(data);
    self->scanCount_ = (evt->status == 0) ? static_cast<int16_t>(evt->number) : SCAN_FAILED;
  } else if (id == WIFI_EVENT_STA_DISCONNECTED && !self->intentionalDisconnect_) {
    const auto* evt = static_cast<wifi_event_sta_disconnected_t*>(data);
    LOG_DBG("WIFI", "disconnected, reason: %d", evt->reason);
    self->lastDisconnectReason_ = evt->reason;
    if (self->connectionState_ == ConnectionState::Connecting || self->connectionState_ == ConnectionState::Connected) {
      self->connectionState_ = ConnectionState::Failed;
    }
  }
}

void HalWifi::ipEventHandler(void* arg, esp_event_base_t, int32_t id, void*) {
  if (id == IP_EVENT_STA_GOT_IP) {
    static_cast<HalWifi*>(arg)->connectionState_ = ConnectionState::Connected;
  }
}
