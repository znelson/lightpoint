#include <HalGPIO.h>
#include <Logging.h>
#include <Timing.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

struct X3ProbeResult {
  bool bq27220 = false;
  bool ds3231 = false;
  bool qmi8658 = false;

  uint8_t score() const {
    return static_cast<uint8_t>(bq27220) + static_cast<uint8_t>(ds3231) + static_cast<uint8_t>(qmi8658);
  }
};

static bool readI2CReg8(i2c_master_bus_handle_t bus, uint8_t addr, uint8_t reg, uint8_t* out) {
  i2c_device_config_t devCfg = {};
  devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  devCfg.device_address = addr;
  devCfg.scl_speed_hz = X3_I2C_FREQ;
  i2c_master_dev_handle_t dev;
  if (i2c_master_bus_add_device(bus, &devCfg, &dev) != ESP_OK) return false;
  const esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, out, 1, 6);
  i2c_master_bus_rm_device(dev);
  return err == ESP_OK;
}

static bool probeBQ27220Signature(i2c_master_bus_handle_t bus) {
  i2c_device_config_t devCfg = {};
  devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  devCfg.device_address = I2C_ADDR_BQ27220;
  devCfg.scl_speed_hz = X3_I2C_FREQ;
  i2c_master_dev_handle_t dev;
  if (i2c_master_bus_add_device(bus, &devCfg, &dev) != ESP_OK) return false;

  bool ok = false;
  do {
    uint8_t reg = BQ27220_SOC_REG;
    uint8_t data[2];
    if (i2c_master_transmit_receive(dev, &reg, 1, data, 2, 6) != ESP_OK) break;
    const uint16_t soc = (static_cast<uint16_t>(data[1]) << 8) | data[0];
    if (soc > 100) break;
    reg = BQ27220_VOLT_REG;
    if (i2c_master_transmit_receive(dev, &reg, 1, data, 2, 6) != ESP_OK) break;
    const uint16_t voltageMv = (static_cast<uint16_t>(data[1]) << 8) | data[0];
    ok = (voltageMv >= 2500 && voltageMv <= 5000);
  } while (false);

  i2c_master_bus_rm_device(dev);
  return ok;
}

static bool probeDS3231Signature(i2c_master_bus_handle_t bus) {
  uint8_t sec = 0;
  if (!readI2CReg8(bus, I2C_ADDR_DS3231, DS3231_SEC_REG, &sec)) return false;
  const uint8_t tensDigit = (sec >> 4) & 0x07;
  const uint8_t onesDigit = sec & 0x0F;
  return tensDigit <= 5 && onesDigit <= 9;
}

static bool probeQMI8658Signature(i2c_master_bus_handle_t bus) {
  uint8_t whoami = 0;
  if (readI2CReg8(bus, I2C_ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  if (readI2CReg8(bus, I2C_ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE) {
    return true;
  }
  return false;
}

X3ProbeResult runX3ProbePass() {
  X3ProbeResult result;

  i2c_master_bus_config_t busCfg = {};
  busCfg.i2c_port = I2C_NUM_0;
  busCfg.sda_io_num = (gpio_num_t)X3_I2C_SDA;
  busCfg.scl_io_num = (gpio_num_t)X3_I2C_SCL;
  busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
  busCfg.glitch_ignore_cnt = 7;
  busCfg.flags.enable_internal_pullup = true;

  i2c_master_bus_handle_t bus;
  if (i2c_new_master_bus(&busCfg, &bus) != ESP_OK) {
    return result;
  }

  result.bq27220 = probeBQ27220Signature(bus);
  result.ds3231 = probeDS3231Signature(bus);
  result.qmi8658 = probeQMI8658Signature(bus);

  i2c_del_master_bus(bus);
  // Reset SDA/SCL pins to floating inputs in case this is an X4 device
  // (these pins serve as UART0_RXD=20 and BAT_GPIO0=0 on X4)
  gpio_set_direction(GPIO_NUM_20, GPIO_MODE_INPUT);
  gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
  return result;
}

}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  nvs_handle_t handle;
  if (nvs_open(HW_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    return defaultValue;
  }
  uint8_t raw = static_cast<uint8_t>(defaultValue);
  nvs_get_u8(handle, key, &raw);
  nvs_close(handle);
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  nvs_handle_t handle;
  if (nvs_open(HW_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    return;
  }
  nvs_set_u8(handle, key, static_cast<uint8_t>(value));
  nvs_commit(handle);
  nvs_close(handle);
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: run active X3 fingerprint probe and persist result.
  const X3GPIO::X3ProbeResult pass1 = X3GPIO::runX3ProbePass();
  vTaskDelay(pdMS_TO_TICKS(2));
  const X3GPIO::X3ProbeResult pass2 = X3GPIO::runX3ProbePass();

  const uint8_t score1 = pass1.score();
  const uint8_t score2 = pass2.score();
  LOG_INF("HW", "X3 probe scores: pass1=%u(bq=%d rtc=%d imu=%d) pass2=%u(bq=%d rtc=%d imu=%d)", score1, pass1.bq27220,
          pass1.ds3231, pass1.qmi8658, score2, pass2.bq27220, pass2.ds3231, pass2.qmi8658);
  const bool x3Confirmed = (score1 >= 2) && (score2 >= 2);
  const bool x4Confirmed = (score1 == 0) && (score2 == 0);

  if (x3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (x4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

}  // namespace

bool HalGPIO::initI2C() {
  if (i2cBus) return true;
  i2c_master_bus_config_t busCfg = {};
  busCfg.i2c_port = I2C_NUM_0;
  busCfg.sda_io_num = (gpio_num_t)X3_I2C_SDA;
  busCfg.scl_io_num = (gpio_num_t)X3_I2C_SCL;
  busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
  busCfg.glitch_ignore_cnt = 7;
  busCfg.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&busCfg, &i2cBus) != ESP_OK) {
    LOG_ERR("GPIO", "I2C bus init failed");
    return false;
  }
  return true;
}

void HalGPIO::begin() {
  inputMgr.begin();

  // Register SPI2 with IDF's device management. EInkDisplay::begin() calls
  // spi_bus_add_device() which requires the bus to be registered first.
  spi_bus_config_t spiBusCfg = {};
  spiBusCfg.mosi_io_num = EPD_MOSI;
  spiBusCfg.miso_io_num = SPI_MISO;
  spiBusCfg.sclk_io_num = EPD_SCLK;
  spiBusCfg.quadwp_io_num = -1;
  spiBusCfg.quadhd_io_num = -1;
  spiBusCfg.max_transfer_sz = 52272;  // EInkDisplay::MAX_BUFFER_SIZE
  spi_bus_initialize(SPI2_HOST, &spiBusCfg, SPI_DMA_CH_AUTO);

  _deviceType = detectDeviceTypeWithFingerprint();

  if (deviceIsX4()) {
    gpio_set_direction((gpio_num_t)BAT_GPIO0, GPIO_MODE_INPUT);
    gpio_set_direction((gpio_num_t)UART0_RXD, GPIO_MODE_INPUT);
  }

  if (deviceIsX3()) {
    if (initI2C()) {
      i2c_device_config_t devCfg = {};
      devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
      devCfg.device_address = I2C_ADDR_BQ27220;
      devCfg.scl_speed_hz = X3_I2C_FREQ;
      if (i2c_master_bus_add_device(i2cBus, &devCfg, &bq27220Dev) != ESP_OK) {
        LOG_ERR("GPIO", "Failed to add BQ27220 I2C device");
        bq27220Dev = nullptr;
      }
    }
  }
}

void HalGPIO::update() {
  inputMgr.update();
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

uint32_t HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

uint32_t HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

adc_oneshot_unit_handle_t HalGPIO::getAdcUnit() const { return inputMgr.getAdcUnit(); }

void HalGPIO::startDeepSleep() {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (inputMgr.isPressed(BTN_POWER)) {
    vTaskDelay(pdMS_TO_TICKS(50));
    inputMgr.update();
  }
  // Arm the wakeup trigger *after* the button is released
  esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

void HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  if (shortPressAllowed) {
    // Fast path - no duration check needed
    return;
  }
  // TODO: Intermittent edge case remains: a single tap followed by another single tap
  // can still power on the device. Tighten wake debounce/state handling here.

  // Calibrate: subtract boot time already elapsed, assuming button held since boot
  const uint32_t calibration = uptime_ms();
  const uint32_t calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;

  const auto start = uptime_ms();
  inputMgr.update();
  // inputMgr.isPressed() may take up to ~500ms to return correct state
  while (!inputMgr.isPressed(BTN_POWER) && uptime_ms() - start < 1000) {
    vTaskDelay(pdMS_TO_TICKS(10));
    inputMgr.update();
  }
  if (inputMgr.isPressed(BTN_POWER)) {
    do {
      vTaskDelay(pdMS_TO_TICKS(10));
      inputMgr.update();
    } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getPowerButtonHeldTime() < calibratedDuration);
    if (inputMgr.getPowerButtonHeldTime() < calibratedDuration) {
      startDeepSleep();
    }
  } else {
    startDeepSleep();
  }
}

bool HalGPIO::isUsbConnected() const {
  if (deviceIsX3()) {
    // X3: infer USB/charging via BQ27220 Current() register (0x0C, signed mA).
    // Positive current means charging (USB connected).
    if (!bq27220Dev) return false;
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      const uint8_t reg = BQ27220_CUR_REG;
      uint8_t data[2];
      if (i2c_master_transmit_receive(bq27220Dev, &reg, 1, data, 2, 4) == ESP_OK) {
        const int16_t currentMa = static_cast<int16_t>((static_cast<uint16_t>(data[1]) << 8) | data[0]);
        return currentMa > 0;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
  }
  // U0RXD/GPIO20 reads HIGH when USB is connected
  return gpio_get_level((gpio_num_t)UART0_RXD) == 1;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const uint32_t wakeupCauses = esp_sleep_get_wakeup_causes();
  const auto resetReason = esp_reset_reason();

  const bool usbConnected = isUsbConnected();
  const bool noWakeupCause = (wakeupCauses == 0);
  const bool gpioWakeup = (wakeupCauses & (1U << ESP_SLEEP_WAKEUP_GPIO)) != 0;

  if ((noWakeupCause && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (gpioWakeup && resetReason == ESP_RST_DEEPSLEEP && usbConnected)) {
    return WakeupReason::PowerButton;
  }
  if (noWakeupCause && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (noWakeupCause && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
