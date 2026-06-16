#pragma once

namespace CorridorKey::Core {

enum class PixelComponents {
  Unknown,
  Alpha,
  RGB,
  RGBA,
};

enum class PixelDepth {
  Unknown,
  Byte,
  Short,
  Half,
  Float,
};

enum class Fielding {
  Unknown,
  None,
  Both,
  Lower,
  Upper,
  Single,
  Doubled,
};

enum class FormatStatus {
  Ok,
  ByteFallback,
  UnsupportedComponents,
  UnsupportedPixelDepth,
  UnsupportedFielding,
  MissingData,
  InvalidRowBytes,
};

struct ImageFormat {
  PixelComponents components = PixelComponents::Unknown;
  PixelDepth depth = PixelDepth::Unknown;
};

struct PixelAspectRatio {
  double value = 1.0;
};

PixelComponents parsePixelComponents(const char* components);
PixelDepth parsePixelDepth(const char* depth);
Fielding parseFielding(const char* fielding);

ImageFormat imageFormatFromOfx(const char* components, const char* depth);

int componentCount(PixelComponents components);
int bytesPerComponent(PixelDepth depth);

FormatStatus inputFormatStatus(const ImageFormat& format);
FormatStatus outputFormatStatus(const ImageFormat& format);
FormatStatus renderFieldingStatus(Fielding fielding);

bool isSuccess(FormatStatus status);
const char* statusCode(FormatStatus status);

PixelAspectRatio passThroughPixelAspectRatio(PixelAspectRatio source);

}  // namespace CorridorKey::Core
