#pragma once

#include "core/PixelBuffer.h"

namespace CorridorKey::Core {

enum class InputColorMode {
  HostManaged,
  SrgbRec709Gamma,
  Linear,
};

enum class AlphaState {
  Straight,
  Premultiplied,
};

enum class OutputMode {
  ProcessedRgba,
  Matte,
  StraightForeground,
  AlphaHintView,
  CheckerComp,
  StatusFrame,
};

enum class Premultiplication {
  Opaque,
  Premultiplied,
  Unpremultiplied,
};

float srgbRec709GammaToLinear(float value);

RgbaPixel convertInputToLinear(const RgbaPixel& pixel, InputColorMode mode);

RgbaPixel premultiply(const RgbaPixel& straight);
RgbaPixel unpremultiply(const RgbaPixel& premultiplied);
RgbaPixel toPremultiplied(const RgbaPixel& pixel, AlphaState state);
RgbaPixel toStraight(const RgbaPixel& pixel, AlphaState state);

RgbaPixel processedRgba(const RgbaPixel& straightLinear, float linearAlpha);
RgbaPixel straightForeground(const RgbaPixel& straightLinear, float linearAlpha);
RgbaPixel mattePixel(float linearAlpha);

Premultiplication outputPremultiplication(OutputMode mode);

}  // namespace CorridorKey::Core
