#pragma once

#include <Typesetter/TextAlign.h>
#include <Typesetter/blocks/BlockStyle.h>

#include "CssStyle.h"

// Factory function to create a BlockStyle from CSS properties.
// Kept in the Epub library since it depends on CssStyle/CssLength types.
inline BlockStyle blockStyleFromCssStyle(const CssStyle& cssStyle, const float emSize,
                                         const CssTextAlign paragraphAlignment, const uint16_t viewportWidth = 0) {
  BlockStyle blockStyle;
  const float vw = viewportWidth;
  const auto maxHorizontalInsetPx = static_cast<int16_t>(emSize * BlockStyle::MAX_HORIZONTAL_INSET_EM);
  // Resolve all CssLength values to pixels using the current font's em size and viewport width
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
  if (paragraphAlignment == CssTextAlign::None) {
    blockStyle.alignment = blockStyle.textAlignDefined ? cssStyle.textAlign : CssTextAlign::Justify;
  } else {
    blockStyle.alignment = paragraphAlignment;
  }
  return blockStyle;
}
