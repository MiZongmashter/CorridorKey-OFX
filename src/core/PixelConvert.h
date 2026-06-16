#pragma once

#include "core/PixelBuffer.h"

#include <cstdint>

namespace CorridorKey::Core {

float halfBitsToFloat(std::uint16_t value);
std::uint16_t floatToHalfBits(float value);

FormatStatus readPixel(const PixelBufferView& image, int x, int y, RgbaPixel& pixel);
FormatStatus writePixel(const PixelBufferView& image, int x, int y,
                        const RgbaPixel& pixel);

FormatStatus copyWindow(const PixelBufferView& source,
                        const PixelBufferView& output,
                        const RectI& renderWindow);
FormatStatus fillWindowTransparent(const PixelBufferView& output,
                                   const RectI& renderWindow);

}  // namespace CorridorKey::Core
