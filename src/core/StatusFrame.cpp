#include "core/StatusFrame.h"

#include "core/PixelConvert.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace CorridorKey::Core {
namespace {

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kGlyphSpacing = 1;

using Glyph = std::array<unsigned char, kGlyphHeight>;

Glyph glyphFor(char input) {
  const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(input)));
  switch (c) {
    case 'A':
      return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B':
      return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C':
      return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    case 'D':
      return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    case 'E':
      return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'F':
      return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    case 'G':
      return {0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0F};
    case 'H':
      return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I':
      return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    case 'J':
      return {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
    case 'K':
      return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L':
      return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    case 'M':
      return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N':
      return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O':
      return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P':
      return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'Q':
      return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    case 'R':
      return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S':
      return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T':
      return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U':
      return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'V':
      return {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
    case 'W':
      return {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
    case 'X':
      return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    case 'Y':
      return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    case 'Z':
      return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    case '0':
      return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    case '1':
      return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case '2':
      return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    case '3':
      return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    case '4':
      return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    case '5':
      return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    case '6':
      return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    case '7':
      return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8':
      return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    case '9':
      return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
    case ':':
      return {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
    case '.':
      return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    case '-':
      return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    case '_':
      return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};
    case '/':
      return {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
    case ' ':
      return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    default:
      return {0x1F, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
  }
}

std::string printable(std::string_view text, std::size_t maxLength) {
  std::string result;
  result.reserve(std::min(text.size(), maxLength));
  for (const unsigned char c : text) {
    if (result.size() >= maxLength) {
      break;
    }
    result.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
  }
  return result;
}

FormatStatus fillRect(const PixelBufferView& output,
                      const RectI& rect,
                      const RgbaPixel& color) {
  FormatStatus status = FormatStatus::Ok;
  for (int y = rect.y1; y < rect.y2; ++y) {
    for (int x = rect.x1; x < rect.x2; ++x) {
      const FormatStatus writeStatus = writePixel(output, x, y, color);
      if (!isSuccess(writeStatus)) {
        return writeStatus;
      }
      if (writeStatus == FormatStatus::ByteFallback) {
        status = FormatStatus::ByteFallback;
      }
    }
  }
  return status;
}

FormatStatus drawGlyph(const PixelBufferView& output,
                       int originX,
                       int originY,
                       int scale,
                       char c,
                       const RgbaPixel& color,
                       const RectI& clip) {
  const Glyph glyph = glyphFor(c);
  for (int row = 0; row < kGlyphHeight; ++row) {
    for (int column = 0; column < kGlyphWidth; ++column) {
      if ((glyph[static_cast<std::size_t>(row)] &
           (1U << static_cast<unsigned int>(kGlyphWidth - 1 - column))) == 0) {
        continue;
      }
      const RectI pixelRect{
          originX + column * scale,
          originY + row * scale,
          originX + (column + 1) * scale,
          originY + (row + 1) * scale,
      };
      const RectI clipped = clipToBounds(pixelRect, clip);
      if (isEmpty(clipped)) {
        continue;
      }
      const FormatStatus status = fillRect(output, clipped, color);
      if (!isSuccess(status)) {
        return status;
      }
    }
  }
  return FormatStatus::Ok;
}

FormatStatus drawText(const PixelBufferView& output,
                      int x,
                      int y,
                      int scale,
                      std::string_view text,
                      const RgbaPixel& color,
                      const RectI& clip) {
  const int advance = (kGlyphWidth + kGlyphSpacing) * scale;
  int cursor = x;
  for (const char c : text) {
    if (cursor + kGlyphWidth * scale > clip.x2) {
      break;
    }
    const FormatStatus status = drawGlyph(output, cursor, y, scale, c, color, clip);
    if (!isSuccess(status)) {
      return status;
    }
    cursor += advance;
  }
  return FormatStatus::Ok;
}

int chooseScale(const RectI& window) {
  return width(window) >= 220 && height(window) >= 110 ? 2 : 1;
}

}  // namespace

FormatStatus renderStatusFrame(const PixelBufferView& output,
                               const RectI& renderWindow,
                               const StatusFrameContent& content) {
  const FormatStatus outputStatus = outputFormatStatus(output.format);
  if (!isSuccess(outputStatus)) {
    return outputStatus;
  }
  if (output.data == nullptr) {
    return FormatStatus::MissingData;
  }

  const RectI window = clipToBounds(renderWindow, output.bounds);
  if (isEmpty(window)) {
    return outputStatus;
  }

  const bool isError = content.severity == StatusFrameSeverity::Error;
  const RgbaPixel background =
      isError ? RgbaPixel{0.16F, 0.025F, 0.035F, 1.0F}
              : RgbaPixel{0.035F, 0.07F, 0.105F, 1.0F};
  const RgbaPixel band =
      isError ? RgbaPixel{0.32F, 0.055F, 0.065F, 1.0F}
              : RgbaPixel{0.055F, 0.14F, 0.20F, 1.0F};
  const RgbaPixel accent =
      isError ? RgbaPixel{1.0F, 0.28F, 0.22F, 1.0F}
              : RgbaPixel{0.32F, 0.76F, 1.0F, 1.0F};
  const RgbaPixel text{0.96F, 0.96F, 0.92F, 1.0F};
  const RgbaPixel muted{0.72F, 0.76F, 0.78F, 1.0F};

  FormatStatus status = fillRect(output, window, background);
  if (!isSuccess(status)) {
    return status;
  }

  const int scale = chooseScale(window);
  const int margin = std::max(3, 5 * scale);
  const int lineHeight = (kGlyphHeight + 3) * scale;
  const RectI topBand{window.x1, window.y1, window.x2,
                      std::min(window.y2, window.y1 + margin * 2 + kGlyphHeight * scale)};
  status = fillRect(output, topBand, band);
  if (!isSuccess(status)) {
    return status;
  }

  const RectI accentRect{window.x1, window.y1, std::min(window.x2, window.x1 + 3 * scale),
                         window.y2};
  status = fillRect(output, accentRect, accent);
  if (!isSuccess(status)) {
    return status;
  }

  int textY = window.y1 + margin;
  const int textX = window.x1 + margin + 2 * scale;
  status = drawText(output, textX, textY, scale, printable(content.title, 48), text, window);
  if (!isSuccess(status)) {
    return status;
  }

  textY += lineHeight + scale;
  status = drawText(output, textX, textY, scale, printable(content.message, 72), accent,
                    window);
  if (!isSuccess(status)) {
    return status;
  }

  textY += lineHeight;
  for (const StatusFrameLine& line : content.lines) {
    if (textY + kGlyphHeight * scale >= window.y2 - margin) {
      break;
    }
    std::string textLine = printable(line.label, 24);
    if (!textLine.empty()) {
      textLine += ": ";
    }
    textLine += printable(line.value, 56);
    status = drawText(output, textX, textY, scale, textLine, muted, window);
    if (!isSuccess(status)) {
      return status;
    }
    textY += lineHeight;
  }

  return outputStatus;
}

}  // namespace CorridorKey::Core
