#pragma once

#include <cstdint>

// Text alignment for a block of content. Format-agnostic; consumed by the
// layout BlockStyle and produced by any parser (CSS, Markdown, ...).
// Numeric values match the order of PARAGRAPH_ALIGNMENT in CrossPointSettings.
enum class TextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4 };
