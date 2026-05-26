// Host-test implementations for the real lib/hal/HalStorage.h. The production
// header is FreeRTOS-clean so tests use it verbatim; this file supplies the
// halStorage global plus stub bodies for every HalStorage / HalFile method
// the test binary actually references. Methods declared in the header but
// never called from test-linked code (most of HalStorage's filesystem
// surface, plus HalFile's seek/size/position/openNextFile/etc.) intentionally
// have no definition here -- the linker only complains about referenced
// symbols, and an undefined-symbol failure on a previously-unused method
// is the signal that a new test exercises new HAL surface. Virtual methods
// from Print (~HalFile, write(uint8_t), flush) must be defined regardless
// because the vtable references them.

#include "HalStorageTestApi.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
std::unordered_map<std::string, std::string> readContents;
}  // namespace

// HalFile::Impl: minimal test state. The production layout has FILE*/DIR* and
// path buffers; tests only ever need read content + a position cursor.
class HalFile::Impl {
 public:
  std::string content;
  size_t pos = 0;
};

namespace test_stubs {

void seedHalFileContent(const std::string& path, std::string content) {
  readContents[path] = std::move(content);
}

void clearHalFileContent() { readContents.clear(); }

HalFile makeReadableHalFile(std::string content) {
  static int counter = 0;
  const std::string path = "/test/stub-" + std::to_string(counter++);
  seedHalFileContent(path, std::move(content));
  HalFile file;
  halStorage.openFileForRead("TEST", path.c_str(), file);
  return file;
}

}  // namespace test_stubs

// ---- HalStorage --------------------------------------------------------

HalStorage halStorage;  // Singleton instance

HalStorage::HalStorage() = default;

bool HalStorage::exists(const char*) { return false; }
bool HalStorage::remove(const char*) { return false; }

bool HalStorage::openFileForRead(const char* /*moduleName*/, const char* path, HalFile& file) {
  auto it = readContents.find(path);
  if (it == readContents.end()) return false;
  auto impl = std::make_unique<HalFile::Impl>();
  impl->content = it->second;
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

// Write paths are not exercised by tests; refuse to open so cache code stays
// no-op without touching the host filesystem.
bool HalStorage::openFileForWrite(const char*, const char*, HalFile&) { return false; }
bool HalStorage::openFileForWrite(const char*, const std::string&, HalFile&) { return false; }

// ---- HalFile -----------------------------------------------------------

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

// Virtual overrides from Print -- needed for the vtable even if unused.
void HalFile::flush() {}
size_t HalFile::write(uint8_t) { return 1; }

// Called via the inline write(const uint8_t*, size_t) override in the header.
size_t HalFile::write(const void*, size_t count) { return count; }

int HalFile::available() const {
  if (!impl) return 0;
  return static_cast<int>(impl->content.size() - impl->pos);
}

int HalFile::read(void* buf, size_t count) {
  if (!impl) return 0;
  const size_t remaining = impl->content.size() - impl->pos;
  const size_t toRead = count < remaining ? count : remaining;
  std::memcpy(buf, impl->content.data() + impl->pos, toRead);
  impl->pos += toRead;
  return static_cast<int>(toRead);
}

bool HalFile::close() {
  if (!impl) return false;
  impl.reset();
  return true;
}

HalFile::operator bool() const { return impl != nullptr; }
