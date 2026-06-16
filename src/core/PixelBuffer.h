#pragma once

#include "core/ImageFormat.h"
#include "core/RenderWindow.h"

namespace CorridorKey::Core {

struct PixelBufferView {
  void* data = nullptr;
  RectI bounds{};
  int rowBytes = 0;
  ImageFormat format{};
};

struct RgbaPixel {
  float r = 0.0F;
  float g = 0.0F;
  float b = 0.0F;
  float a = 0.0F;
};

}  // namespace CorridorKey::Core
