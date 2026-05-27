// Stubs for Block subclasses referenced transitively by Page.cpp's
// deserialize / serialize switch. SectionTest links the real Page.cpp
// (so round-trip is honest) but never constructs a TextBlock or
// ImageBlock -- only PageHorizontalRule -- so these bodies exist purely
// to satisfy the linker. Calling any of them at runtime would indicate
// a test using the wrong block type.

#include <Typesetter/blocks/ImageBlock.h>
#include <Typesetter/blocks/TextBlock.h>

// --- TextBlock ------------------------------------------------------------

void TextBlock::render(const GfxRenderer&, int, int, int) const {}
bool TextBlock::serialize(HalFile&) const { return true; }
std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile&) { return nullptr; }

// --- ImageBlock -----------------------------------------------------------

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}
bool ImageBlock::imageExists() const { return true; }
void ImageBlock::render(GfxRenderer&, int, int) {}
bool ImageBlock::serialize(HalFile&) { return true; }
std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile&) { return nullptr; }
