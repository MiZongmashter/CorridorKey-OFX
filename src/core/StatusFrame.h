#pragma once

#include "core/PixelBuffer.h"

#include <string>
#include <vector>

namespace CorridorKey::Core {

enum class StatusFrameSeverity {
  Status,
  Error,
};

struct StatusFrameLine {
  std::string label;
  std::string value;
};

struct StatusFrameContent {
  StatusFrameSeverity severity = StatusFrameSeverity::Status;
  std::string title;
  std::string message;
  std::vector<StatusFrameLine> lines;
};

FormatStatus renderStatusFrame(const PixelBufferView& output,
                               const RectI& renderWindow,
                               const StatusFrameContent& content);

}  // namespace CorridorKey::Core
