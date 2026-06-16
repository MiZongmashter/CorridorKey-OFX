#pragma once

#include "core/PixelBuffer.h"

#include <vector>

namespace CorridorKey::Core {

enum class AlphaHintSource {
  External,
  SourceAlpha,
  RedChannel,
  RoughFallback,
};

enum class AlphaHintStatus {
  ExternalPresent,
  SourceAlphaUsed,
  RedChannelUsed,
  RoughFallbackUsed,
  MissingHintFallback,
  InvalidHintFallback,
};

struct AlphaHintBuffer {
  RectI bounds{};
  std::vector<float> values;
};

struct AlphaHintResult {
  AlphaHintBuffer hint{};
  AlphaHintStatus status = AlphaHintStatus::MissingHintFallback;
  FormatStatus formatStatus = FormatStatus::Ok;
};

AlphaHintResult resolveAlphaHint(AlphaHintSource sourceMode,
                                 const PixelBufferView& source,
                                 const PixelBufferView* externalHint,
                                 const RectI& renderWindow);

float alphaHintValueAt(const AlphaHintBuffer& hint, int x, int y);

const char* alphaHintStatusCode(AlphaHintStatus status);
const char* alphaHintStatusLabel(AlphaHintStatus status);
bool alphaHintStatusIsWarning(AlphaHintStatus status);

}  // namespace CorridorKey::Core
