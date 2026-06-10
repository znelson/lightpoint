#pragma once

#include <FunctionRef.h>
#include <Rect.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class GfxRenderer;
struct RecentBook;

struct TabInfo {
  const char* label;
  bool selected;
};

struct ThemeMetrics {
  uint8_t batteryWidth;
  uint8_t batteryHeight;

  uint8_t topPadding;
  uint8_t batteryBarHeight;
  uint8_t headerHeight;
  uint8_t verticalSpacing;

  uint8_t contentSidePadding;
  uint8_t listRowHeight;
  uint8_t listWithSubtitleRowHeight;
  uint8_t menuRowHeight;
  uint8_t menuSpacing;

  uint8_t tabSpacing;
  uint8_t tabBarHeight;

  uint8_t scrollBarWidth;
  int8_t scrollBarRightOffset;

  uint8_t homeTopPadding;
  uint16_t homeCoverHeight;
  uint16_t homeCoverTileHeight;
  uint8_t homeRecentBooksCount;
  bool homeContinueReadingInMenu;
  uint8_t homeMenuTopOffset;

  uint8_t buttonHintsHeight;
  uint8_t sideButtonHintsWidth;

  uint8_t progressBarHeight;
  uint8_t progressBarMarginTop;
  uint8_t statusBarHorizontalMargin;
  uint8_t statusBarVerticalMargin;

  uint8_t keyboardKeyWidth;
  uint8_t keyboardKeyHeight;
  uint8_t keyboardKeySpacing;
  uint8_t keyboardBottomKeyHeight;
  uint8_t keyboardBottomKeySpacing;
  bool keyboardBottomAligned;
  bool keyboardCenteredText;
  int8_t keyboardVerticalOffset;
  uint8_t keyboardTextFieldWidthPercent;
  uint8_t keyboardWidthPercent;
  uint8_t keyboardKeyCornerRadius;
  bool keyboardFillUnselected;
  bool keyboardOutlineAllUnselected;
  bool keyboardDrawSpecialOutlineWhenUnselected;
  uint8_t keyboardSecondaryLabelRightPadding;
  uint8_t keyboardSecondaryLabelTopPadding;
  uint8_t keyboardMinArrowHeadSize;

  float popupTopOffsetRatio;
  uint8_t popupMarginX;
  uint8_t popupMarginY;
  uint8_t popupFrameThickness;
  uint8_t popupCornerRadius;
  bool popupTextBold;
  bool popupTextInverted;
  int8_t popupTextBaselineOffsetY;
  uint8_t popupProgressBarHeight;
  bool popupProgressDrawOutline;
  bool popupProgressClampPercent;
  bool popupProgressFillInverted;
  bool popupProgressOutlineInverted;

  uint8_t textFieldHorizontalPadding;
  uint8_t textFieldNormalThickness;
  uint8_t textFieldCursorThickness;
  int8_t textFieldLineEndOffset;
};

enum UIIcon { None = 0, Folder, Text, Image, Book, File, Recent, Settings, Library, Wifi, Bookmark };

enum class KeyboardKeyType { Normal, Shift, Mode, Space, Del, Ok, Disabled };

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 50,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 10,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 22,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 0,
                                 .keyboardFillUnselected = false,
                                 .keyboardOutlineAllUnselected = false,
                                 .keyboardDrawSpecialOutlineWhenUnselected = true,
                                 .keyboardSecondaryLabelRightPadding = 1,
                                 .keyboardSecondaryLabelTopPadding = 0,
                                 .keyboardMinArrowHeadSize = 0,
                                 .popupTopOffsetRatio = 0.075f,
                                 .popupMarginX = 15,
                                 .popupMarginY = 15,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = true,
                                 .popupTextInverted = true,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = true,
                                 .popupProgressOutlineInverted = true,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Component drawing methods
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect,
                       bool showPercentage = true) const;  // Left aligned (reader mode)
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                        bool showPercentage = true) const;  // Right aligned (UI headers)
  virtual void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4) const;
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  virtual int getListPageItems(int contentHeight, bool hasSubtitle) const;
  virtual void drawList(const GfxRenderer& renderer, Rect rect, uint16_t itemCount,
                        std::optional<uint16_t> selectedIndex, FunctionRef<std::string(int index)> rowTitle,
                        FunctionRef<std::string(int index)> rowSubtitle = nullptr,
                        FunctionRef<UIIcon(int index)> rowIcon = nullptr,
                        FunctionRef<std::string(int index)> rowValue = nullptr, bool highlightValue = false,
                        FunctionRef<bool(int index)> rowDimmed = nullptr) const;
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                          const char* subtitle = nullptr) const;
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                          bool selected) const;
  virtual void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                   uint16_t selectorIndex, bool hasCachedCover, bool bufferRestored,
                                   FunctionRef<bool()> storeCoverBuffer) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, uint16_t buttonCount,
                              std::optional<uint16_t> selectedIndex, FunctionRef<std::string(int index)> buttonLabel,
                              FunctionRef<UIIcon(int index)> rowIcon) const;
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                     std::string title, const int paddingBottom = 0, const int textYOffset = 0,
                     const bool fillMargin = true) const;
  void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual void drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                               const char* secondaryLabel = nullptr, KeyboardKeyType keyType = KeyboardKeyType::Normal,
                               bool inactiveSelection = false) const;
  virtual bool showsFileIcons() const { return false; }

  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  static void drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);
};
