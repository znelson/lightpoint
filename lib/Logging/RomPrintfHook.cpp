#include "Logging.h"

#ifdef ENABLE_SERIAL_LOG

#include <esp_rom_sys.h>

#include <cstddef>

namespace {
constexpr size_t HOOK_BUF_SIZE = 256;
char hookBuf[HOOK_BUF_SIZE];
size_t hookPos = 0;

extern "C" void romPrintfHookPutc(char c) {
  if (c == '\r') return;
  if (c == '\n' || hookPos >= HOOK_BUF_SIZE - 1) {
    if (hookPos > 0) {
      hookBuf[hookPos] = '\0';
      addToLogRingBuffer(hookBuf);
    }
    hookPos = 0;
    return;
  }
  hookBuf[hookPos++] = c;
}
}  // namespace

void installRomPrintfHook() { esp_rom_install_channel_putc(2, &romPrintfHookPutc); }

#endif  // ENABLE_SERIAL_LOG
