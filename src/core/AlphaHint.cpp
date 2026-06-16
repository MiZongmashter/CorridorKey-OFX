#include "core/AlphaHint.h"

#include "core/PixelConvert.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace CorridorKey::Core {
namespace {

constexpr float kFallbackAlpha = 1.0F;

enum class HintChannel {
  Alpha,
  Red,
};

AlphaHintBuffer makeBuffer(const RectI& bounds, float fillValue) {
  const auto count =
      static_cast<std::size_t>(width(bounds)) * static_cast<std::size_t>(height(bounds));
  return AlphaHintBuffer{bounds, std::vector<float>(count, fillValue)};
}

AlphaHintResult fallbackResult(const RectI& renderWindow,
                               AlphaHintStatus status,
                               FormatStatus formatStatus = FormatStatus::Ok) {
  return AlphaHintResult{
      makeBuffer(renderWindow, kFallbackAlpha),
      status,
      formatStatus,
  };
}

bool canReadChannel(const PixelBufferView& image, HintChannel channel) {
  if (channel == HintChannel::Alpha) {
    return image.format.components == PixelComponents::Alpha ||
           image.format.components == PixelComponents::RGBA;
  }
  return image.format.components == PixelComponents::RGB ||
         image.format.components == PixelComponents::RGBA;
}

int channelOffset(const ImageFormat& format, HintChannel channel) {
  if (format.components == PixelComponents::Alpha) {
    return 0;
  }
  return channel == HintChannel::Alpha ? 3 : 0;
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

FormatStatus alphaInputFormatStatus(PixelDepth depth) {
  switch (depth) {
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

FormatStatus validateHintView(const PixelBufferView& image, HintChannel channel) {
  if (image.data == nullptr) {
    return FormatStatus::MissingData;
  }
  if (!canReadChannel(image, channel)) {
    return FormatStatus::UnsupportedComponents;
  }
  const FormatStatus status = image.format.components == PixelComponents::Alpha
                                  ? alphaInputFormatStatus(image.format.depth)
                                  : inputFormatStatus(image.format);
  if (!isSuccess(status)) {
    return status;
  }
  if (width(image.bounds) > 0 && !rowBytesCanHoldBounds(image)) {
    return FormatStatus::InvalidRowBytes;
  }
  return status;
}

const unsigned char* channelAddress(const PixelBufferView& image,
                                    int x,
                                    int y,
                                    HintChannel channel) {
  if (!contains(image.bounds, x, y)) {
    return nullptr;
  }
  const auto* base = static_cast<const unsigned char*>(image.data);
  return base + static_cast<long long>(y - image.bounds.y1) * image.rowBytes +
         static_cast<long long>(x - image.bounds.x1) * pixelStrideBytes(image.format) +
         static_cast<long long>(channelOffset(image.format, channel)) *
             bytesPerComponent(image.format.depth);
}

float readChannelValue(const unsigned char* address, PixelDepth depth) {
  switch (depth) {
    case PixelDepth::Byte:
      return static_cast<float>(*address) / 255.0F;
    case PixelDepth::Half: {
      std::uint16_t value = 0;
      std::memcpy(&value, address, sizeof(value));
      return halfBitsToFloat(value);
    }
    case PixelDepth::Float: {
      float value = 0.0F;
      std::memcpy(&value, address, sizeof(value));
      return value;
    }
    case PixelDepth::Short:
    case PixelDepth::Unknown:
      return 0.0F;
  }
  return 0.0F;
}

AlphaHintResult extractChannel(const PixelBufferView& image,
                               const RectI& renderWindow,
                               HintChannel channel,
                               AlphaHintStatus successStatus) {
  const FormatStatus viewStatus = validateHintView(image, channel);
  if (!isSuccess(viewStatus)) {
    return fallbackResult(renderWindow, AlphaHintStatus::InvalidHintFallback,
                          viewStatus);
  }

  AlphaHintResult result{
      makeBuffer(renderWindow, 0.0F),
      successStatus,
      viewStatus,
  };

  const int hintWidth = width(renderWindow);
  for (int y = renderWindow.y1; y < renderWindow.y2; ++y) {
    for (int x = renderWindow.x1; x < renderWindow.x2; ++x) {
      const unsigned char* address = channelAddress(image, x, y, channel);
      if (address == nullptr) {
        return fallbackResult(renderWindow, AlphaHintStatus::InvalidHintFallback,
                              FormatStatus::MissingData);
      }

      const float value = readChannelValue(address, image.format.depth);
      const auto offset =
          static_cast<std::size_t>(y - renderWindow.y1) * static_cast<std::size_t>(hintWidth) +
          static_cast<std::size_t>(x - renderWindow.x1);
      result.hint.values[offset] = value;
    }
  }
  return result;
}

}  // namespace

AlphaHintResult resolveAlphaHint(AlphaHintSource sourceMode,
                                 const PixelBufferView& source,
                                 const PixelBufferView* externalHint,
                                 const RectI& renderWindow) {
  switch (sourceMode) {
    case AlphaHintSource::External:
      if (externalHint == nullptr) {
        return fallbackResult(renderWindow, AlphaHintStatus::MissingHintFallback);
      }
      return extractChannel(*externalHint, renderWindow, HintChannel::Alpha,
                            AlphaHintStatus::ExternalPresent);
    case AlphaHintSource::SourceAlpha:
      if (source.format.components != PixelComponents::Alpha &&
          source.format.components != PixelComponents::RGBA) {
        return fallbackResult(renderWindow, AlphaHintStatus::MissingHintFallback);
      }
      return extractChannel(source, renderWindow, HintChannel::Alpha,
                            AlphaHintStatus::SourceAlphaUsed);
    case AlphaHintSource::RedChannel:
      if (externalHint == nullptr) {
        return fallbackResult(renderWindow, AlphaHintStatus::MissingHintFallback);
      }
      return extractChannel(*externalHint, renderWindow, HintChannel::Red,
                            AlphaHintStatus::RedChannelUsed);
    case AlphaHintSource::RoughFallback:
      return fallbackResult(renderWindow, AlphaHintStatus::RoughFallbackUsed);
  }
  return fallbackResult(renderWindow, AlphaHintStatus::MissingHintFallback);
}

float alphaHintValueAt(const AlphaHintBuffer& hint, int x, int y) {
  if (!contains(hint.bounds, x, y)) {
    return 0.0F;
  }
  const int hintWidth = width(hint.bounds);
  const auto offset =
      static_cast<std::size_t>(y - hint.bounds.y1) * static_cast<std::size_t>(hintWidth) +
      static_cast<std::size_t>(x - hint.bounds.x1);
  return hint.values[offset];
}

const char* alphaHintStatusCode(AlphaHintStatus status) {
  switch (status) {
    case AlphaHintStatus::ExternalPresent:
      return "external_present";
    case AlphaHintStatus::SourceAlphaUsed:
      return "source_alpha_used";
    case AlphaHintStatus::RedChannelUsed:
      return "red_channel_used";
    case AlphaHintStatus::RoughFallbackUsed:
      return "rough_fallback_used";
    case AlphaHintStatus::MissingHintFallback:
      return "missing_hint_fallback";
    case AlphaHintStatus::InvalidHintFallback:
      return "invalid_hint_fallback";
  }
  return "missing_hint_fallback";
}

const char* alphaHintStatusLabel(AlphaHintStatus status) {
  switch (status) {
    case AlphaHintStatus::ExternalPresent:
      return "external present";
    case AlphaHintStatus::SourceAlphaUsed:
      return "source alpha used";
    case AlphaHintStatus::RedChannelUsed:
      return "red channel used";
    case AlphaHintStatus::RoughFallbackUsed:
      return "rough fallback used";
    case AlphaHintStatus::MissingHintFallback:
      return "missing hint; rough fallback used";
    case AlphaHintStatus::InvalidHintFallback:
      return "invalid hint; rough fallback used";
  }
  return "missing hint; rough fallback used";
}

bool alphaHintStatusIsWarning(AlphaHintStatus status) {
  return status == AlphaHintStatus::RoughFallbackUsed ||
         status == AlphaHintStatus::MissingHintFallback ||
         status == AlphaHintStatus::InvalidHintFallback;
}

}  // namespace CorridorKey::Core
