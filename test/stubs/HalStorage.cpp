// Host-test implementations for the real lib/hal/HalStorage.h. The production
// header is FreeRTOS-clean so tests use it verbatim; this file supplies the
// halStorage global plus stub bodies for every HalStorage / HalFile method
// the test binary actually references. Methods declared in the header but
// never called from test-linked code (most of HalStorage's filesystem
// surface, plus HalFile's seek64/seekCur/openNextFile/etc.) intentionally
// have no definition here -- the linker only complains about referenced
// symbols, and an undefined-symbol failure on a previously-unused method
// is the signal that a new test exercises new HAL surface. Virtual methods
// from Print (~HalFile, write(uint8_t), flush) must be defined regardless
// because the vtable references them.
//
// Write support: openFileForWrite opens a HalFile whose writes accumulate
// in an in-memory buffer. close() publishes the buffer to fileStorage so a
// later openFileForRead on the same path returns the written bytes. This
// is enough to round-trip cache files (Typesetter::Section etc.) through
// the stub without touching the host filesystem.

#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#include "HalStorageTestApi.h"

namespace {
// Shared backing store. Reads pull from here; writes publish to here on
// HalFile::close. seedHalFileContent writes directly so tests can
// pre-populate read-only fixtures.
std::unordered_map<std::string, std::string> fileStorage;
}  // namespace

// HalFile::Impl: minimal test state. The production layout has FILE*/DIR* and
// path buffers; tests only ever need a content buffer plus a position cursor.
// In write mode, writePath holds the destination so close() can publish.
class HalFile::Impl {
 public:
  std::string content;
  size_t pos = 0;
  bool writable = false;
  std::string writePath;  // empty unless writable
};

namespace test_stubs {

void seedHalFileContent(const std::string& path, std::string content) { fileStorage[path] = std::move(content); }

void clearHalFileContent() { fileStorage.clear(); }

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

bool HalStorage::exists(const char* path) { return fileStorage.find(path) != fileStorage.end(); }

bool HalStorage::remove(const char* path) { return fileStorage.erase(path) > 0; }

bool HalStorage::openFileForRead([[maybe_unused]] const char* moduleName, const char* path, HalFile& file) {
  auto it = fileStorage.find(path);
  if (it == fileStorage.end()) return false;
  auto impl = std::make_unique<HalFile::Impl>();
  impl->content = it->second;
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

// Write mode truncates: any prior content for `path` is dropped from the
// storage map on close. Until close, writes accumulate in the Impl buffer
// so an unfinished write does not poison a subsequent read.
bool HalStorage::openFileForWrite([[maybe_unused]] const char* moduleName, const char* path, HalFile& file) {
  auto impl = std::make_unique<HalFile::Impl>();
  impl->writable = true;
  impl->writePath = path;
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

// ---- HalFile -----------------------------------------------------------

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

void HalFile::flush() {}

// Single-byte write: overwrites at pos if within content, extends otherwise.
size_t HalFile::write(uint8_t b) {
  if (!impl || !impl->writable) return 0;
  if (impl->pos < impl->content.size()) {
    impl->content[impl->pos] = static_cast<char>(b);
  } else {
    if (impl->pos > impl->content.size()) {
      impl->content.resize(impl->pos, '\0');  // pad with zeros if seek skipped past EOF
    }
    impl->content.push_back(static_cast<char>(b));
  }
  impl->pos++;
  return 1;
}

// Bulk write: overwrites bytes within current content, extends past EOF.
// Pads with zeros if pos > content.size() (matches typical filesystem behaviour).
size_t HalFile::write(const void* buf, size_t count) {
  if (!impl || !impl->writable || count == 0) return impl && impl->writable ? count : 0;
  const auto* src = static_cast<const char*>(buf);
  if (impl->pos > impl->content.size()) {
    impl->content.resize(impl->pos, '\0');
  }
  const size_t end = impl->pos + count;
  if (end > impl->content.size()) {
    impl->content.resize(end, '\0');
  }
  std::memcpy(&impl->content[impl->pos], src, count);
  impl->pos = end;
  return count;
}

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

size_t HalFile::position() const { return impl ? impl->pos : 0; }

size_t HalFile::size() { return impl ? impl->content.size() : 0; }

bool HalFile::seek(size_t pos) {
  if (!impl) return false;
  impl->pos = pos;
  return true;
}

// Publish writable buffer to shared storage so a later openFileForRead sees it.
bool HalFile::close() {
  if (!impl) return false;
  if (impl->writable) {
    fileStorage[impl->writePath] = std::move(impl->content);
  }
  impl.reset();
  return true;
}

HalFile::operator bool() const { return impl != nullptr; }
