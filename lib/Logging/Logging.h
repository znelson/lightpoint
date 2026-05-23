#pragma once

#include <Print.h>

#include <string>

/*
Define ENABLE_SERIAL_LOG to enable logging
Can be set in platformio.ini build_flags or as a compile definition

Define LOG_LEVEL to control log verbosity:
0 = ERR only
1 = ERR + INF
2 = ERR + INF + DBG
If not defined, defaults to 0

For raw serial access (e.g., binary data, special formatting), use the
logSerial object directly:
    logSerial.write(binaryData, length);

logSerial is MySerialImpl::instance and can be used anywhere Serial is needed.
*/

#ifndef LOG_LEVEL
#define LOG_LEVEL 0
#endif

void logPrintf(const char* level, const char* origin, const char* format, ...);

#ifdef ENABLE_SERIAL_LOG
#if LOG_LEVEL >= 0
#define LOG_ERR(origin, format, ...) logPrintf("ERR", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_ERR(origin, format, ...)
#endif

#if LOG_LEVEL >= 1
#define LOG_INF(origin, format, ...) logPrintf("INF", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_INF(origin, format, ...)
#endif

#if LOG_LEVEL >= 2
#define LOG_DBG(origin, format, ...) logPrintf("DBG", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_DBG(origin, format, ...)
#endif
#else
#define LOG_DBG(origin, format, ...)
#define LOG_ERR(origin, format, ...)
#define LOG_INF(origin, format, ...)
#endif

std::string getLastLogs();
void clearLastLogs();

// Public so the esp_rom_printf hook can push CORRUPT HEAP messages from
// inside ESP-IDF into the crash-report ring buffer.
void addToLogRingBuffer(const char* message);

// Installs a putc on esp_rom_printf channel 2 that pushes complete lines into
// the crash-report ring buffer. Call once during setup. No-op in slim builds.
#ifdef ENABLE_SERIAL_LOG
void installRomPrintfHook();
#endif

// Validates the RTC log state (magic word + logHead range). Returns true if
// corruption was detected (magic mismatch or logHead out of range), meaning
// logMessages is untrusted garbage. Callers should call clearLastLogs() when
// this returns true so getLastLogs() does not dump corrupt data into crash reports.
bool sanitizeLogHead();

class MySerialImpl : public Print {
 public:
  void begin(unsigned long baud);
  operator bool() const;
  int available();
  size_t readBytesUntil(char delim, char* buf, size_t maxLen);
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  void flush() override;
  static MySerialImpl instance;
};
inline MySerialImpl MySerialImpl::instance;

#define logSerial MySerialImpl::instance
