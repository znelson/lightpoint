#pragma once

#include <Typesetter/TextAlign.h>
#include <Typesetter/blocks/BlockStyle.h>

#include <algorithm>

#include "CssStyle.h"

// Construct a layout BlockStyle from a parsed CSS style, resolving CssLength
// values to pixels with the current font em size and viewport width. Lives in
// lib/Epub because it depends on CssLength/CssStyle (CSS-specific types).
// The layout BlockStyle itself lives in lib/Typesetter and stays free of CSS
// concepts.
inline BlockStyle blockStyleFromCssStyle(const CssStyle& cssStyle, const float emSize,
                                         const TextAlign paragraphAlignment, const uint16_t viewportWidth = 0) {
  BlockStyle blockStyle;
  const float vw = viewportWidth;
  const auto maxHorizontalInsetPx = static_cast<int16_t>(emSize * BlockStyle::MAX_HORIZONTAL_INSET_EM);
  blockStyle.marginTop = cssStyle.marginTop.toPixelsInt16(emSize, vw);
  blockStyle.marginBottom = cssStyle.marginBottom.toPixelsInt16(emSize, vw);
  blockStyle.marginLeft = std::min(cssStyle.marginLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
  blockStyle.marginRight = std::min(cssStyle.marginRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

  blockStyle.paddingTop = cssStyle.paddingTop.toPixelsInt16(emSize, vw);
  blockStyle.paddingBottom = cssStyle.paddingBottom.toPixelsInt16(emSize, vw);
  blockStyle.paddingLeft = std::min(cssStyle.paddingLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
  blockStyle.paddingRight = std::min(cssStyle.paddingRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);

  // For textIndent: if it's a percentage we can't resolve (no viewport width),
  // leave textIndentDefined=false so the EmSpace fallback in applyParagraphIndent() is used
  if (cssStyle.hasTextIndent() && cssStyle.textIndent.isResolvable(vw)) {
    blockStyle.textIndent = cssStyle.textIndent.toPixelsInt16(emSize, vw);
    blockStyle.textIndentDefined = true;
  }
  blockStyle.textAlignDefined = cssStyle.hasTextAlign();
  // User setting overrides CSS, unless "Book's Style" alignment setting is selected
  if (paragraphAlignment == TextAlign::None) {
    blockStyle.alignment = blockStyle.textAlignDefined ? cssStyle.textAlign : TextAlign::Justify;
  } else {
    blockStyle.alignment = paragraphAlignment;
  }
  return blockStyle;
}
