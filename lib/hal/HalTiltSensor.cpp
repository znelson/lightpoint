#include "HalTiltSensor.h"

#include <HalPlatform.h>
#include <Logging.h>
#include <driver/i2c_master.h>

#include <cmath>

HalTiltSensor halTiltSensor;  // Singleton instance

bool HalTiltSensor::writeReg(uint8_t reg, uint8_t val) const {
  const uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(i2cDev, buf, 2, 10) == ESP_OK;
}

bool HalTiltSensor::readReg(uint8_t reg, uint8_t* val) const {
  return i2c_master_transmit_receive(i2cDev, &reg, 1, val, 1, 10) == ESP_OK;
}

bool HalTiltSensor::readGyro(float& gx, float& gy, float& gz) const {
  const uint8_t startReg = REG_GX_L;
  uint8_t data[6];
  if (i2c_master_transmit_receive(i2cDev, &startReg, 1, data, 6, 10) != ESP_OK) {
    return false;
  }

  // If Full Scale is +-512 dps, the scale factor is 32768 / 512 = 64 LSB/dps
  constexpr float SCALE = 1.0f / 64.0f;
  auto readInt16 = [&](size_t i) -> int16_t {
    return static_cast<int16_t>((static_cast<uint16_t>(data[i + 1]) << 8) | data[i]);
  };
  gx = readInt16(0) * SCALE;
  gy = readInt16(2) * SCALE;
  gz = readInt16(4) * SCALE;
  return true;
}

void HalTiltSensor::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  auto tryAddr = [&](uint8_t addr) -> bool {
    if (i2cDev) {
      i2c_master_bus_rm_device(i2cDev);
      i2cDev = nullptr;
    }
    i2c_device_config_t devCfg = {};
    devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    devCfg.device_address = addr;
    devCfg.scl_speed_hz = X3_I2C_FREQ;
    if (i2c_master_bus_add_device(gpio.getI2CBus(), &devCfg, &i2cDev) != ESP_OK) return false;
    uint8_t whoami = 0;
    return readReg(QMI8658_WHO_AM_I_REG, &whoami) && whoami == QMI8658_WHO_AM_I_VALUE;
  };

  if (tryAddr(I2C_ADDR_QMI8658)) {
    _i2cAddr = I2C_ADDR_QMI8658;
  } else if (tryAddr(I2C_ADDR_QMI8658_ALT)) {
    _i2cAddr = I2C_ADDR_QMI8658_ALT;
  } else {
    if (i2cDev) {
      i2c_master_bus_rm_device(i2cDev);
      i2cDev = nullptr;
    }
    LOG_ERR("GYR", "QMI8658 IMU not found");
    _available = false;
    return;
  }

  LOG_INF("GYR", "QMI8658 IMU found at 0x%02X", _i2cAddr);

  if (!writeReg(REG_CTRL7, CTRL7_DISABLE_ALL) || !writeReg(REG_CTRL3, CTRL3_FS_512DPS | CTRL3_ODR_28HZ) ||
      !writeReg(REG_CTRL1, CTRL1_BASE | CTRL1_SENSOR_DISABLE)) {
    LOG_ERR("GYR", "QMI8658 register configuration failed");
    _available = false;
    return;
  }

  _available = true;
  _initMs = halPlatform.millis();
  _lastPollMs = halPlatform.millis();
  LOG_INF("GYR", "QMI8658 gyro initialized and put to sleep");
}

bool HalTiltSensor::wake() {
  if (!_available) {
    return false;
  }

  // Wait for init to complete before waking
  if ((halPlatform.millis() - _initMs) < SLEEP_STABILIZE_MS) {
    return false;
  }

  if (writeReg(REG_CTRL1, CTRL1_BASE) && writeReg(REG_CTRL7, CTRL7_GYRO_ENABLE)) {
    _lastPollMs = halPlatform.millis();
    _lastTiltMs = halPlatform.millis();
    _wakeMs = halPlatform.millis();
    LOG_INF("GYR", "QMI8658 woke up");
    return true;
  } else {
    LOG_ERR("GYR", "Failed to wake QMI8658");
    return false;
  }
}

bool HalTiltSensor::deepSleep() {
  if (!_available) {
    return false;
  }

  if ((halPlatform.millis() - _wakeMs) < SLEEP_STABILIZE_MS) {
    return false;
  }

  if (writeReg(REG_CTRL7, CTRL7_DISABLE_ALL) && writeReg(REG_CTRL1, CTRL1_BASE | CTRL1_SENSOR_DISABLE)) {
    // Clear any residual state so it doesn't immediately trigger upon waking
    clearPendingEvents();
    _inTilt = false;
    LOG_INF("GYR", "QMI8658 entered sleep mode");
    return true;
  } else {
    LOG_ERR("GYR", "Failed to put QMI8658 to sleep");
    return false;
  }
}

void HalTiltSensor::update(const uint8_t mode, const uint8_t orientation, const bool inReader) {
  if (!_available) {
    return;
  }

  // State machine: wake up or sleep based on the enabled flag
  if ((mode != CrossPointTiltPageTurn::TILT_OFF) && !_isAwake) {
    _isAwake = wake();
    return;
  } else if ((mode == CrossPointTiltPageTurn::TILT_OFF) && _isAwake) {
    _isAwake = !deepSleep();
    return;
  }

  // If disabled, skip the rest of the polling logic and avoid unnecessary I2C traffic in non-reader activities
  if ((mode == CrossPointTiltPageTurn::TILT_OFF) || !inReader) {
    return;
  }

  const uint32_t now = halPlatform.millis();
  // Stabilization: discard readings during gyro startup transient
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) {
    return;
  }

  if ((now - _lastPollMs) < POLL_INTERVAL_MS) {
    return;
  }
  _lastPollMs = now;

  float gx, gy, gz;
  if (!readGyro(gx, gy, gz)) {
    return;
  }

  // Map the gyro axis to left/right tilt based on reader orientation.
  // On the X3 PCB: X axis = left/right in portrait, Y axis = left/right in landscape.
  float tiltAxis;
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? -gx : gx;
      break;
    case CrossPointOrientation::INVERTED:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? gx : -gx;
      break;
    case CrossPointOrientation::LANDSCAPE_CW:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? gy : -gy;
      break;
    case CrossPointOrientation::LANDSCAPE_CCW:
      tiltAxis = mode == CrossPointTiltPageTurn::TILT_INVERTED ? -gy : gy;
      break;
    default:
      tiltAxis = gx;
      break;
  }

  if (_inTilt) {
    // Wait for device to return to neutral before allowing next trigger
    if (fabsf(tiltAxis) < NEUTRAL_RATE_DPS) {
      _inTilt = false;
    }
  } else {
    // Check for new tilt gesture (with cooldown)
    if ((now - _lastTiltMs) >= COOLDOWN_MS) {
      if (tiltAxis > RATE_THRESHOLD_DPS) {
        _tiltForwardEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Forward Trigger=(%.1f) dps", tiltAxis);
      } else if (tiltAxis < -RATE_THRESHOLD_DPS) {
        _tiltBackEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Backward Trigger=(%.1f) dps", tiltAxis);
      }
    }
  }
}

bool HalTiltSensor::wasTiltedForward() {
  const bool val = _tiltForwardEvent;
  _tiltForwardEvent = false;
  return val;
}

bool HalTiltSensor::wasTiltedBack() {
  const bool val = _tiltBackEvent;
  _tiltBackEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
  // Intentionally preserve _inTilt so a held tilt doesn't retrigger on next poll
}
