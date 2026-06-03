// Stub implementations of non-inline virtual methods for Page/PageElement
// subclasses and ImageBlock. The Typesetter test constructs PageImage,
// ImageBlock, and (via real ParsedText layout) PageLine/TextBlock instances;
// the linker needs definitions for their vtables. These stubs satisfy the
// linker without dragging in the production rendering pipeline
// (lib/ImageDecoder, FsHelpers, etc.).
//
// None of these methods are invoked by the tests -- they exist only so the
// vtables resolve. The real ParsedText.cpp + TextBlock.cpp ARE linked into
// the test (so layout runs end-to-end); their virtual methods that depend on
// the rendering pipeline (TextBlock::render, ImageBlock::render) are
// overridden here as no-ops via the test build's link order.

#include <Typesetter/Page.h>
#include <Typesetter/blocks/ImageBlock.h>
#include <Typesetter/blocks/TextBlock.h>

// --- PageLine -------------------------------------------------------------

void PageLine::render(GfxRenderer&, int, int, int) {}
bool PageLine::serialize(HalFile&) { return true; }
std::unique_ptr<PageLine> PageLine::deserialize(HalFile&) { return nullptr; }

// --- PageImage ------------------------------------------------------------

void PageImage::render(GfxRenderer&, int, int, int) {}
bool PageImage::serialize(HalFile&) { return true; }
std::unique_ptr<PageImage> PageImage::deserialize(HalFile&) { return nullptr; }

// --- PageHorizontalRule ---------------------------------------------------

void PageHorizontalRule::render(GfxRenderer&, int, int, int) {}
bool PageHorizontalRule::serialize(HalFile&) { return true; }
std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(HalFile&) { return nullptr; }

// --- Page -----------------------------------------------------------------

void Page::render(GfxRenderer&, int, int, int) const {}
bool Page::serialize(HalFile&) const { return true; }
std::unique_ptr<Page> Page::deserialize(HalFile&) { return nullptr; }

// --- ImageBlock -----------------------------------------------------------

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}
bool ImageBlock::imageExists() const { return true; }
void ImageBlock::render(GfxRenderer&, int, int) {}
bool ImageBlock::serialize(HalFile&) { return true; }
std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile&) { return nullptr; }
