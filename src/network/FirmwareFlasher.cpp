#include "FirmwareFlasher.h"

#include <HalPlatform.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <psa/crypto.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "OtaBootSwitch.h"

namespace firmware_flash {

namespace {
constexpr uint8_t ESP_IMAGE_MAGIC = 0xE9;
constexpr size_t MIN_FIRMWARE_SIZE = 64 * 1024;
constexpr size_t SEC = SPI_FLASH_SEC_SIZE;  // 4 KiB
constexpr size_t BLK = 64 * 1024;           // 64 KiB block-erase granularity
constexpr size_t CHUNK = 4096;
constexpr size_t SHA_TRAILER = 32;
constexpr uint8_t CHECKSUM_SEED = 0xEF;
constexpr size_t HEADER_SIZE = 24;
constexpr size_t SEG_HEADER_SIZE = 8;
}  // namespace

const char* resultName(Result r) {
  switch (r) {
    case Result::OK:
      return "OK";
    case Result::OPEN_FAIL:
      return "OPEN_FAIL";
    case Result::TOO_SMALL:
      return "TOO_SMALL";
    case Result::TOO_LARGE:
      return "TOO_LARGE";
    case Result::BAD_MAGIC:
      return "BAD_MAGIC";
    case Result::BAD_SEGMENTS:
      return "BAD_SEGMENTS";
    case Result::BAD_CHECKSUM:
      return "BAD_CHECKSUM";
    case Result::BAD_SHA:
      return "BAD_SHA";
    case Result::BAD_SIZE:
      return "BAD_SIZE";
    case Result::NO_PARTITION:
      return "NO_PARTITION";
    case Result::OOM:
      return "OOM";
    case Result::READ_FAIL:
      return "READ_FAIL";
    case Result::ERASE_FAIL:
      return "ERASE_FAIL";
    case Result::WRITE_FAIL:
      return "WRITE_FAIL";
    case Result::OTADATA_FAIL:
      return "OTADATA_FAIL";
  }
  return "?";
}

namespace {
// Stream `length` bytes from `file` starting at the current read offset, feeding them through
// both the XOR-checksum and SHA256 accumulators. Used by validateImageFile so the whole image
// is verified end-to-end without holding it in RAM (ESP32-C3 only has ~380 KB).
Result feedHashAndChecksum(HalFile& file, size_t length, uint8_t* xorAccum, psa_hash_operation_t* sha, uint8_t* buf) {
  size_t remaining = length;
  while (remaining > 0) {
    const size_t want = std::min<size_t>(CHUNK, remaining);
    const int got = file.read(buf, want);
    if (got <= 0 || static_cast<size_t>(got) != want) return Result::READ_FAIL;
    if (sha) psa_hash_update(sha, buf, want);
    if (xorAccum) {
      uint8_t acc = *xorAccum;
      for (size_t i = 0; i < want; i++) acc ^= buf[i];
      *xorAccum = acc;
    }
    remaining -= want;
  }
  return Result::OK;
}
}  // namespace

Result validateImageFile(const char* sdPath, size_t partitionSize) {
  HalFile file;
  if (!halStorage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "validate: open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const size_t fileSize = file.fileSize();
  if (fileSize < MIN_FIRMWARE_SIZE) {
    LOG_ERR("FLASH", "validate: too small: %u", static_cast<unsigned>(fileSize));
    return Result::TOO_SMALL;
  }
  if (partitionSize > 0 && fileSize > partitionSize) {
    LOG_ERR("FLASH", "validate: too large: %u > %u", static_cast<unsigned>(fileSize),
            static_cast<unsigned>(partitionSize));
    return Result::TOO_LARGE;
  }

  uint8_t header[HEADER_SIZE];
  if (file.read(header, HEADER_SIZE) != static_cast<int>(HEADER_SIZE)) {
    LOG_ERR("FLASH", "validate: header read failed");
    return Result::READ_FAIL;
  }
  if (header[0] != ESP_IMAGE_MAGIC) {
    LOG_ERR("FLASH", "validate: bad magic 0x%02X", header[0]);
    return Result::BAD_MAGIC;
  }
  const uint8_t segCount = header[1];
  const bool hashAppended = header[23] != 0;

  auto buf = makeUniqueNoThrow<uint8_t[]>(CHUNK);
  if (!buf) {
    LOG_ERR("FLASH", "OOM chunk buffer (%u bytes)", static_cast<unsigned>(CHUNK));
    return Result::OOM;
  }

  psa_hash_operation_t shaCtx = PSA_HASH_OPERATION_INIT;
  psa_hash_setup(&shaCtx, PSA_ALG_SHA_256);
  // Releases the PSA hash context on every exit path; safe after psa_hash_finish
  // (abort on an inactive operation is a no-op). The local HalFile closes itself.
  const ScopedCleanup hashCleanup{[&shaCtx] { psa_hash_abort(&shaCtx); }};
  psa_hash_update(&shaCtx, header, HEADER_SIZE);

  uint8_t xorAccum = CHECKSUM_SEED;
  size_t pos = HEADER_SIZE;

  for (uint8_t i = 0; i < segCount; i++) {
    if (pos + SEG_HEADER_SIZE > fileSize) {
      LOG_ERR("FLASH", "validate: seg %u header overruns EOF at %u", i, static_cast<unsigned>(pos));
      return Result::BAD_SEGMENTS;
    }
    uint8_t segHdr[SEG_HEADER_SIZE];
    if (file.read(segHdr, SEG_HEADER_SIZE) != static_cast<int>(SEG_HEADER_SIZE)) {
      return Result::READ_FAIL;
    }
    psa_hash_update(&shaCtx, segHdr, SEG_HEADER_SIZE);
    pos += SEG_HEADER_SIZE;

    uint32_t dataLen;
    std::memcpy(&dataLen, segHdr + 4, sizeof(dataLen));
    if (pos + dataLen > fileSize) {
      LOG_ERR("FLASH", "validate: seg %u data overruns EOF (%u + %u > %u)", i, static_cast<unsigned>(pos),
              static_cast<unsigned>(dataLen), static_cast<unsigned>(fileSize));
      return Result::BAD_SEGMENTS;
    }

    const Result feedRes = feedHashAndChecksum(file, dataLen, &xorAccum, &shaCtx, buf.get());
    if (feedRes != Result::OK) {
      return feedRes;
    }
    pos += dataLen;
  }

  // pad_end is the 16-byte aligned offset at which the checksum byte sits at pad_end - 1.
  const size_t padEnd = (pos + 16) & ~static_cast<size_t>(15);
  const size_t expectedTotal = padEnd + (hashAppended ? SHA_TRAILER : 0);
  if (expectedTotal != fileSize) {
    LOG_ERR("FLASH", "validate: size mismatch body+pad=%u sha=%u expected=%u actual=%u", static_cast<unsigned>(padEnd),
            static_cast<unsigned>(hashAppended ? SHA_TRAILER : 0), static_cast<unsigned>(expectedTotal),
            static_cast<unsigned>(fileSize));
    return Result::BAD_SIZE;
  }

  // Read the padding bytes (which include the stored checksum at the last byte) into the SHA stream.
  const size_t padLen = padEnd - pos;
  uint8_t padBuf[16];
  if (padLen > sizeof(padBuf)) {
    return Result::BAD_SIZE;
  }
  if (padLen > 0 && file.read(padBuf, padLen) != static_cast<int>(padLen)) {
    return Result::READ_FAIL;
  }
  psa_hash_update(&shaCtx, padBuf, padLen);

  const uint8_t storedChecksum = padBuf[padLen - 1];
  if ((xorAccum & 0xFF) != storedChecksum) {
    LOG_ERR("FLASH", "validate: checksum mismatch computed=0x%02X stored=0x%02X", xorAccum, storedChecksum);
    return Result::BAD_CHECKSUM;
  }

  if (hashAppended) {
    uint8_t computed[SHA_TRAILER];
    size_t computedLen = 0;
    psa_hash_finish(&shaCtx, computed, SHA_TRAILER, &computedLen);
    uint8_t stored[SHA_TRAILER];
    if (file.read(stored, SHA_TRAILER) != static_cast<int>(SHA_TRAILER)) {
      return Result::READ_FAIL;
    }
    if (std::memcmp(computed, stored, SHA_TRAILER) != 0) {
      LOG_ERR("FLASH", "validate: SHA256 mismatch");
      return Result::BAD_SHA;
    }
  }

  return Result::OK;
}

Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx, bool alreadyValidated) {
  // Resolve destination first so we can size-check during validation. The full image-integrity
  // pass below verifies header, segment table, XOR checksum and SHA256 trailer end-to-end before
  // we touch otadata, so a truncated/corrupted .bin can never become the next boot target.
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FLASH", "no next-update partition");
    return Result::NO_PARTITION;
  }

  // When the caller already ran validateImageFile() against this same partition
  // size (e.g. SdFirmwareUpdateActivity validates before the confirmation
  // prompt), skip the redundant integrity scan. We still keep the partition
  // lookup so the rest of the flashing path stays unchanged.
  if (!alreadyValidated) {
    const Result validateRes = validateImageFile(sdPath, dest->size);
    if (validateRes != Result::OK) {
      LOG_ERR("FLASH", "image validation failed: %s", resultName(validateRes));
      return validateRes;
    }
  }

  HalFile file;
  if (!halStorage.openFileForRead("FLASH", sdPath, file) || !file) {
    LOG_ERR("FLASH", "open failed: %s", sdPath);
    return Result::OPEN_FAIL;
  }

  const size_t firmwareSize = file.fileSize();
  LOG_INF("FLASH", "src=%s size=%u dest=%s @0x%x partsize=%u", sdPath, static_cast<unsigned>(firmwareSize), dest->label,
          static_cast<unsigned>(dest->address), static_cast<unsigned>(dest->size));

  auto buffer = makeUniqueNoThrow<uint8_t[]>(CHUNK);
  if (!buffer) {
    LOG_ERR("FLASH", "OOM chunk buffer (%u bytes)", static_cast<unsigned>(CHUNK));
    return Result::OOM;
  }

  // Interleave erase + write so the progress bar advances 0→100% smoothly
  // rather than stalling for several seconds during a single up-front erase.
  size_t streamPos = 0;
  size_t erasedUpto = 0;
  while (streamPos < firmwareSize) {
    if (streamPos >= erasedUpto) {
      size_t eraseLen = std::min<size_t>(BLK, dest->size - streamPos);
      eraseLen = (eraseLen + SEC - 1) & ~(SEC - 1);
      eraseLen = std::min<size_t>(eraseLen, dest->size - streamPos);
      if (esp_partition_erase_range(dest, streamPos, eraseLen) != ESP_OK) {
        LOG_ERR("FLASH", "erase @%u (len=%u) failed", static_cast<unsigned>(streamPos),
                static_cast<unsigned>(eraseLen));
        return Result::ERASE_FAIL;
      }
      erasedUpto = streamPos + eraseLen;
    }

    const size_t want = std::min<size_t>(CHUNK, firmwareSize - streamPos);
    const int read = file.read(buffer.get(), want);
    if (read <= 0 || static_cast<size_t>(read) != want) {
      LOG_ERR("FLASH", "read @%u: got=%d want=%u", static_cast<unsigned>(streamPos), read, static_cast<unsigned>(want));
      return Result::READ_FAIL;
    }
    if (esp_partition_write(dest, streamPos, buffer.get(), want) != ESP_OK) {
      LOG_ERR("FLASH", "write @%u failed", static_cast<unsigned>(streamPos));
      return Result::WRITE_FAIL;
    }
    streamPos += want;
    if (onProgress) onProgress(streamPos, firmwareSize, ctx);
    halPlatform.delay(1);
  }
  file.close();

  if (!ota_boot::switchTo(dest)) {
    LOG_ERR("FLASH", "otadata switch failed");
    return Result::OTADATA_FAIL;
  }
  return Result::OK;
}

}  // namespace firmware_flash
