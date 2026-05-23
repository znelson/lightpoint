#define HAL_STORAGE_IMPL
#include "HalStorage.h"

#include <Logging.h>
#include <dirent.h>
#include <driver/spi_master.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>

HalStorage HalStorage::instance;

// All SD card paths are under the VFS mount point "/sd".
// HalStorage prepends this prefix internally so callers use plain paths like "/books/file.epub".
namespace {
constexpr char SD_PREFIX[] = "/sd";
constexpr gpio_num_t SD_CS = GPIO_NUM_12;
static sdmmc_card_t* sdCard = nullptr;
static bool sdInitialized = false;

std::string sdPath(const char* path) {
  std::string result = SD_PREFIX;
  if (path && path[0] != '/') result += '/';
  if (path) result += path;
  return result;
}

// Convert oflag_t to fopen mode string
const char* oflagToMode(oflag_t oflag) {
  const bool write = (oflag & O_WRITE) != 0;
  const bool creat = (oflag & O_CREAT) != 0;
  const bool trunc = (oflag & O_TRUNC) != 0;
  if (write && (creat || trunc)) return "w+";
  if (write) return "r+";
  return "r";
}

// Recursive remove — operates on full /sd/... paths, no mutex (called under StorageLock).
bool removeDirRecursive(const char* path) {
  DIR* dir = opendir(path);
  if (!dir) return false;
  struct dirent* entry;
  char childPath[512];
  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    snprintf(childPath, sizeof(childPath), "%s/%s", path, entry->d_name);
    if (entry->d_type == DT_DIR) {
      if (!removeDirRecursive(childPath)) {
        closedir(dir);
        return false;
      }
    } else {
      if (remove(childPath) != 0) {
        closedir(dir);
        return false;
      }
    }
  }
  closedir(dir);
  return rmdir(path) == 0;
}
}  // namespace

HalStorage::HalStorage() {
  storageMutex = xSemaphoreCreateMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() {
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI2_HOST;

  sdspi_device_config_t slotConfig = SDSPI_DEVICE_CONFIG_DEFAULT();
  slotConfig.gpio_cs = SD_CS;
  slotConfig.host_id = SPI2_HOST;

  esp_vfs_fat_mount_config_t mountConfig = {};
  mountConfig.format_if_mount_failed = false;
  mountConfig.max_files = 8;
  mountConfig.allocation_unit_size = 16 * 1024;

  const esp_err_t err = esp_vfs_fat_sdspi_mount("/sd", &host, &slotConfig, &mountConfig, &sdCard);
  sdInitialized = (err == ESP_OK);
  if (!sdInitialized) {
    LOG_ERR("SD", "Mount failed: %s (0x%x)", esp_err_to_name(err), err);
  }
  return sdInitialized;
}

bool HalStorage::ready() const { return sdInitialized; }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTake(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGive(HalStorage::getInstance().storageMutex); }
};

std::vector<std::string> HalStorage::listFiles(const char* path, int maxFiles) {
  StorageLock lock;
  std::vector<std::string> ret;
  const std::string fullPath = sdPath(path);
  DIR* dir = opendir(fullPath.c_str());
  if (!dir) return ret;
  int count = 0;
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr && count < maxFiles) {
    if (entry->d_type == DT_DIR) continue;
    ret.emplace_back(entry->d_name);
    ++count;
  }
  closedir(dir);
  return ret;
}

std::string HalStorage::readFile(const char* path) {
  StorageLock lock;
  const std::string fullPath = sdPath(path);
  FILE* fp = fopen(fullPath.c_str(), "r");
  if (!fp) return {};
  constexpr size_t maxSize = 50000;
  std::string content;
  char buf[256];
  size_t total = 0;
  while (total < maxSize) {
    const size_t want = std::min(sizeof(buf), maxSize - total);
    const size_t r = fread(buf, 1, want, fp);
    if (r == 0) break;
    content.append(buf, r);
    total += r;
  }
  fclose(fp);
  return content;
}

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  StorageLock lock;
  const std::string fullPath = sdPath(path);
  FILE* fp = fopen(fullPath.c_str(), "r");
  if (!fp) return false;
  constexpr size_t localBufSize = 256;
  uint8_t buf[localBufSize];
  const size_t toRead = (chunkSize == 0 || chunkSize > localBufSize) ? localBufSize : chunkSize;
  while (true) {
    const size_t r = fread(buf, 1, toRead, fp);
    if (r > 0)
      out.write(buf, r);
    else
      break;
  }
  fclose(fp);
  return true;
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  StorageLock lock;
  if (!buffer || bufferSize == 0) return 0;
  const std::string fullPath = sdPath(path);
  FILE* fp = fopen(fullPath.c_str(), "r");
  if (!fp) {
    buffer[0] = '\0';
    return 0;
  }
  const size_t maxToRead = (maxBytes == 0) ? (bufferSize - 1) : std::min(maxBytes, bufferSize - 1);
  const size_t r = fread(buffer, 1, maxToRead, fp);
  buffer[r] = '\0';
  fclose(fp);
  return r;
}

bool HalStorage::writeFile(const char* path, const std::string& content) {
  StorageLock lock;
  const std::string fullPath = sdPath(path);
  FILE* fp = fopen(fullPath.c_str(), "w");
  if (!fp) return false;
  const size_t written = fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  return written == content.size();
}

bool HalStorage::ensureDirectoryExists(const char* path) {
  StorageLock lock;
  const std::string fullPath = sdPath(path);
  struct stat st;
  if (stat(fullPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return true;
  return ::mkdir(fullPath.c_str(), 0777) == 0;
}

bool HalStorage::mkdir(const char* path, const bool /*pFlag*/) {
  StorageLock lock;
  const std::string fullPath = sdPath(path);
  return ::mkdir(fullPath.c_str(), 0777) == 0;
}

bool HalStorage::exists(const char* path) {
  StorageLock lock;
  struct stat st;
  return stat(sdPath(path).c_str(), &st) == 0;
}

bool HalStorage::remove(const char* path) {
  StorageLock lock;
  return ::remove(sdPath(path).c_str()) == 0;
}

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  StorageLock lock;
  return ::rename(sdPath(oldPath).c_str(), sdPath(newPath).c_str()) == 0;
}

bool HalStorage::rmdir(const char* path) {
  StorageLock lock;
  return ::rmdir(sdPath(path).c_str()) == 0;
}

bool HalStorage::removeDir(const char* path) {
  StorageLock lock;
  return removeDirRecursive(sdPath(path).c_str());
}

// ---- HalFile::Impl -------------------------------------------------------

class HalFile::Impl {
 public:
  FILE* fp = nullptr;
  DIR* dir = nullptr;
  ~Impl() {
    if (fp) fclose(fp);
    if (dir) closedir(dir);
  }
  char name[256] = {};      // filename component (for getName, openNextFile)
  char fullPath[512] = {};  // full "/sd/..." path (for rename)
  bool isDir = false;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

// ---- HalStorage::open ----------------------------------------------------

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;
  const std::string full = sdPath(path);
  const char* mode = oflagToMode(oflag);

  struct stat st;
  const bool pathIsDir = (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode));

  auto newImpl = std::make_unique<HalFile::Impl>();
  // Store just the leaf name for getName()
  const char* slash = strrchr(path, '/');
  strncpy(newImpl->name, slash ? slash + 1 : path, sizeof(newImpl->name) - 1);
  strncpy(newImpl->fullPath, full.c_str(), sizeof(newImpl->fullPath) - 1);
  newImpl->isDir = pathIsDir;

  if (pathIsDir) {
    newImpl->dir = opendir(full.c_str());
  } else {
    newImpl->fp = fopen(full.c_str(), mode);
  }
  return HalFile(std::move(newImpl));
}

bool HalStorage::openFileForRead(const char* /*moduleName*/, const char* path, HalFile& file) {
  StorageLock lock;
  const std::string full = sdPath(path);
  auto newImpl = std::make_unique<HalFile::Impl>();
  newImpl->fp = fopen(full.c_str(), "r");
  if (!newImpl->fp) return false;
  const char* slash = strrchr(path, '/');
  strncpy(newImpl->name, slash ? slash + 1 : path, sizeof(newImpl->name) - 1);
  strncpy(newImpl->fullPath, full.c_str(), sizeof(newImpl->fullPath) - 1);
  file = HalFile(std::move(newImpl));
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* /*moduleName*/, const char* path, HalFile& file) {
  StorageLock lock;
  const std::string full = sdPath(path);
  auto newImpl = std::make_unique<HalFile::Impl>();
  newImpl->fp = fopen(full.c_str(), "w");
  if (!newImpl->fp) return false;
  const char* slash = strrchr(path, '/');
  strncpy(newImpl->name, slash ? slash + 1 : path, sizeof(newImpl->name) - 1);
  strncpy(newImpl->fullPath, full.c_str(), sizeof(newImpl->fullPath) - 1);
  file = HalFile(std::move(newImpl));
  return true;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

// ---- HalFile methods -----------------------------------------------------

#define HAL_FILE_LOCK HalStorage::StorageLock lock;

void HalFile::flush() {
  HAL_FILE_LOCK
  if (impl && impl->fp) fflush(impl->fp);
}

size_t HalFile::getName(char* name, size_t len) {
  HAL_FILE_LOCK
  if (!impl || !name || len == 0) return 0;
  const size_t n = strnlen(impl->name, sizeof(impl->name));
  const size_t copy = n < len ? n : len - 1;
  memcpy(name, impl->name, copy);
  name[copy] = '\0';
  return copy;
}

static size_t fileSize_(FILE* fp) {
  const long cur = ftell(fp);
  fseek(fp, 0, SEEK_END);
  const size_t sz = static_cast<size_t>(ftell(fp));
  fseek(fp, cur, SEEK_SET);
  return sz;
}

size_t HalFile::size() { return fileSize(); }
size_t HalFile::fileSize() {
  HAL_FILE_LOCK
  if (!impl || !impl->fp) return 0;
  return fileSize_(impl->fp);
}
uint64_t HalFile::fileSize64() { return static_cast<uint64_t>(fileSize()); }

bool HalFile::seek(size_t pos) { return seekSet(pos); }
bool HalFile::seek64(uint64_t pos) {
  HAL_FILE_LOCK
  if (!impl || !impl->fp) return false;
  return fseek(impl->fp, static_cast<long>(pos), SEEK_SET) == 0;
}
bool HalFile::seekCur(int64_t offset) {
  HAL_FILE_LOCK
  if (!impl || !impl->fp) return false;
  return fseek(impl->fp, static_cast<long>(offset), SEEK_CUR) == 0;
}
bool HalFile::seekSet(size_t offset) {
  HAL_FILE_LOCK
  if (!impl || !impl->fp) return false;
  return fseek(impl->fp, static_cast<long>(offset), SEEK_SET) == 0;
}

int HalFile::available() const {
  HAL_FILE_LOCK
  if (!impl || !impl->fp) return 0;
  const long cur = ftell(impl->fp);
  fseek(impl->fp, 0, SEEK_END);
  const long end = ftell(impl->fp);
  fseek(impl->fp, cur, SEEK_SET);
  return static_cast<int>(end > cur ? end - cur : 0);
}

size_t HalFile::position() const {
  HAL_FILE_LOCK
  if (!impl || !impl->fp) return 0;
  return static_cast<size_t>(ftell(impl->fp));
}

int HalFile::read(void* buf, size_t count) {
  HAL_FILE_LOCK
  if (!impl || !impl->fp) return -1;
  return static_cast<int>(fread(buf, 1, count, impl->fp));
}
int HalFile::read() {
  HAL_FILE_LOCK
  if (!impl || !impl->fp) return -1;
  return fgetc(impl->fp);
}

size_t HalFile::write(const void* buf, size_t count) {
  HAL_FILE_LOCK
  if (!impl || !impl->fp) return 0;
  return fwrite(buf, 1, count, impl->fp);
}
size_t HalFile::write(uint8_t b) {
  HAL_FILE_LOCK
  if (!impl || !impl->fp) return 0;
  // fwrite, not fputc: picolibc's __bufio_put writes buf[bf->len] BEFORE
  // checking overflow, so if entered with bf->len == bf->size it scribbles
  // one byte past the buffer and corrupts the heap-poison tail canary.
  // fwrite flushes first when the buffer is already full.
  return fwrite(&b, 1, 1, impl->fp);
}

bool HalFile::rename(const char* newPath) {
  HAL_FILE_LOCK
  if (!impl || !impl->fullPath[0]) return false;
  if (impl->fp) {
    fclose(impl->fp);
    impl->fp = nullptr;
  }
  return ::rename(impl->fullPath, sdPath(newPath).c_str()) == 0;
}

bool HalFile::isDirectory() const {
  if (!impl) return false;
  return impl->isDir;
}

void HalFile::rewindDirectory() {
  HAL_FILE_LOCK
  if (impl && impl->dir) rewinddir(impl->dir);
}

bool HalFile::close() {
  HAL_FILE_LOCK
  if (!impl) return false;
  bool ok = true;
  if (impl->fp) {
    ok = (fclose(impl->fp) == 0);
    impl->fp = nullptr;
  }
  if (impl->dir) {
    closedir(impl->dir);
    impl->dir = nullptr;
  }
  return ok;
}

HalFile HalFile::openNextFile() {
  HAL_FILE_LOCK
  if (!impl || !impl->dir) return HalFile();

  struct dirent* entry;
  do {
    entry = readdir(impl->dir);
  } while (entry && (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0));

  if (!entry) return HalFile();

  char childPath[512];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
  snprintf(childPath, sizeof(childPath), "%s/%s", impl->fullPath, entry->d_name);
#pragma GCC diagnostic pop

  auto newImpl = std::make_unique<Impl>();
  strncpy(newImpl->name, entry->d_name, sizeof(newImpl->name) - 1);
  strncpy(newImpl->fullPath, childPath, sizeof(newImpl->fullPath) - 1);

  struct stat st;
  newImpl->isDir = (stat(childPath, &st) == 0 && S_ISDIR(st.st_mode));

  if (newImpl->isDir) {
    newImpl->dir = opendir(childPath);
  } else {
    newImpl->fp = fopen(childPath, "r");
  }
  return HalFile(std::move(newImpl));
}

bool HalFile::isOpen() const { return impl != nullptr && (impl->fp != nullptr || impl->dir != nullptr); }
HalFile::operator bool() const { return isOpen(); }
