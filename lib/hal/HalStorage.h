#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <vector>

// Open flags (matching SdFat values for API compatibility)
using oflag_t = uint16_t;
static constexpr oflag_t O_RDONLY = 0x00;
static constexpr oflag_t O_WRONLY = 0x01;
static constexpr oflag_t O_RDWR = 0x02;
static constexpr oflag_t O_CREAT = 0x200;
static constexpr oflag_t O_TRUNC = 0x400;
static constexpr oflag_t O_WRITE = O_WRONLY | O_RDWR;  // SdFat compat alias

class HalFile;

class HalStorage {
 public:
  HalStorage();
  HalStorage(const HalStorage&) = delete;
  HalStorage& operator=(const HalStorage&) = delete;
  HalStorage(HalStorage&&) = delete;
  HalStorage& operator=(HalStorage&&) = delete;

  bool begin();
  bool ready() const;
  std::vector<std::string> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a string. Returns empty string on failure.
  std::string readFile(const char* path);

  HalFile open(const char* path, oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
  bool removeDir(const char* path);

  // Implementation detail: defined in HalStorage.cpp, references a file-static
  // FreeRTOS mutex so this header stays free of FreeRTOS / ESP-IDF symbols.
  // See the StorageLock policy block at the top of HalStorage.cpp.
  class StorageLock;
};

extern HalStorage halStorage;  // Singleton

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  std::unique_ptr<Impl> impl;
  explicit HalFile(std::unique_ptr<Impl> impl);

 public:
  HalFile();
  ~HalFile();
  HalFile(HalFile&&);
  HalFile& operator=(HalFile&&);
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush() override;
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // read a single byte
  size_t write(const void* buf, size_t count);
  size_t write(const uint8_t* buf, size_t count) override { return write(static_cast<const void*>(buf), count); }
  size_t write(uint8_t b) override;
  bool rename(const char* newPath);
  bool isDirectory() const;
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const;
  operator bool() const;
};
