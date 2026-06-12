#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

class GfxRenderer;

class ImageBlock final {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;
};
