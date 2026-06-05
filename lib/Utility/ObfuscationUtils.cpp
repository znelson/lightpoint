#include "ObfuscationUtils.h"

#include <HalPlatform.h>
#include <Logging.h>
#include <mbedtls/base64.h>

#include <cstring>

namespace obfuscation {

namespace {
constexpr size_t HW_KEY_LEN = 6;

// Simple lazy init — no thread-safety concern on single-core ESP32-C3.
const uint8_t* getHwKey() {
  static uint8_t key[HW_KEY_LEN] = {};
  static bool initialized = false;
  if (!initialized) {
    const uint64_t id = halPlatform.deviceId();
    for (size_t i = 0; i < HW_KEY_LEN; i++) {
      key[i] = static_cast<uint8_t>(id >> (40 - i * 8));
    }
    initialized = true;
  }
  return key;
}
}  // namespace

void xorTransform(std::string& data) {
  const uint8_t* key = getHwKey();
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= key[i % HW_KEY_LEN];
  }
}

std::string obfuscateToBase64(const std::string& plaintext) {
  if (plaintext.empty()) return {};
  std::string temp = plaintext;
  xorTransform(temp);

  // Query required output length
  size_t encodedLen = 0;
  mbedtls_base64_encode(nullptr, 0, &encodedLen, reinterpret_cast<const unsigned char*>(temp.data()), temp.size());
  std::string out(encodedLen, '\0');
  mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), encodedLen, &encodedLen,
                        reinterpret_cast<const unsigned char*>(temp.data()), temp.size());
  out.resize(encodedLen);
  return out;
}

std::string deobfuscateFromBase64(const char* encoded, bool* ok) {
  if (encoded == nullptr || encoded[0] == '\0') {
    if (ok) *ok = false;
    return "";
  }
  if (ok) *ok = true;
  size_t encodedLen = strlen(encoded);
  // First call: get required output buffer size
  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &decodedLen, reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    LOG_ERR("OBF", "Base64 decode size query failed (ret=%d)", ret);
    if (ok) *ok = false;
    return "";
  }
  std::string result(decodedLen, '\0');
  ret = mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&result[0]), decodedLen, &decodedLen,
                              reinterpret_cast<const unsigned char*>(encoded), encodedLen);
  if (ret != 0) {
    LOG_ERR("OBF", "Base64 decode failed (ret=%d)", ret);
    if (ok) *ok = false;
    return "";
  }
  result.resize(decodedLen);
  xorTransform(result);
  return result;
}

void selfTest() {
  const char* testInputs[] = {"", "hello", "WiFi P@ssw0rd!", "a"};
  bool allPassed = true;
  for (const char* input : testInputs) {
    std::string encoded = obfuscateToBase64(std::string(input));
    std::string decoded = deobfuscateFromBase64(encoded.c_str());
    if (decoded != input) {
      LOG_ERR("OBF", "FAIL: \"%s\" -> \"%s\" -> \"%s\"", input, encoded.c_str(), decoded.c_str());
      allPassed = false;
    }
  }
  std::string enc = obfuscateToBase64("test123");
  if (enc == "test123") {
    LOG_ERR("OBF", "FAIL: obfuscated output identical to plaintext");
    allPassed = false;
  }
  if (allPassed) {
    LOG_DBG("OBF", "Obfuscation self-test PASSED");
  }
}

}  // namespace obfuscation
