#pragma once

class CrossPointSettings;
class CrossPointState;
class HalFile;
class WifiCredentialStore;
class RecentBooksStore;

namespace JsonSettingsIO {

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
bool loadSettingsFromFile(CrossPointSettings& s, HalFile& file);

// CrossPointState
bool saveState(const CrossPointState& s, const char* path);
bool loadStateFromFile(CrossPointState& s, HalFile& file);

// WifiCredentialStore
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifiFromFile(WifiCredentialStore& store, HalFile& file);

// RecentBooksStore
bool saveRecentBooks(const RecentBooksStore& store, const char* path);
bool loadRecentBooksFromFile(RecentBooksStore& store, HalFile& file);

}  // namespace JsonSettingsIO
