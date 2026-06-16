#include "core/ColorAlphaContract.h"

#include <algorithm>
#include <cmath>

namespace CorridorKey::Core {
namespace {

float finiteOrZero(float value) {
  return std::isfinite(value) ? value : 0.0F;
}

float safeAlpha(float value) {
  return std::clamp(finiteOrZero(value), 0.0F, 1.0F);
}

RgbaPixel transparentBlack() {
  return RgbaPixel{0.0F, 0.0F, 0.0F, 0.0F};
}

RgbaPixel withAlpha(const RgbaPixel& pixel, float alpha) {
  return RgbaPixel{finiteOrZero(pixel.r), finiteOrZero(pixel.g),
                   finiteOrZero(pixel.b), safeAlpha(alpha)};
}

}  // namespace

float srgbRec709GammaToLinear(float value) {
  const float encoded = finiteOrZero(value);
  if (encoded <= 0.0F) {
    return 0.0F;
  }
  if (encoded <= 0.04045F) {
    return encoded / 12.92F;
  }
  return std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

RgbaPixel convertInputToLinear(const RgbaPixel& pixel, InputColorMode mode) {
  switch (mode) {
    case InputColorMode::HostManaged:
    case InputColorMode::Linear:
      return withAlpha(pixel, pixel.a);
    case InputColorMode::SrgbRec709Gamma:
      return RgbaPixel{
          srgbRec709GammaToLinear(pixel.r),
          srgbRec709GammaToLinear(pixel.g),
          srgbRec709GammaToLinear(pixel.b),
          safeAlpha(pixel.a),
      };
  }
  return withAlpha(pixel, pixel.a);
}

RgbaPixel premultiply(const RgbaPixel& straight) {
  const float alpha = safeAlpha(straight.a);
  if (alpha == 0.0F) {
    return transparentBlack();
  }
  return RgbaPixel{finiteOrZero(straight.r) * alpha,
                   finiteOrZero(straight.g) * alpha,
                   finiteOrZero(straight.b) * alpha, alpha};
}

RgbaPixel unpremultiply(const RgbaPixel& premultiplied) {
  const float alpha = safeAlpha(premultiplied.a);
  if (alpha == 0.0F) {
    return transparentBlack();
  }
  return RgbaPixel{finiteOrZero(premultiplied.r) / alpha,
                   finiteOrZero(premultiplied.g) / alpha,
                   finiteOrZero(premultiplied.b) / alpha, alpha};
}

RgbaPixel toPremultiplied(const RgbaPixel& pixel, AlphaState state) {
  if (state == AlphaState::Premultiplied) {
    const RgbaPixel sanitized = withAlpha(pixel, pixel.a);
    return sanitized.a == 0.0F ? transparentBlack() : sanitized;
  }
  return premultiply(pixel);
}

RgbaPixel toStraight(const RgbaPixel& pixel, AlphaState state) {
  if (state == AlphaState::Premultiplied) {
    return unpremultiply(pixel);
  }
  return withAlpha(pixel, pixel.a);
}

RgbaPixel processedRgba(const RgbaPixel& straightLinear, float linearAlpha) {
  return premultiply(withAlpha(straightLinear, linearAlpha));
}

RgbaPixel straightForeground(const RgbaPixel& straightLinear, float linearAlpha) {
  return withAlpha(straightLinear, linearAlpha);
}

RgbaPixel mattePixel(float linearAlpha) {
  const float alpha = safeAlpha(linearAlpha);
  return RgbaPixel{alpha, alpha, alpha, alpha};
}

Premultiplication outputPremultiplication(OutputMode mode) {
  switch (mode) {
    case OutputMode::ProcessedRgba:
      return Premultiplication::Premultiplied;
    case OutputMode::Matte:
    case OutputMode::StraightForeground:
    case OutputMode::AlphaHintView:
      return Premultiplication::Unpremultiplied;
    case OutputMode::CheckerComp:
    case OutputMode::StatusFrame:
      return Premultiplication::Opaque;
  }
  return Premultiplication::Premultiplied;
}

}  // namespace CorridorKey::Core
