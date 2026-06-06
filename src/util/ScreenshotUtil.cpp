#include "ScreenshotUtil.h"

#include <BitmapHelpers.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalPlatform.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <string>

#include "Bitmap.h"  // Required for BmpHeader struct definition
#include "activities/Activity.h"

void ScreenshotUtil::buildFilename(const ScreenshotInfo& info, char* buf, size_t bufSize) {
  const unsigned int ts = static_cast<unsigned int>(halPlatform.millis());

  if (info.readerType == ScreenshotInfo::ReaderType::None || info.title[0] == '\0') {
    snprintf(buf, bufSize, "/screenshots/screenshot-%u.bmp", ts);
    return;
  }

  char sanitizedTitle[64];
  FsHelpers::sanitizePathComponentForFat32(info.title, sanitizedTitle, sizeof(sanitizedTitle));
  if (sanitizedTitle[0] == '\0') {
    snprintf(buf, bufSize, "/screenshots/screenshot-%u.bmp", ts);
    return;
  }

  int pct = info.progressPercent;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  const bool hasChapter = info.readerType == ScreenshotInfo::ReaderType::Epub && info.spineIndex.has_value();
  // Display spine index as 1-based for user-facing filenames
  const int chapterNum = hasChapter ? *info.spineIndex + 1 : 0;

  if (hasChapter) {
    snprintf(buf, bufSize, "/screenshots/%s/%s_ch%d_p%d_%dpct_%u.bmp", sanitizedTitle, sanitizedTitle, chapterNum,
             info.currentPage, pct, ts);
  } else {
    snprintf(buf, bufSize, "/screenshots/%s/%s_p%d_%dpct_%u.bmp", sanitizedTitle, sanitizedTitle, info.currentPage, pct,
             ts);
  }

  // Truncate title if total path exceeds FAT32 limit
  if (strlen(buf) > 255) {
    size_t titleLen = strlen(sanitizedTitle);
    size_t overhead = strlen(buf) - 2 * titleLen;
    if (overhead < 255) {
      size_t maxTitleLen = (255 - overhead) / 2;
      // Walk back to a valid UTF-8 boundary to avoid corrupting multibyte characters
      while (maxTitleLen > 0 && (sanitizedTitle[maxTitleLen] & 0xC0) == 0x80) {
        maxTitleLen--;
      }
      sanitizedTitle[maxTitleLen] = '\0';
      if (hasChapter) {
        snprintf(buf, bufSize, "/screenshots/%s/%s_ch%d_p%d_%dpct_%u.bmp", sanitizedTitle, sanitizedTitle, chapterNum,
                 info.currentPage, pct, ts);
      } else {
        snprintf(buf, bufSize, "/screenshots/%s/%s_p%d_%dpct_%u.bmp", sanitizedTitle, sanitizedTitle, info.currentPage,
                 pct, ts);
      }
    } else {
      snprintf(buf, bufSize, "/screenshots/screenshot-%u.bmp", ts);
    }
  }
}

void ScreenshotUtil::takeScreenshot(GfxRenderer& renderer) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) {
    LOG_ERR("SCR", "Framebuffer not available");
    return;
  }

  ScreenshotInfo info = activityManager.getScreenshotInfo();
  char filename[256];
  buildFilename(info, filename, sizeof(filename));

  bool saved = saveFramebufferAsBmp(filename, fb, renderer.getDisplayWidth(), renderer.getDisplayHeight());
  if (saved) {
    LOG_DBG("SCR", "Screenshot saved to %s", filename);
  } else {
    LOG_ERR("SCR", "Failed to save screenshot");
    return;
  }

  // Display a border around the screen to indicate a screenshot was taken
  if (renderer.storeBwBuffer()) {
    const auto margins = renderer.getOrientedViewableMargins();
    int width = renderer.getScreenWidth() - margins.left - margins.right - 1;
    int height = renderer.getScreenHeight() - margins.top - margins.bottom - 1;
    // Add extra margin to the border to make it more visible
    renderer.drawRect(margins.left + 1, margins.top + 1, width - 2, height - 2, 2, true);
    renderer.displayBuffer();
    halPlatform.delay(1000);
    renderer.restoreBwBuffer();
    renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
  }
}

bool ScreenshotUtil::saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height) {
  if (!framebuffer) {
    return false;
  }

  // Note: the width and height, we rotate the image 90d counter-clockwise to match the default display orientation
  int phyWidth = height;
  int phyHeight = width;

  std::string path(filename);
  size_t last_slash = path.find_last_of('/');
  if (last_slash != std::string::npos) {
    std::string dir = path.substr(0, last_slash);
    if (!halStorage.exists(dir.c_str())) {
      if (!halStorage.mkdir(dir.c_str())) {
        return false;
      }
    }
  }

  HalFile file;
  if (!halStorage.openFileForWrite("SCR", filename, file)) {
    LOG_ERR("SCR", "Failed to save screenshot");
    return false;
  }

  BmpHeader header;

  createBmpHeader(&header, phyWidth, phyHeight, BmpRowOrder::BottomUp);

  bool write_error = false;
  if (file.write(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    write_error = true;
  }

  if (write_error) {
    // Explicitly close() file before calling halStorage.remove()
    file.close();
    halStorage.remove(filename);
    return false;
  }

  const uint32_t rowSizePadded = (phyWidth + 31) / 32 * 4;
  // Max row size for 528px height (X3) after rotation = 68 bytes; use fixed buffer to avoid VLA
  constexpr size_t kMaxRowSize = 68;
  if (rowSizePadded > kMaxRowSize) {
    LOG_ERR("SCR", "Row size %u exceeds buffer capacity", static_cast<unsigned int>(rowSizePadded));
    // Explicitly close() file before calling halStorage.remove()
    file.close();
    halStorage.remove(filename);
    return false;
  }

  // rotate the image 90d counter-clockwise on-the-fly while writing to save memory
  uint8_t rowBuffer[kMaxRowSize];
  memset(rowBuffer, 0, rowSizePadded);

  for (int outY = 0; outY < phyHeight; outY++) {
    for (int outX = 0; outX < phyWidth; outX++) {
      // 90d counter-clockwise: source (srcX, srcY)
      // BMP rows are bottom-to-top, so outY=0 is the bottom of the displayed image
      int srcX = width - 1 - outY;     // phyHeight == width
      int srcY = phyWidth - 1 - outX;  // phyWidth == height
      int fbIndex = srcY * (width / 8) + (srcX / 8);
      uint8_t pixel = (framebuffer[fbIndex] >> (7 - (srcX % 8))) & 0x01;
      rowBuffer[outX / 8] |= pixel << (7 - (outX % 8));
    }
    if (file.write(rowBuffer, rowSizePadded) != rowSizePadded) {
      write_error = true;
      break;
    }
    memset(rowBuffer, 0, rowSizePadded);  // Clear the buffer for the next row
  }

  // Explicitly close() file before calling halStorage.remove()
  file.close();

  if (write_error) {
    halStorage.remove(filename);
    return false;
  }

  return true;
}
