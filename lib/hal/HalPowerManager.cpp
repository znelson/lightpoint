#include "HalPowerManager.h"

#include <Logging.h>
#include <Timing.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_sleep.h>
#include <soc/rtc.h>

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

void HalPowerManager::begin() {
  if (gpio.deviceIsX3()) {
    // I2C bus is already initialised by gpio.begin() for X3.
    // Create a device handle for the BQ27220 fuel gauge (battery SOC monitoring).
    i2c_device_config_t devCfg = {};
    devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    devCfg.device_address = I2C_ADDR_BQ27220;
    devCfg.scl_speed_hz = X3_I2C_FREQ;
    if (i2c_master_bus_add_device(gpio.getI2CBus(), &devCfg, &bq27220Dev) != ESP_OK) {
      LOG_ERR("PWR", "Failed to add BQ27220 I2C device");
      bq27220Dev = nullptr;
    }
    _batteryUseI2C = (bq27220Dev != nullptr);
  } else {
    gpio_set_direction((gpio_num_t)BAT_GPIO0, GPIO_MODE_INPUT);
  }
  rtc_cpu_freq_config_t freqConf;
  rtc_clk_cpu_freq_get_config(&freqConf);
  normalFreq = static_cast<int>(freqConf.freq_mhz);
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // Note: We don't use mutex here to avoid too much overhead,
  // it's not very important if we read a slightly stale value for currentLockMode
  const LockMode mode = currentLockMode;

  auto setCpuFreqMhz = [](int mhz) -> bool {
    rtc_cpu_freq_config_t conf;
    if (!rtc_clk_cpu_freq_mhz_to_config(static_cast<uint32_t>(mhz), &conf)) {
      return false;
    }
    rtc_clk_cpu_freq_set_config(&conf);
    return true;
  };

  if (mode == None && enabled && !isLowPower) {
    LOG_DBG("PWR", "Going to low-power mode");
    if (!setCpuFreqMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      return;
    }
    isLowPower = true;

  } else if ((!enabled || mode != None) && isLowPower) {
    LOG_DBG("PWR", "Restoring normal CPU frequency");
    if (!setCpuFreqMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      return;
    }
    isLowPower = false;
  }

  // Otherwise, no change needed
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio.update();
  }
  // Pre-sleep routines from the original firmware
  // GPIO13 is connected to battery latch MOSFET, we need to make sure it's low during sleep
  // Note that this means the MCU will be completely powered off during sleep, including RTC
  constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
  gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SPIWP, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_SPIWP);
  gpio_set_direction((gpio_num_t)InputManager::POWER_BUTTON_PIN, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)InputManager::POWER_BUTTON_PIN, GPIO_PULLUP_ONLY);
  // Arm the wakeup trigger *after* the button is released
  // Note: this is only useful for waking up on USB power. On battery, the MCU will be completely powered off, so the
  // power button is hard-wired to briefly provide power to the MCU, waking it up regardless of the wakeup source
  // configuration
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  if (_batteryUseI2C) {
    const uint32_t now = uptime_ms();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    if (!bq27220Dev) {
      return _batteryCachedPercent;
    }

    // Read SOC from BQ27220 fuel gauge (16-bit LE register).
    // On I2C error, keep last known value to avoid UI jitter/slowdowns.
    const uint8_t reg = BQ27220_SOC_REG;
    uint8_t data[2];
    if (i2c_master_transmit_receive(bq27220Dev, &reg, 1, data, 2, 4) != ESP_OK) {
      _batteryLastPollMs = now;
      return _batteryCachedPercent;
    }
    const uint16_t soc = (static_cast<uint16_t>(data[1]) << 8) | data[0];
    _batteryCachedPercent = soc > 100 ? 100 : soc;
    _batteryLastPollMs = now;
    return _batteryCachedPercent;
  }
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  // Current limitation: only one lock at a time
  if (powerManager.currentLockMode != None) {
    LOG_ERR("PWR", "Lock already held, ignore");
    valid = false;
  } else {
    powerManager.currentLockMode = NormalSpeed;
    valid = true;
  }
  xSemaphoreGive(powerManager.modeMutex);
  if (valid) {
    // Immediately restore normal CPU frequency if currently in low-power mode
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  if (valid) {
    powerManager.currentLockMode = None;
  }
  xSemaphoreGive(powerManager.modeMutex);
}
