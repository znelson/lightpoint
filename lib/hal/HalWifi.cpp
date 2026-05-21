#include "HalWifi.h"

#include <Logging.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>

HalWifi halWifi;

void HalWifi::init() {
  if (initialized_) return;
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  initialized_ = true;
}

bool HalWifi::isActive() const {
  wifi_mode_t mode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&mode);
  return mode != WIFI_MODE_NULL;
}

void HalWifi::stop() {
  if (!isActive()) return;
  esp_wifi_disconnect();
  esp_wifi_stop();
}

bool HalWifi::getMacAddress(uint8_t mac[6]) const { return esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK; }

bool HalWifi::isConnected() const {
  wifi_ap_record_t apInfo;
  return esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK;
}

bool HalWifi::getIpAddress(char* buf, size_t len) const {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) return false;
  esp_netif_ip_info_t ipInfo = {};
  if (esp_netif_get_ip_info(netif, &ipInfo) != ESP_OK) return false;
  snprintf(buf, len, "%d.%d.%d.%d", static_cast<int>((ipInfo.ip.addr >> 0) & 0xFF),
           static_cast<int>((ipInfo.ip.addr >> 8) & 0xFF), static_cast<int>((ipInfo.ip.addr >> 16) & 0xFF),
           static_cast<int>((ipInfo.ip.addr >> 24) & 0xFF));
  return true;
}

void HalWifi::setHostname(const char* hostname) {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) {
    LOG_ERR("WIFI", "setHostname: netif not found");
    return;
  }
  esp_netif_set_hostname(netif, hostname);
}

void HalWifi::setPowerSave(bool enable) { esp_wifi_set_ps(enable ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE); }
