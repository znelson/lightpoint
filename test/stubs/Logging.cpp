// Host-test stub for lib/Logging/Logging.h. We don't link the production
// Logging.cpp because it talks to USB-JTAG serial. The only thing TU's that
// transitively include Logging.h actually need from us is the MySerialImpl
// vtable -- the header defines `inline MySerialImpl MySerialImpl::instance;`,
// so every including TU emits a relocation against the vtable, and gcc
// (unlike clang on macOS) demands a real definition to anchor it.
//
// LOG_* macros expand to nothing when ENABLE_SERIAL_LOG is not defined
// (test builds), so logPrintf and the rest of the diagnostic API are never
// referenced and stay undefined.

#include <Logging.h>

size_t MySerialImpl::write(uint8_t) { return 1; }
size_t MySerialImpl::write(const uint8_t*, size_t size) { return size; }
void MySerialImpl::flush() {}
