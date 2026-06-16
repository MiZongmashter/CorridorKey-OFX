#include "core/PixelConvert.h"
#include "core/StatusFrame.h"

#include "ofxCore.h"
#include "ofxImageEffect.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "status frame test failed: " << message << '\n';
    return false;
  }
  return true;
}

CorridorKey::Core::ImageFormat format(const char* components, const char* depth) {
  return CorridorKey::Core::imageFormatFromOfx(components, depth);
}

}  // namespace

int main() {
  using namespace CorridorKey::Core;

  bool ok = true;

  {
    constexpr int width = 96;
    constexpr int height = 48;
    std::vector<float> output(width * height * 4, -1.0F);
    PixelBufferView view{output.data(), RectI{0, 0, width, height},
                         width * 4 * static_cast<int>(sizeof(float)),
                         format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};

    const StatusFrameContent content{
        StatusFrameSeverity::Error,
        "Sidecar Error",
        "Protocol error",
        {{"Backend", "stub"}, {"Last Error", "protocol_error"}},
    };
    ok &= expect(renderStatusFrame(view, RectI{0, 0, width, height}, content) ==
                     FormatStatus::Ok,
                 "float status frame should render successfully");

    int brightPixels = 0;
    int changedPixels = 0;
    for (std::size_t index = 0; index < output.size(); index += 4) {
      if (output[index] != -1.0F || output[index + 1] != -1.0F ||
          output[index + 2] != -1.0F || output[index + 3] != -1.0F) {
        ++changedPixels;
      }
      if (output[index] > 0.75F && output[index + 1] > 0.75F &&
          output[index + 2] > 0.75F) {
        ++brightPixels;
      }
    }
    ok &= expect(changedPixels == width * height,
                 "status frame should fill the requested output area");
    ok &= expect(brightPixels > 80, "status frame should contain readable bright text");
  }

  {
    constexpr int width = 24;
    constexpr int height = 16;
    std::vector<float> output(width * height * 4, -1.0F);
    PixelBufferView view{output.data(), RectI{0, 0, width, height},
                         width * 4 * static_cast<int>(sizeof(float)),
                         format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};

    const StatusFrameContent content{
        StatusFrameSeverity::Status,
        "Status",
        "Waiting",
        {},
    };
    ok &= expect(renderStatusFrame(view, RectI{4, 3, 20, 12}, content) ==
                     FormatStatus::Ok,
                 "partial status frame should render successfully");
    ok &= expect(output[0] == -1.0F && output[1] == -1.0F && output[2] == -1.0F &&
                     output[3] == -1.0F,
                 "status frame must not write outside render window");

    RgbaPixel inside{};
    ok &= expect(readPixel(view, 5, 4, inside) == FormatStatus::Ok,
                 "inside status frame should be readable");
    ok &= expect(inside.a == 1.0F, "status frame should write opaque pixels");
  }

  {
    constexpr int width = 64;
    constexpr int height = 32;
    std::vector<std::uint16_t> output(width * height * 4, floatToHalfBits(-1.0F));
    PixelBufferView view{output.data(), RectI{0, 0, width, height},
                         width * 4 * static_cast<int>(sizeof(std::uint16_t)),
                         format(kOfxImageComponentRGBA, kOfxBitDepthHalf)};
    const StatusFrameContent content{
        StatusFrameSeverity::Error,
        "Crash",
        "Runtime stopped",
        {},
    };
    ok &= expect(renderStatusFrame(view, RectI{0, 0, width, height}, content) ==
                     FormatStatus::Ok,
                 "half status frame should render successfully");
    ok &= expect(halfBitsToFloat(output[0]) >= 0.0F,
                 "half output should be overwritten with visible pixels");
  }

  return ok ? 0 : 1;
}
