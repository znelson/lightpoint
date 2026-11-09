#pragma once

#include <HalPlatform.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <stdint.h>

#include <cstring>
#include <memory>
#include <string>

// Streaming writer for the 2-bit (4 level) pixel cache produced during decode.
// Packs 4 pixels per byte, MSB first.
//
// The decoders emit destination rows top to bottom, so only a small band of
// rows is held in RAM; completed rows are appended to the cache file as the
// decode advances. A full-screen image needs a couple of KB here instead of
// one contiguous ~75KB block, which the heap usually cannot supply.
//
// File layout: uint16 width, uint16 height, then height rows of bytesPerRow.
// Rows the decoder never writes are stored as zeros.
struct PixelCache {
  std::unique_ptr<uint8_t[]> band;
  HalFile file;
  std::string path;
  int width;
  int height;
  int bytesPerRow;
  int originX;    // config.x - to convert screen coords to cache coords
  int originY;    // config.y
  int bandRows;   // rows the band can hold
  int bandStart;  // image row stored at the start of the band
  bool active;

  PixelCache()
      : width(0), height(0), bytesPerRow(0), originX(0), originY(0), bandRows(0), bandStart(0), active(false) {}
  ~PixelCache() { abandon(); }
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;
  PixelCache(PixelCache&&) = delete;
  PixelCache& operator=(PixelCache&&) = delete;

  static constexpr size_t MAX_CACHE_BYTES = 256 * 1024;  // 256KB limit for embedded targets
  static constexpr size_t MAX_BAND_BYTES = 8 * 1024;

  // Opens the cache file and allocates a band of maxBandRows rows. maxBandRows
  // must cover the tallest run of rows the decoder writes between advanceTo() calls.
  bool begin(const std::string& cachePath, int w, int h, int ox, int oy, int maxBandRows) {
    abandon();
    if (w <= 0 || h <= 0 || w > UINT16_MAX || h > UINT16_MAX || maxBandRows <= 0) return false;
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
    bandRows = maxBandRows < h ? maxBandRows : h;
    bandStart = 0;

    const size_t fileBytes = (size_t)bytesPerRow * h;
    if (fileBytes > MAX_CACHE_BYTES) {
      LOG_ERR("IMG", "Cache too large: %u bytes for %dx%d (limit %u)", static_cast<unsigned>(fileBytes), w, h,
              static_cast<unsigned>(MAX_CACHE_BYTES));
      return false;
    }
    const size_t bandBytes = (size_t)bytesPerRow * bandRows;
    if (bandBytes > MAX_BAND_BYTES) {
      LOG_ERR("IMG", "Cache band too large: %u bytes (%d rows) for %dx%d", static_cast<unsigned>(bandBytes), bandRows,
              w, h);
      return false;
    }

    band = makeUniqueNoThrow<uint8_t[]>(bandBytes);  // zero-initialized
    if (!band) {
      LOG_ERR("IMG", "OOM cache band: %u bytes for %dx%d (free %u, largest block %u)", static_cast<unsigned>(bandBytes),
              w, h, static_cast<unsigned>(halPlatform.freeHeap()),
              static_cast<unsigned>(halPlatform.largestFreeBlock()));
      return false;
    }

    if (!halStorage.openFileForWrite("IMG", cachePath, file)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %s", cachePath.c_str());
      band.reset();
      return false;
    }
    path = cachePath;
    active = true;

    const uint16_t header[2] = {static_cast<uint16_t>(w), static_cast<uint16_t>(h)};
    if (file.write(header, sizeof(header)) != sizeof(header)) {
      LOG_ERR("IMG", "Cache header write failed: %s", path.c_str());
      abandon();
      return false;
    }
    return true;
  }

  // Declares every image row before `row` complete: flushes them to the file
  // and moves the band so it starts at `row`. Rows must not go backwards.
  // On failure the cache is abandoned and the partial file removed.
  bool advanceTo(int row) {
    if (!active) return false;
    if (row > height) row = height;
    if (row == bandStart) return true;
    if (row < bandStart) {
      LOG_ERR("IMG", "Cache rows out of order (%d after %d), dropping cache", row, bandStart);
      abandon();
      return false;
    }

    int pending = row - bandStart;
    const int held = pending < bandRows ? pending : bandRows;
    bool ok = writeRows(held);
    pending -= held;
    memset(band.get(), 0, (size_t)bytesPerRow * bandRows);
    // Rows skipped past the end of the band were never written: store zeros.
    while (ok && pending > 0) {
      const int n = pending < bandRows ? pending : bandRows;
      ok = writeRows(n);
      pending -= n;
    }
    if (!ok) {
      LOG_ERR("IMG", "Cache write failed, dropping cache: %s", path.c_str());
      abandon();
      return false;
    }
    bandStart = row;
    return true;
  }

  // True when image rows [row, row + rows) all fit in the band as positioned.
  bool holdsRows(int row, int rows) const { return active && row >= bandStart && row + rows <= bandStart + bandRows; }

  // Screen Y of the band's first row, for DirectCacheWriter::beginRow().
  int bandScreenY() const { return originY + bandStart; }

  // Flushes the remaining rows and closes the file.
  bool finish() {
    if (!advanceTo(height)) return false;
    active = false;
    band.reset();
    if (!file.close()) {
      LOG_ERR("IMG", "Cache close failed, dropping cache: %s", path.c_str());
      halStorage.remove(path.c_str());
      return false;
    }
    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes)", path.c_str(), width, height, 4 + bytesPerRow * height);
    return true;
  }

  // Stops caching and removes the partial file. Safe to call when inactive.
  void abandon() {
    if (!active) return;
    active = false;
    band.reset();
    file.close();  // must close before remove: FatFS will not unlink an open file
    halStorage.remove(path.c_str());
  }

 private:
  bool writeRows(int rows) {
    const size_t bytes = (size_t)bytesPerRow * rows;
    return file.write(band.get(), bytes) == bytes;
  }
};
