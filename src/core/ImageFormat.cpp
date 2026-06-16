#include "core/ImageFormat.h"

#include <cstring>

namespace CorridorKey::Core {
namespace {

bool equals(const char* actual, const char* expected) {
  return actual != nullptr && std::strcmp(actual, expected) == 0;
}

bool isSupportedComponents(PixelComponents components) {
  return components == PixelComponents::RGB || components == PixelComponents::RGBA;
}

}  // namespace

PixelComponents parsePixelComponents(const char* components) {
  if (equals(components, "OfxImageComponentAlpha")) {
    return PixelComponents::Alpha;
  }
  if (equals(components, "OfxImageComponentRGB")) {
    return PixelComponents::RGB;
  }
  if (equals(components, "OfxImageComponentRGBA")) {
    return PixelComponents::RGBA;
  }
  return PixelComponents::Unknown;
}

PixelDepth parsePixelDepth(const char* depth) {
  if (equals(depth, "OfxBitDepthByte")) {
    return PixelDepth::Byte;
  }
  if (equals(depth, "OfxBitDepthShort")) {
    return PixelDepth::Short;
  }
  if (equals(depth, "OfxBitDepthHalf")) {
    return PixelDepth::Half;
  }
  if (equals(depth, "OfxBitDepthFloat")) {
    return PixelDepth::Float;
  }
  return PixelDepth::Unknown;
}

Fielding parseFielding(const char* fielding) {
  if (equals(fielding, "OfxFieldNone")) {
    return Fielding::None;
  }
  if (equals(fielding, "OfxFieldBoth")) {
    return Fielding::Both;
  }
  if (equals(fielding, "OfxFieldLower")) {
    return Fielding::Lower;
  }
  if (equals(fielding, "OfxFieldUpper")) {
    return Fielding::Upper;
  }
  if (equals(fielding, "OfxFieldSingle")) {
    return Fielding::Single;
  }
  if (equals(fielding, "OfxFieldDoubled")) {
    return Fielding::Doubled;
  }
  return Fielding::Unknown;
}

ImageFormat imageFormatFromOfx(const char* components, const char* depth) {
  return ImageFormat{parsePixelComponents(components), parsePixelDepth(depth)};
}

int componentCount(PixelComponents components) {
  switch (components) {
    case PixelComponents::Alpha:
      return 1;
    case PixelComponents::RGB:
      return 3;
    case PixelComponents::RGBA:
      return 4;
    case PixelComponents::Unknown:
      return 0;
  }
  return 0;
}

int bytesPerComponent(PixelDepth depth) {
  switch (depth) {
    case PixelDepth::Byte:
      return 1;
    case PixelDepth::Short:
    case PixelDepth::Half:
      return 2;
    case PixelDepth::Float:
      return 4;
    case PixelDepth::Unknown:
      return 0;
  }
  return 0;
}

FormatStatus inputFormatStatus(const ImageFormat& format) {
  if (!isSupportedComponents(format.components)) {
    return FormatStatus::UnsupportedComponents;
  }
  switch (format.depth) {
    case PixelDepth::Byte:
      return FormatStatus::ByteFallback;
    case PixelDepth::Half:
    case PixelDepth::Float:
      return FormatStatus::Ok;
    case PixelDepth::Short:
    case PixelDepth::Unknown:
      return FormatStatus::UnsupportedPixelDepth;
  }
  return FormatStatus::UnsupportedPixelDepth;
}

FormatStatus outputFormatStatus(const ImageFormat& format) {
  if (!isSupportedComponents(format.components)) {
    return FormatStatus::UnsupportedComponents;
  }
  switch (format.depth) {
    case PixelDepth::Half:
    case PixelDepth::Float:
      return FormatStatus::Ok;
    case PixelDepth::Byte:
    case PixelDepth::Short:
    case PixelDepth::Unknown:
      return FormatStatus::UnsupportedPixelDepth;
  }
  return FormatStatus::UnsupportedPixelDepth;
}

FormatStatus renderFieldingStatus(Fielding fielding) {
  return fielding == Fielding::None ? FormatStatus::Ok
                                    : FormatStatus::UnsupportedFielding;
}

bool isSuccess(FormatStatus status) {
  return status == FormatStatus::Ok || status == FormatStatus::ByteFallback;
}

const char* statusCode(FormatStatus status) {
  switch (status) {
    case FormatStatus::Ok:
      return "ok";
    case FormatStatus::ByteFallback:
      return "byte_fallback";
    case FormatStatus::UnsupportedComponents:
      return "unsupported_components";
    case FormatStatus::UnsupportedPixelDepth:
      return "unsupported_pixel_depth";
    case FormatStatus::UnsupportedFielding:
      return "unsupported_fielding";
    case FormatStatus::MissingData:
      return "missing_data";
    case FormatStatus::InvalidRowBytes:
      return "invalid_row_bytes";
  }
  return "unknown";
}

PixelAspectRatio passThroughPixelAspectRatio(PixelAspectRatio source) {
  return source;
}

}  // namespace CorridorKey::Core
