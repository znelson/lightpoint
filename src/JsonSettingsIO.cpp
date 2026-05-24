#include "JsonSettingsIO.h"

#include <HalStorage.h>
#include <JsonWriter.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <StreamingJsonParser.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "SettingsList.h"
#include "WifiCredentialStore.h"

namespace {

inline bool keyIs(const char* k, size_t len, std::string_view name) {
  return name.size() == len && memcmp(k, name.data(), len) == 0;
}

inline uint32_t parseUInt(const char* s) { return static_cast<uint32_t>(strtoul(s, nullptr, 10)); }

}  // namespace

// ---- CrossPointState --------------------------------------------------------

namespace {

struct StateLoadCtx {
  CrossPointState* s;
  enum class Field : uint8_t {
    NONE,
    OPEN_EPUB_PATH,
    RECENT_SLEEP_IMAGES,
    RECENT_SLEEP_POS,
    RECENT_SLEEP_FILL,
    READER_ACTIVITY_LOAD_COUNT,
    LAST_SLEEP_FROM_READER,
    SHOW_BOOT_SCREEN,
  };
  Field currentKey = Field::NONE;
  uint8_t depth = 0;
  bool inImagesArray = false;
  uint8_t imagesFilled = 0;
};

void stateOnKey(void* p, const char* k, size_t len) {
  auto* ctx = static_cast<StateLoadCtx*>(p);
  // Only top-level keys map to state fields.
  if (ctx->depth != 1) {
    ctx->currentKey = StateLoadCtx::Field::NONE;
    return;
  }
  using F = StateLoadCtx::Field;
  if (keyIs(k, len, "openEpubPath"))
    ctx->currentKey = F::OPEN_EPUB_PATH;
  else if (keyIs(k, len, "recentSleepImages"))
    ctx->currentKey = F::RECENT_SLEEP_IMAGES;
  else if (keyIs(k, len, "recentSleepPos"))
    ctx->currentKey = F::RECENT_SLEEP_POS;
  else if (keyIs(k, len, "recentSleepFill"))
    ctx->currentKey = F::RECENT_SLEEP_FILL;
  else if (keyIs(k, len, "readerActivityLoadCount"))
    ctx->currentKey = F::READER_ACTIVITY_LOAD_COUNT;
  else if (keyIs(k, len, "lastSleepFromReader"))
    ctx->currentKey = F::LAST_SLEEP_FROM_READER;
  else if (keyIs(k, len, "showBootScreen"))
    ctx->currentKey = F::SHOW_BOOT_SCREEN;
  else
    ctx->currentKey = F::NONE;
}

void stateOnString(void* p, const char* v, size_t len) {
  auto* ctx = static_cast<StateLoadCtx*>(p);
  if (ctx->currentKey == StateLoadCtx::Field::OPEN_EPUB_PATH) {
    ctx->s->openEpubPath.assign(v, len);
  }
  ctx->currentKey = StateLoadCtx::Field::NONE;
}

void stateOnNumber(void* p, const char* v, size_t /*len*/) {
  auto* ctx = static_cast<StateLoadCtx*>(p);
  // Numbers inside the recentSleepImages array fill the buffer up to capacity.
  if (ctx->inImagesArray) {
    if (ctx->imagesFilled < CrossPointState::SLEEP_RECENT_COUNT) {
      ctx->s->recentSleepImages[ctx->imagesFilled++] = static_cast<uint16_t>(parseUInt(v));
    }
    return;
  }
  using F = StateLoadCtx::Field;
  const uint32_t n = parseUInt(v);
  switch (ctx->currentKey) {
    case F::RECENT_SLEEP_POS:
      ctx->s->recentSleepPos = static_cast<uint8_t>(n);
      break;
    case F::RECENT_SLEEP_FILL:
      ctx->s->recentSleepFill = static_cast<uint8_t>(n);
      break;
    case F::READER_ACTIVITY_LOAD_COUNT:
      ctx->s->readerActivityLoadCount = static_cast<uint8_t>(n);
      break;
    default:
      break;
  }
  ctx->currentKey = F::NONE;
}

void stateOnBool(void* p, bool v) {
  auto* ctx = static_cast<StateLoadCtx*>(p);
  using F = StateLoadCtx::Field;
  switch (ctx->currentKey) {
    case F::LAST_SLEEP_FROM_READER:
      ctx->s->lastSleepFromReader = v;
      break;
    case F::SHOW_BOOT_SCREEN:
      ctx->s->showBootScreen = v;
      break;
    default:
      break;
  }
  ctx->currentKey = F::NONE;
}

void stateOnObjectStart(void* p) {
  auto* ctx = static_cast<StateLoadCtx*>(p);
  ++ctx->depth;
}

void stateOnObjectEnd(void* p) {
  auto* ctx = static_cast<StateLoadCtx*>(p);
  if (ctx->depth > 0) --ctx->depth;
  ctx->currentKey = StateLoadCtx::Field::NONE;
}

void stateOnArrayStart(void* p) {
  auto* ctx = static_cast<StateLoadCtx*>(p);
  if (ctx->currentKey == StateLoadCtx::Field::RECENT_SLEEP_IMAGES) {
    ctx->inImagesArray = true;
    ctx->imagesFilled = 0;
  }
  ctx->currentKey = StateLoadCtx::Field::NONE;
}

void stateOnArrayEnd(void* p) {
  auto* ctx = static_cast<StateLoadCtx*>(p);
  ctx->inImagesArray = false;
}

}  // namespace

bool JsonSettingsIO::saveState(const CrossPointState& s, const char* path) {
  std::string json;
  JsonWriter w(json);
  w.beginObject();
  w.key("openEpubPath");
  w.valueString(s.openEpubPath);
  w.key("recentSleepImages");
  w.beginArray();
  for (int i = 0; i < CrossPointState::SLEEP_RECENT_COUNT; ++i) w.valueUInt(s.recentSleepImages[i]);
  w.endArray();
  w.key("recentSleepPos");
  w.valueUInt(s.recentSleepPos);
  w.key("recentSleepFill");
  w.valueUInt(s.recentSleepFill);
  w.key("readerActivityLoadCount");
  w.valueUInt(s.readerActivityLoadCount);
  w.key("lastSleepFromReader");
  w.valueBool(s.lastSleepFromReader);
  w.key("showBootScreen");
  w.valueBool(s.showBootScreen);
  w.endObject();
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadState(CrossPointState& s, const char* json) {
  // Preserve the legacy behavior of zeroing the sleep-image buffer before
  // parsing -- holes in the array stay 0 even if the JSON omits entries.
  memset(s.recentSleepImages, 0, sizeof(s.recentSleepImages));

  StateLoadCtx ctx{&s};
  JsonCallbacks cbs{&ctx,    stateOnKey,         stateOnString,    stateOnNumber,     stateOnBool,
                    nullptr, stateOnObjectStart, stateOnObjectEnd, stateOnArrayStart, stateOnArrayEnd};
  StreamingJsonParser parser(cbs);
  parser.feed(json, strlen(json));
  if (parser.hasError()) {
    LOG_ERR("CPS", "JSON parse error");
    return false;
  }

  // Same clamping as the previous implementation.
  const int actualCount = ctx.imagesFilled;
  if (s.recentSleepPos >= CrossPointState::SLEEP_RECENT_COUNT) {
    s.recentSleepPos = actualCount > 0 ? s.recentSleepPos % CrossPointState::SLEEP_RECENT_COUNT : 0;
  }
  s.recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(s.recentSleepFill), actualCount));
  return true;
}

// ---- CrossPointSettings -----------------------------------------------------

namespace {

// Mirrors the lambda in the previous ArduinoJson-based loader. Returns val
// when it is in [0, maxExcl); otherwise falls back to def. Used for ENUM /
// TOGGLE fields where out-of-range values must not stick.
inline uint8_t inRangeOrDefault(uint8_t val, uint8_t maxExcl, uint8_t def) { return val < maxExcl ? val : def; }

struct SettingsLoadCtx {
  CrossPointSettings* s;
  const std::vector<SettingInfo>* list;  // cached snapshot from getSettingsList()
  enum class Special : uint8_t {
    NONE,
    FRONT_BACK,
    FRONT_CONFIRM,
    FRONT_LEFT,
    FRONT_RIGHT,
    FONT_FAMILY,
    SD_FONT_NAME,
    LANGUAGE,
  };
  // Either a SettingsList match or a special-key match -- never both.
  int settingsListIdx = -1;
  Special special = Special::NONE;
  uint8_t depth = 0;
};

void settingsOnKey(void* p, const char* k, size_t len) {
  auto* ctx = static_cast<SettingsLoadCtx*>(p);
  ctx->settingsListIdx = -1;
  ctx->special = SettingsLoadCtx::Special::NONE;
  if (ctx->depth != 1) return;

  // Special-cased top-level keys: managed outside SettingsList by the front
  // button remap activity, the font picker, and the language picker.
  using Sp = SettingsLoadCtx::Special;
  if (keyIs(k, len, "frontButtonBack")) {
    ctx->special = Sp::FRONT_BACK;
    return;
  }
  if (keyIs(k, len, "frontButtonConfirm")) {
    ctx->special = Sp::FRONT_CONFIRM;
    return;
  }
  if (keyIs(k, len, "frontButtonLeft")) {
    ctx->special = Sp::FRONT_LEFT;
    return;
  }
  if (keyIs(k, len, "frontButtonRight")) {
    ctx->special = Sp::FRONT_RIGHT;
    return;
  }
  if (keyIs(k, len, "fontFamily")) {
    ctx->special = Sp::FONT_FAMILY;
    return;
  }
  if (keyIs(k, len, "sdFontFamilyName")) {
    ctx->special = Sp::SD_FONT_NAME;
    return;
  }
  if (keyIs(k, len, "language")) {
    ctx->special = Sp::LANGUAGE;
    return;
  }

  // SettingsList lookup. The list is ~40 entries; linear search is fine
  // and avoids any per-load heap churn for a map.
  for (size_t i = 0; i < ctx->list->size(); ++i) {
    const auto& info = (*ctx->list)[i];
    if (!info.key) continue;
    if (!info.valuePtr && !info.stringOffset) continue;
    if (keyIs(k, len, info.key)) {
      ctx->settingsListIdx = static_cast<int>(i);
      return;
    }
  }
}

void settingsOnString(void* p, const char* v, size_t len) {
  auto* ctx = static_cast<SettingsLoadCtx*>(p);
  using Sp = SettingsLoadCtx::Special;

  if (ctx->special == Sp::SD_FONT_NAME) {
    // Stored as a fixed char[32] buffer on CrossPointSettings.
    strncpy(ctx->s->sdFontFamilyName, v, sizeof(ctx->s->sdFontFamilyName) - 1);
    ctx->s->sdFontFamilyName[sizeof(ctx->s->sdFontFamilyName) - 1] = '\0';
    ctx->special = Sp::NONE;
    return;
  }
  if (ctx->special == Sp::LANGUAGE) {
    // I18n::languageFromCode expects a null-terminated string. SAX tokens
    // are bounded by len, so copy into a tiny stack buffer first.
    char buf[16];
    const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, v, n);
    buf[n] = '\0';
    ctx->s->language = static_cast<uint8_t>(I18n::languageFromCode(buf));
    ctx->special = Sp::NONE;
    return;
  }

  if (ctx->settingsListIdx >= 0) {
    const auto& info = (*ctx->list)[ctx->settingsListIdx];
    if (info.stringOffset) {
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
      } else {
        char* destPtr = reinterpret_cast<char*>(ctx->s) + info.stringOffset;
        // strncpy copies up to N bytes and zero-pads if source is shorter
        // than N; matches the previous behavior exactly. The token buffer
        // is null-terminated at len, so this is safe.
        strncpy(destPtr, v, info.stringMaxLen - 1);
        destPtr[info.stringMaxLen - 1] = '\0';
      }
    }
  }
  ctx->settingsListIdx = -1;
  ctx->special = Sp::NONE;
}

void settingsOnNumber(void* p, const char* v, size_t /*len*/) {
  auto* ctx = static_cast<SettingsLoadCtx*>(p);
  if (ctx->depth != 1) return;
  using Sp = SettingsLoadCtx::Special;
  using S = CrossPointSettings;
  const uint8_t n = static_cast<uint8_t>(parseUInt(v));

  switch (ctx->special) {
    case Sp::FRONT_BACK:
      ctx->s->frontButtonBack = inRangeOrDefault(n, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
      ctx->special = Sp::NONE;
      return;
    case Sp::FRONT_CONFIRM:
      ctx->s->frontButtonConfirm = inRangeOrDefault(n, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_CONFIRM);
      ctx->special = Sp::NONE;
      return;
    case Sp::FRONT_LEFT:
      ctx->s->frontButtonLeft = inRangeOrDefault(n, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
      ctx->special = Sp::NONE;
      return;
    case Sp::FRONT_RIGHT:
      ctx->s->frontButtonRight = inRangeOrDefault(n, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
      ctx->special = Sp::NONE;
      return;
    case Sp::FONT_FAMILY:
      ctx->s->fontFamily = inRangeOrDefault(n, S::BUILTIN_FONT_COUNT, 0);
      ctx->special = Sp::NONE;
      return;
    default:
      break;
  }

  if (ctx->settingsListIdx >= 0) {
    const auto& info = (*ctx->list)[ctx->settingsListIdx];
    if (info.valuePtr) {
      const uint8_t defaultVal = ctx->s->*(info.valuePtr);
      uint8_t outVal = n;
      switch (info.type) {
        case SettingType::ENUM:
          outVal = inRangeOrDefault(outVal, static_cast<uint8_t>(info.enumValues.size()), defaultVal);
          break;
        case SettingType::TOGGLE:
          outVal = inRangeOrDefault(outVal, 2, defaultVal);
          break;
        case SettingType::VALUE:
          if (outVal < info.valueRange.min)
            outVal = info.valueRange.min;
          else if (outVal > info.valueRange.max)
            outVal = info.valueRange.max;
          break;
        default:
          break;
      }
      ctx->s->*(info.valuePtr) = outVal;
    }
  }
  ctx->settingsListIdx = -1;
  ctx->special = Sp::NONE;
}

void settingsOnBool(void* p, bool v) {
  // No bool settings exist in SettingsList, but tolerate the type defensively
  // for forward-compat (e.g. a future loader writing toggles as bool instead
  // of 0/1). Map to the same handling as a numeric 0/1.
  const char* tmp = v ? "1" : "0";
  settingsOnNumber(p, tmp, 1);
}

void settingsOnObjectStart(void* p) {
  auto* ctx = static_cast<SettingsLoadCtx*>(p);
  ++ctx->depth;
}

void settingsOnObjectEnd(void* p) {
  auto* ctx = static_cast<SettingsLoadCtx*>(p);
  if (ctx->depth > 0) --ctx->depth;
  ctx->settingsListIdx = -1;
  ctx->special = SettingsLoadCtx::Special::NONE;
}

void settingsOnArrayStart(void* p) {
  // No array fields in Settings; reset the pending key so any nested values
  // don't accidentally land in a setting.
  auto* ctx = static_cast<SettingsLoadCtx*>(p);
  ctx->settingsListIdx = -1;
  ctx->special = SettingsLoadCtx::Special::NONE;
}

void settingsOnArrayEnd(void* /*p*/) {}

}  // namespace

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  std::string json;
  JsonWriter w(json);
  w.beginObject();

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (e.g. ACTION rows) are not persisted.
    if (!info.valuePtr && !info.stringOffset) continue;

    w.key(info.key);
    if (info.stringOffset) {
      const char* strPtr = reinterpret_cast<const char*>(&s) + info.stringOffset;
      w.valueString(strPtr);
    } else {
      w.valueUInt(s.*(info.valuePtr));
    }
  }

  // Front button remap -- managed by RemapFrontButtons sub-activity, not in SettingsList.
  w.key("frontButtonBack");
  w.valueUInt(s.frontButtonBack);
  w.key("frontButtonConfirm");
  w.valueUInt(s.frontButtonConfirm);
  w.key("frontButtonLeft");
  w.valueUInt(s.frontButtonLeft);
  w.key("frontButtonRight");
  w.valueUInt(s.frontButtonRight);
  // Font family -- dynamic getter/setter in SettingsList so the generic loop skipped it.
  w.key("fontFamily");
  w.valueUInt(s.fontFamily);
  // SD card font family name -- only emit when non-empty (matches prior behavior).
  if (s.sdFontFamilyName[0] != '\0') {
    w.key("sdFontFamilyName");
    w.valueString(s.sdFontFamilyName);
  }
  // Language stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  w.key("language");
  w.valueString((s.language < getLanguageCount()) ? LANGUAGE_CODES[s.language] : "EN");

  w.endObject();
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadSettings(CrossPointSettings& s, const char* json) {
  const std::vector<SettingInfo> list = getSettingsList();

  SettingsLoadCtx ctx{&s, &list};
  JsonCallbacks cbs{&ctx,    settingsOnKey,         settingsOnString,    settingsOnNumber,     settingsOnBool,
                    nullptr, settingsOnObjectStart, settingsOnObjectEnd, settingsOnArrayStart, settingsOnArrayEnd};
  StreamingJsonParser parser(cbs);
  parser.feed(json, strlen(json));
  if (parser.hasError()) {
    LOG_ERR("CPS", "JSON parse error");
    return false;
  }

  CrossPointSettings::validateFrontButtonMapping(s);
  LOG_DBG("CPS", "Settings loaded from file");
  return true;
}

// ---- WifiCredentialStore ----------------------------------------------------

namespace {

// The SAX callbacks below live in an anonymous namespace and so don't get
// the `friend JsonSettingsIO::loadWifi` access that WifiCredentialStore
// grants. Accumulate results into this local ctx; the friend function
// loadWifi commits them at the end.
//
// Fixed-size array (vs std::vector) for the credentials buffer: cap is
// known at compile time, so we avoid the 3 grow-and-copy reallocs a vector
// would do as it doubles from 0 -> 1 -> 2 -> 4 -> 8.
struct WifiLoadCtx {
  std::string lastSsid;
  std::array<WifiCredential, WifiCredentialStore::MAX_NETWORKS> creds;
  size_t credCount = 0;
  enum class TopKey : uint8_t { NONE, LAST_SSID, CREDENTIALS };
  enum class CredKey : uint8_t { NONE, SSID, PASSWORD_OBF };
  TopKey topKey = TopKey::NONE;
  CredKey credKey = CredKey::NONE;
  uint8_t depth = 0;
  bool inCredentialsArray = false;
  bool inCredentialObject = false;
  WifiCredential current;
};

void wifiOnKey(void* p, const char* k, size_t len) {
  auto* ctx = static_cast<WifiLoadCtx*>(p);
  if (ctx->depth == 1) {
    using T = WifiLoadCtx::TopKey;
    if (keyIs(k, len, "lastConnectedSsid"))
      ctx->topKey = T::LAST_SSID;
    else if (keyIs(k, len, "credentials"))
      ctx->topKey = T::CREDENTIALS;
    else
      ctx->topKey = T::NONE;
  } else if (ctx->inCredentialObject) {
    using C = WifiLoadCtx::CredKey;
    if (keyIs(k, len, "ssid"))
      ctx->credKey = C::SSID;
    else if (keyIs(k, len, "password_obf"))
      ctx->credKey = C::PASSWORD_OBF;
    else
      ctx->credKey = C::NONE;
  }
}

void wifiOnString(void* p, const char* v, size_t len) {
  auto* ctx = static_cast<WifiLoadCtx*>(p);
  if (ctx->depth == 1 && ctx->topKey == WifiLoadCtx::TopKey::LAST_SSID) {
    ctx->lastSsid.assign(v, len);
    ctx->topKey = WifiLoadCtx::TopKey::NONE;
    return;
  }
  if (ctx->inCredentialObject) {
    using C = WifiLoadCtx::CredKey;
    if (ctx->credKey == C::SSID) {
      ctx->current.ssid.assign(v, len);
    } else if (ctx->credKey == C::PASSWORD_OBF) {
      // deobfuscateFromBase64 wants a null-terminated C string; SAX tokens
      // carry their length explicitly, so copy to a stack buffer first.
      // Base64 of an 8-byte+ password fits well under 256 bytes.
      char buf[256];
      const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
      memcpy(buf, v, n);
      buf[n] = '\0';
      ctx->current.password = obfuscation::deobfuscateFromBase64(buf);
    }
    ctx->credKey = C::NONE;
  }
}

void wifiOnObjectStart(void* p) {
  auto* ctx = static_cast<WifiLoadCtx*>(p);
  ++ctx->depth;
  // A nested object inside the credentials array is one credential entry.
  if (ctx->depth == 2 && ctx->inCredentialsArray) {
    ctx->inCredentialObject = true;
    ctx->current = WifiCredential{};
  }
}

void wifiOnObjectEnd(void* p) {
  auto* ctx = static_cast<WifiLoadCtx*>(p);
  if (ctx->inCredentialObject) {
    // Cap silently at MAX_NETWORKS -- matches the previous loader's break.
    if (ctx->credCount < ctx->creds.size()) {
      ctx->creds[ctx->credCount++] = std::move(ctx->current);
    }
    ctx->inCredentialObject = false;
  }
  if (ctx->depth > 0) --ctx->depth;
  ctx->credKey = WifiLoadCtx::CredKey::NONE;
}

void wifiOnArrayStart(void* p) {
  auto* ctx = static_cast<WifiLoadCtx*>(p);
  if (ctx->depth == 1 && ctx->topKey == WifiLoadCtx::TopKey::CREDENTIALS) {
    ctx->inCredentialsArray = true;
  }
  ctx->topKey = WifiLoadCtx::TopKey::NONE;
}

void wifiOnArrayEnd(void* p) {
  auto* ctx = static_cast<WifiLoadCtx*>(p);
  ctx->inCredentialsArray = false;
}

}  // namespace

bool JsonSettingsIO::saveWifi(const WifiCredentialStore& store, const char* path) {
  std::string json;
  JsonWriter w(json);
  w.beginObject();
  w.key("lastConnectedSsid");
  w.valueString(store.getLastConnectedSsid());
  w.key("credentials");
  w.beginArray();
  for (const auto& cred : store.getCredentials()) {
    w.beginObject();
    w.key("ssid");
    w.valueString(cred.ssid);
    w.key("password_obf");
    w.valueString(obfuscation::obfuscateToBase64(cred.password));
    w.endObject();
  }
  w.endArray();
  w.endObject();
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadWifi(WifiCredentialStore& store, const char* json) {
  WifiLoadCtx ctx;
  JsonCallbacks cbs{&ctx,    wifiOnKey,         wifiOnString,    nullptr,          nullptr,
                    nullptr, wifiOnObjectStart, wifiOnObjectEnd, wifiOnArrayStart, wifiOnArrayEnd};
  StreamingJsonParser parser(cbs);
  parser.feed(json, strlen(json));
  if (parser.hasError()) {
    LOG_ERR("WCS", "JSON parse error");
    return false;
  }

  // Friend access: commit the accumulated results into the private members.
  // Move the [0, credCount) prefix into the destination vector; assign()
  // sizes the vector exactly once via std::distance on the iterator pair.
  store.credentials.assign(std::make_move_iterator(ctx.creds.begin()),
                           std::make_move_iterator(ctx.creds.begin() + ctx.credCount));
  store.lastConnectedSsid = std::move(ctx.lastSsid);

  LOG_DBG("WCS", "Loaded %zu WiFi credentials from file", store.credentials.size());
  return true;
}

// ---- RecentBooksStore -------------------------------------------------------

namespace {

constexpr size_t RECENT_BOOKS_MAX = 10;

struct RecentBooksLoadCtx {
  // Accumulate locally; friend function below moves into RecentBooksStore.
  // Fixed-size array (vs std::vector) for the same reason WifiLoadCtx uses
  // one: the cap is known at compile time, so we skip the doubling reallocs
  // a vector would do as it grew from 0 to RECENT_BOOKS_MAX.
  std::array<RecentBook, RECENT_BOOKS_MAX> books;
  size_t bookCount = 0;
  enum class TopKey : uint8_t { NONE, BOOKS };
  enum class BookKey : uint8_t { NONE, PATH, TITLE, AUTHOR, COVER_BMP_PATH };
  TopKey topKey = TopKey::NONE;
  BookKey bookKey = BookKey::NONE;
  uint8_t depth = 0;
  bool inBooksArray = false;
  bool inBookObject = false;
  RecentBook current;
};

void recentOnKey(void* p, const char* k, size_t len) {
  auto* ctx = static_cast<RecentBooksLoadCtx*>(p);
  if (ctx->depth == 1) {
    using T = RecentBooksLoadCtx::TopKey;
    ctx->topKey = keyIs(k, len, "books") ? T::BOOKS : T::NONE;
  } else if (ctx->inBookObject) {
    using B = RecentBooksLoadCtx::BookKey;
    if (keyIs(k, len, "path"))
      ctx->bookKey = B::PATH;
    else if (keyIs(k, len, "title"))
      ctx->bookKey = B::TITLE;
    else if (keyIs(k, len, "author"))
      ctx->bookKey = B::AUTHOR;
    else if (keyIs(k, len, "coverBmpPath"))
      ctx->bookKey = B::COVER_BMP_PATH;
    else
      ctx->bookKey = B::NONE;
  }
}

void recentOnString(void* p, const char* v, size_t len) {
  auto* ctx = static_cast<RecentBooksLoadCtx*>(p);
  if (!ctx->inBookObject) return;
  using B = RecentBooksLoadCtx::BookKey;
  switch (ctx->bookKey) {
    case B::PATH:
      ctx->current.path.assign(v, len);
      break;
    case B::TITLE:
      ctx->current.title.assign(v, len);
      break;
    case B::AUTHOR:
      ctx->current.author.assign(v, len);
      break;
    case B::COVER_BMP_PATH:
      ctx->current.coverBmpPath.assign(v, len);
      break;
    default:
      break;
  }
  ctx->bookKey = B::NONE;
}

void recentOnObjectStart(void* p) {
  auto* ctx = static_cast<RecentBooksLoadCtx*>(p);
  ++ctx->depth;
  if (ctx->depth == 2 && ctx->inBooksArray) {
    ctx->inBookObject = true;
    ctx->current = RecentBook{};
  }
}

void recentOnObjectEnd(void* p) {
  auto* ctx = static_cast<RecentBooksLoadCtx*>(p);
  if (ctx->inBookObject) {
    // Match the previous 10-entry cap (enforced at intake; extras dropped).
    if (ctx->bookCount < ctx->books.size()) {
      ctx->books[ctx->bookCount++] = std::move(ctx->current);
    }
    ctx->inBookObject = false;
  }
  if (ctx->depth > 0) --ctx->depth;
  ctx->bookKey = RecentBooksLoadCtx::BookKey::NONE;
}

void recentOnArrayStart(void* p) {
  auto* ctx = static_cast<RecentBooksLoadCtx*>(p);
  if (ctx->depth == 1 && ctx->topKey == RecentBooksLoadCtx::TopKey::BOOKS) {
    ctx->inBooksArray = true;
  }
  ctx->topKey = RecentBooksLoadCtx::TopKey::NONE;
}

void recentOnArrayEnd(void* p) {
  auto* ctx = static_cast<RecentBooksLoadCtx*>(p);
  ctx->inBooksArray = false;
}

}  // namespace

bool JsonSettingsIO::saveRecentBooks(const RecentBooksStore& store, const char* path) {
  std::string json;
  JsonWriter w(json);
  w.beginObject();
  w.key("books");
  w.beginArray();
  for (const auto& book : store.getBooks()) {
    w.beginObject();
    w.key("path");
    w.valueString(book.path);
    w.key("title");
    w.valueString(book.title);
    w.key("author");
    w.valueString(book.author);
    w.key("coverBmpPath");
    w.valueString(book.coverBmpPath);
    w.endObject();
  }
  w.endArray();
  w.endObject();
  return Storage.writeFile(path, json);
}

bool JsonSettingsIO::loadRecentBooks(RecentBooksStore& store, const char* json) {
  RecentBooksLoadCtx ctx;
  JsonCallbacks cbs{&ctx,
                    recentOnKey,
                    recentOnString,
                    nullptr,
                    nullptr,
                    nullptr,
                    recentOnObjectStart,
                    recentOnObjectEnd,
                    recentOnArrayStart,
                    recentOnArrayEnd};
  StreamingJsonParser parser(cbs);
  parser.feed(json, strlen(json));
  if (parser.hasError()) {
    LOG_ERR("RBS", "JSON parse error");
    return false;
  }

  // Friend access: commit the [0, bookCount) prefix into the destination
  // vector. assign() sizes the vector exactly once from the iterator pair.
  store.recentBooks.assign(std::make_move_iterator(ctx.books.begin()),
                           std::make_move_iterator(ctx.books.begin() + ctx.bookCount));
  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", store.getCount());
  return true;
}
