#include "BookmarkStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>

namespace {
constexpr const char* MOD = "BKM";

// File location: /.crosspoint/bookmarks/<hash>.bin. Hash matches the Epub
// cache-dir convention (Epub.h ctor) so bookmarks survive path-stable
// renames; the separate root keeps them outside the per-book cache dir
// so Clear Cache leaves bookmarks alone.
std::string pathForBook(const std::string& bookPath) {
  return "/.crosspoint/bookmarks/" + std::to_string(std::hash<std::string>{}(bookPath)) + ".bin";
}

// Wire-format mirror of BookmarkEntry. Fixed layout, written as one
// 78 B buffer so each read/write is a single SD op rather than per-field
// PODs.
struct RawEntry {
  uint16_t spineIndex;
  uint16_t paragraphIndex;
  uint16_t liIndex;
  uint8_t summaryLen;
  uint8_t summary[BookmarkStore::SUMMARY_MAX];
};
static_assert(sizeof(RawEntry) == BookmarkStore::ENTRY_SIZE, "BookmarkStore entry size mismatch");

void encode(const BookmarkEntry& src, RawEntry& dst) {
  dst.spineIndex = src.spineIndex;
  dst.paragraphIndex = src.paragraphIndex;
  dst.liIndex = src.liIndex;
  const size_t n = std::min(src.summary.size(), static_cast<size_t>(BookmarkStore::SUMMARY_MAX));
  dst.summaryLen = n;
  std::memcpy(dst.summary, src.summary.data(), n);
  // Zero unused tail so leftover stack/heap bytes don't leak onto disk.
  if (n < BookmarkStore::SUMMARY_MAX) std::memset(dst.summary + n, 0, BookmarkStore::SUMMARY_MAX - n);
}

void decode(const RawEntry& src, BookmarkEntry& dst) {
  dst.spineIndex = src.spineIndex;
  dst.paragraphIndex = src.paragraphIndex;
  dst.liIndex = src.liIndex;
  const size_t n = std::min(static_cast<size_t>(src.summaryLen), static_cast<size_t>(BookmarkStore::SUMMARY_MAX));
  dst.summary.assign(reinterpret_cast<const char*>(src.summary), n);
}

// Open path for read, validate the version byte, return the count via outCount.
// On any failure the file is closed and false is returned.
bool openValidated(const std::string& path, HalFile& f, uint16_t& outCount) {
  if (!halStorage.openFileForRead(MOD, path.c_str(), f)) return false;
  uint8_t version;
  serialization::readPod(f, version);
  if (version != BookmarkStore::FILE_VERSION) {
    LOG_ERR(MOD, "Version mismatch (got %u, expected %u): %s", version, BookmarkStore::FILE_VERSION, path.c_str());
    f.close();
    return false;
  }
  serialization::readPod(f, outCount);
  return true;
}

// Ensure the parent directory of path exists. Bookmarks live under
// /.crosspoint/bookmarks/ which may not exist on first use.
void ensureParentDir(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return;
  halStorage.mkdir(path.substr(0, slash).c_str());
}

std::string tmpPath(const std::string& path) { return path + ".tmp"; }
}  // namespace

BookmarkStore::BookmarkStore(const std::string& bookPath) : path_(pathForBook(bookPath)) {}

bool BookmarkStore::sortsBefore(const BookmarkEntry& a, const BookmarkEntry& b) {
  if (a.spineIndex != b.spineIndex) return a.spineIndex < b.spineIndex;
  return a.paragraphIndex < b.paragraphIndex;
}

uint16_t BookmarkStore::count() {
  if (cachedCount_) return *cachedCount_;
  HalFile f;
  uint16_t n = 0;
  if (openValidated(path_, f, n)) {
    f.close();
  } else {
    n = 0;
  }
  cachedCount_ = n;
  return n;
}

std::optional<BookmarkEntry> BookmarkStore::entryAt(uint16_t index) {
  if (index >= count()) return std::nullopt;
  HalFile f;
  uint16_t n = 0;
  if (!openValidated(path_, f, n)) return std::nullopt;
  if (index >= n) {
    f.close();
    return std::nullopt;
  }
  if (!f.seek(HEADER_SIZE + static_cast<size_t>(index) * ENTRY_SIZE)) {
    f.close();
    return std::nullopt;
  }
  RawEntry raw;
  const int got = f.read(&raw, sizeof(raw));
  f.close();
  if (got != static_cast<int>(sizeof(raw))) return std::nullopt;
  BookmarkEntry out;
  decode(raw, out);
  return out;
}

bool BookmarkStore::insert(const BookmarkEntry& entry) {
  ensureParentDir(path_);
  const uint16_t n = count();

  // Locate insertion index by reading entries in order until one sorts after
  // the new entry; back-bookmarking (rare) walks more entries than forward
  // reading (common, where insertIndex == n on the first comparison).
  HalFile src;
  bool hasSrc = false;
  if (n > 0) {
    uint16_t srcCount = 0;
    if (!openValidated(path_, src, srcCount)) {
      LOG_ERR(MOD, "Insert: failed to read existing bookmark file: %s", path_.c_str());
      return false;
    }
    hasSrc = true;
  }

  const std::string tmp = tmpPath(path_);
  HalFile dst;
  if (!halStorage.openFileForWrite(MOD, tmp.c_str(), dst)) {
    LOG_ERR(MOD, "Insert: failed to open temp file: %s", tmp.c_str());
    if (hasSrc) src.close();
    return false;
  }

  const uint16_t newCount = n + 1;
  serialization::writePod(dst, FILE_VERSION);
  serialization::writePod(dst, newCount);

  RawEntry newRaw;
  encode(entry, newRaw);
  bool inserted = false;

  for (uint16_t i = 0; i < n; ++i) {
    RawEntry raw;
    if (src.read(&raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) {
      LOG_ERR(MOD, "Insert: short read at index %u", i);
      src.close();
      dst.close();
      halStorage.remove(tmp.c_str());
      return false;
    }
    if (!inserted) {
      BookmarkEntry existing;
      decode(raw, existing);
      if (sortsBefore(entry, existing)) {
        dst.write(&newRaw, sizeof(newRaw));
        inserted = true;
      }
    }
    dst.write(&raw, sizeof(raw));
  }
  if (!inserted) {
    dst.write(&newRaw, sizeof(newRaw));
  }

  if (hasSrc) src.close();
  dst.close();

  if (!halStorage.rename(tmp.c_str(), path_.c_str())) {
    LOG_ERR(MOD, "Insert: rename failed (%s -> %s)", tmp.c_str(), path_.c_str());
    halStorage.remove(tmp.c_str());
    return false;
  }
  cachedCount_ = newCount;
  return true;
}

bool BookmarkStore::eraseAt(uint16_t index) {
  const uint16_t n = count();
  if (index >= n) return false;

  HalFile src;
  uint16_t srcCount = 0;
  if (!openValidated(path_, src, srcCount)) {
    LOG_ERR(MOD, "Erase: failed to read existing bookmark file: %s", path_.c_str());
    return false;
  }

  const std::string tmp = tmpPath(path_);
  HalFile dst;
  if (!halStorage.openFileForWrite(MOD, tmp.c_str(), dst)) {
    LOG_ERR(MOD, "Erase: failed to open temp file: %s", tmp.c_str());
    src.close();
    return false;
  }

  const uint16_t newCount = n - 1;
  serialization::writePod(dst, FILE_VERSION);
  serialization::writePod(dst, newCount);

  for (uint16_t i = 0; i < n; ++i) {
    RawEntry raw;
    if (src.read(&raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) {
      LOG_ERR(MOD, "Erase: short read at index %u", i);
      src.close();
      dst.close();
      halStorage.remove(tmp.c_str());
      return false;
    }
    if (i != index) {
      dst.write(&raw, sizeof(raw));
    }
  }

  src.close();
  dst.close();

  if (!halStorage.rename(tmp.c_str(), path_.c_str())) {
    LOG_ERR(MOD, "Erase: rename failed (%s -> %s)", tmp.c_str(), path_.c_str());
    halStorage.remove(tmp.c_str());
    return false;
  }
  cachedCount_ = newCount;

  // Empty file: remove instead of leaving a 3 B stub on SD.
  if (newCount == 0) {
    halStorage.remove(path_.c_str());
  }
  return true;
}
