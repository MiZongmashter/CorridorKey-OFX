#include "core/PixelConvert.h"

#include <cstring>
#include <limits>

namespace CorridorKey::Core {
namespace {

FormatStatus mergeStatus(FormatStatus current, FormatStatus next) {
  if (!isSuccess(current)) {
    return current;
  }
  if (!isSuccess(next)) {
    return next;
  }
  return current == FormatStatus::ByteFallback || next == FormatStatus::ByteFallback
             ? FormatStatus::ByteFallback
             : FormatStatus::Ok;
}

int pixelStrideBytes(const ImageFormat& format) {
  return componentCount(format.components) * bytesPerComponent(format.depth);
}

bool rowBytesCanHoldBounds(const PixelBufferView& image) {
  if (image.rowBytes == 0) {
    return false;
  }
  const auto minimumRowBytes = static_cast<long long>(width(image.bounds)) *
                               static_cast<long long>(pixelStrideBytes(image.format));
  const long long rowBytes = image.rowBytes;
  const long long rowByteMagnitude = rowBytes < 0 ? -rowBytes : rowBytes;
  return minimumRowBytes <= std::numeric_limits<int>::max() &&
         rowByteMagnitude >= minimumRowBytes;
}

std::uint32_t roundedShiftRightToEven(std::uint32_t value, int shift) {
  if (shift <= 0) {
    return value;
  }
  const std::uint32_t shifted = value >> shift;
  const std::uint32_t remainderMask = (1U << shift) - 1U;
  const std::uint32_t remainder = value & remainderMask;
  const std::uint32_t halfway = 1U << (shift - 1);
  return shifted + (remainder > halfway ||
                            (remainder == halfway && (shifted & 1U))
                        ? 1U
                        : 0U);
}

FormatStatus validateInputView(const PixelBufferView& image) {
  if (image.data == nullptr) {
    return FormatStatus::MissingData;
  }
  const FormatStatus status = inputFormatStatus(image.format);
  if (!isSuccess(status)) {
    return status;
  }
  if (width(image.bounds) > 0 && !rowBytesCanHoldBounds(image)) {
    return FormatStatus::InvalidRowBytes;
  }
  return status;
}

FormatStatus validateOutputView(const PixelBufferView& image) {
  if (image.data == nullptr) {
    return FormatStatus::MissingData;
  }
  const FormatStatus status = outputFormatStatus(image.format);
  if (!isSuccess(status)) {
    return status;
  }
  if (width(image.bounds) > 0 && !rowBytesCanHoldBounds(image)) {
    return FormatStatus::InvalidRowBytes;
  }
  return status;
}

unsigned char* pixelAddress(const PixelBufferView& image, int x, int y) {
  if (!contains(image.bounds, x, y)) {
    return nullptr;
  }
  auto* base = static_cast<unsigned char*>(image.data);
  return base + static_cast<long long>(y - image.bounds.y1) * image.rowBytes +
         static_cast<long long>(x - image.bounds.x1) * pixelStrideBytes(image.format);
}

float readComponent(const unsigned char* address, PixelDepth depth, int component) {
  const int offset = component * bytesPerComponent(depth);
  switch (depth) {
    case PixelDepth::Byte:
      return static_cast<float>(address[component]) / 255.0F;
    case PixelDepth::Half: {
      std::uint16_t value = 0;
      std::memcpy(&value, address + offset, sizeof(value));
      return halfBitsToFloat(value);
    }
    case PixelDepth::Float: {
      float value = 0.0F;
      std::memcpy(&value, address + offset, sizeof(value));
      return value;
    }
    case PixelDepth::Short:
    case PixelDepth::Unknown:
      return 0.0F;
  }
  return 0.0F;
}

void writeComponent(unsigned char* address, PixelDepth depth, int component, float value) {
  const int offset = component * bytesPerComponent(depth);
  switch (depth) {
    case PixelDepth::Half: {
      const std::uint16_t bits = floatToHalfBits(value);
      std::memcpy(address + offset, &bits, sizeof(bits));
      return;
    }
    case PixelDepth::Float: {
      std::memcpy(address + offset, &value, sizeof(value));
      return;
    }
    case PixelDepth::Byte:
    case PixelDepth::Short:
    case PixelDepth::Unknown:
      return;
  }
}

RgbaPixel transparentBlack() {
  return RgbaPixel{0.0F, 0.0F, 0.0F, 0.0F};
}

}  // namespace

float halfBitsToFloat(std::uint16_t value) {
  const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000U) << 16U;
  int exponent = static_cast<int>((value >> 10U) & 0x1FU);
  std::uint32_t mantissa = value & 0x03FFU;

  std::uint32_t bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      while ((mantissa & 0x0400U) == 0) {
        mantissa <<= 1U;
        --exponent;
      }
      ++exponent;
      mantissa &= ~0x0400U;
      bits = sign | (static_cast<std::uint32_t>(exponent + 112) << 23U) |
             (mantissa << 13U);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000U | (mantissa << 13U);
  } else {
    bits = sign | (static_cast<std::uint32_t>(exponent + 112) << 23U) |
           (mantissa << 13U);
  }

  float result = 0.0F;
  std::memcpy(&result, &bits, sizeof(result));
  return result;
}

std::uint16_t floatToHalfBits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));

  const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
  const std::uint32_t rawExponent = (bits >> 23U) & 0xFFU;
  int exponent = static_cast<int>(rawExponent) - 127 + 15;
  std::uint32_t mantissa = bits & 0x007FFFFFU;

  if (rawExponent == 0xFFU) {
    if (mantissa == 0) {
      return static_cast<std::uint16_t>(sign | 0x7C00U);
    }
    std::uint16_t payload = static_cast<std::uint16_t>(mantissa >> 13U);
    if (payload == 0) {
      payload = 1;
    }
    return static_cast<std::uint16_t>(sign | 0x7C00U | payload);
  }

  if (exponent <= 0) {
    if (exponent < -10) {
      return sign;
    }
    mantissa |= 0x00800000U;
    const int shift = 14 - exponent;
    return static_cast<std::uint16_t>(
        sign | static_cast<std::uint16_t>(roundedShiftRightToEven(mantissa, shift)));
  }
  if (exponent >= 31) {
    return static_cast<std::uint16_t>(sign | 0x7C00U);
  }

  mantissa = roundedShiftRightToEven(mantissa, 13) << 13U;
  if (mantissa == 0x00800000U) {
    mantissa = 0;
    ++exponent;
    if (exponent >= 31) {
      return static_cast<std::uint16_t>(sign | 0x7C00U);
    }
  }

  return static_cast<std::uint16_t>(
      sign | static_cast<std::uint16_t>(exponent << 10U) |
      static_cast<std::uint16_t>(mantissa >> 13U));
}

FormatStatus readPixel(const PixelBufferView& image, int x, int y, RgbaPixel& pixel) {
  const FormatStatus status = validateInputView(image);
  if (!isSuccess(status)) {
    return status;
  }

  const unsigned char* address = pixelAddress(image, x, y);
  if (address == nullptr) {
    return FormatStatus::MissingData;
  }

  pixel.r = readComponent(address, image.format.depth, 0);
  pixel.g = readComponent(address, image.format.depth, 1);
  pixel.b = readComponent(address, image.format.depth, 2);
  pixel.a = image.format.components == PixelComponents::RGBA
                ? readComponent(address, image.format.depth, 3)
                : 1.0F;
  return status;
}

FormatStatus writePixel(const PixelBufferView& image, int x, int y,
                        const RgbaPixel& pixel) {
  const FormatStatus status = validateOutputView(image);
  if (!isSuccess(status)) {
    return status;
  }

  unsigned char* address = pixelAddress(image, x, y);
  if (address == nullptr) {
    return FormatStatus::MissingData;
  }

  writeComponent(address, image.format.depth, 0, pixel.r);
  writeComponent(address, image.format.depth, 1, pixel.g);
  writeComponent(address, image.format.depth, 2, pixel.b);
  if (image.format.components == PixelComponents::RGBA) {
    writeComponent(address, image.format.depth, 3, pixel.a);
  }
  return status;
}

FormatStatus copyWindow(const PixelBufferView& source,
                        const PixelBufferView& output,
                        const RectI& renderWindow) {
  FormatStatus status = validateInputView(source);
  if (!isSuccess(status)) {
    return status;
  }
  const FormatStatus outputStatus = validateOutputView(output);
  if (!isSuccess(outputStatus)) {
    return outputStatus;
  }
  status = mergeStatus(status, outputStatus);

  const RectI writeWindow = clipToBounds(renderWindow, output.bounds);
  if (isEmpty(writeWindow)) {
    return status;
  }

  for (int y = writeWindow.y1; y < writeWindow.y2; ++y) {
    for (int x = writeWindow.x1; x < writeWindow.x2; ++x) {
      RgbaPixel pixel = transparentBlack();
      if (contains(source.bounds, x, y)) {
        const FormatStatus readStatus = readPixel(source, x, y, pixel);
        if (!isSuccess(readStatus)) {
          return readStatus;
        }
        status = mergeStatus(status, readStatus);
      }
      const FormatStatus writeStatus = writePixel(output, x, y, pixel);
      if (!isSuccess(writeStatus)) {
        return writeStatus;
      }
      status = mergeStatus(status, writeStatus);
    }
  }

  return status;
}

FormatStatus fillWindowTransparent(const PixelBufferView& output,
                                   const RectI& renderWindow) {
  FormatStatus status = validateOutputView(output);
  if (!isSuccess(status)) {
    return status;
  }

  const RectI writeWindow = clipToBounds(renderWindow, output.bounds);
  for (int y = writeWindow.y1; y < writeWindow.y2; ++y) {
    for (int x = writeWindow.x1; x < writeWindow.x2; ++x) {
      const FormatStatus writeStatus = writePixel(output, x, y, transparentBlack());
      if (!isSuccess(writeStatus)) {
        return writeStatus;
      }
      status = mergeStatus(status, writeStatus);
    }
  }
  return status;
}

}  // namespace CorridorKey::Core
