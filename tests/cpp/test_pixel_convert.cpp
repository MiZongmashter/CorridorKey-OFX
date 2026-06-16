#include "core/PixelBuffer.h"
#include "core/PixelConvert.h"

#include "ofxCore.h"
#include "ofxImageEffect.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "pixel convert test failed: " << message << '\n';
    return false;
  }
  return true;
}

bool near(float actual, float expected) {
  return std::abs(actual - expected) < 0.001F;
}

bool expectPixel(const CorridorKey::Core::RgbaPixel& actual,
                 const CorridorKey::Core::RgbaPixel& expected,
                 const std::string& label) {
  bool ok = true;
  ok &= expect(near(actual.r, expected.r), label + " red mismatch");
  ok &= expect(near(actual.g, expected.g), label + " green mismatch");
  ok &= expect(near(actual.b, expected.b), label + " blue mismatch");
  ok &= expect(near(actual.a, expected.a), label + " alpha mismatch");
  return ok;
}

CorridorKey::Core::ImageFormat format(const char* components, const char* depth) {
  return CorridorKey::Core::imageFormatFromOfx(components, depth);
}

}  // namespace

int main() {
  using namespace CorridorKey::Core;

  bool ok = true;

  {
    std::vector<float> source = {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F};
    std::vector<float> outputRgba(8, -1.0F);
    std::vector<float> outputRgb(6, -1.0F);
    PixelBufferView sourceView{source.data(), RectI{0, 0, 2, 1},
                               2 * 3 * static_cast<int>(sizeof(float)),
                               format(kOfxImageComponentRGB, kOfxBitDepthFloat)};
    PixelBufferView outputRgbaView{outputRgba.data(), RectI{0, 0, 2, 1},
                                   2 * 4 * static_cast<int>(sizeof(float)),
                                   format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    PixelBufferView outputRgbView{outputRgb.data(), RectI{0, 0, 2, 1},
                                  2 * 3 * static_cast<int>(sizeof(float)),
                                  format(kOfxImageComponentRGB, kOfxBitDepthFloat)};

    ok &= expect(copyWindow(sourceView, outputRgbaView, RectI{0, 0, 2, 1}) ==
                     FormatStatus::Ok,
                 "RGB float to RGBA float copy should succeed");
    ok &= expect(outputRgba == std::vector<float>({0.1F, 0.2F, 0.3F, 1.0F,
                                                   0.4F, 0.5F, 0.6F, 1.0F}),
                 "RGB input should write opaque RGBA output");
    ok &= expect(copyWindow(sourceView, outputRgbView, RectI{0, 0, 2, 1}) ==
                     FormatStatus::Ok,
                 "RGB float to RGB float copy should succeed");
    ok &= expect(outputRgb == source, "RGB float output should preserve RGB channels");
  }

  {
    std::vector<std::uint16_t> source = {
        floatToHalfBits(0.25F), floatToHalfBits(0.5F), floatToHalfBits(0.75F),
        floatToHalfBits(0.6F),
    };
    std::vector<std::uint16_t> outputRgb(3, floatToHalfBits(-1.0F));
    std::vector<std::uint16_t> outputRgba(4, floatToHalfBits(-1.0F));
    PixelBufferView sourceView{source.data(), RectI{0, 0, 1, 1},
                               4 * static_cast<int>(sizeof(std::uint16_t)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthHalf)};
    PixelBufferView outputRgbView{outputRgb.data(), RectI{0, 0, 1, 1},
                                  3 * static_cast<int>(sizeof(std::uint16_t)),
                                  format(kOfxImageComponentRGB, kOfxBitDepthHalf)};
    PixelBufferView outputRgbaView{outputRgba.data(), RectI{0, 0, 1, 1},
                                   4 * static_cast<int>(sizeof(std::uint16_t)),
                                   format(kOfxImageComponentRGBA, kOfxBitDepthHalf)};

    ok &= expect(copyWindow(sourceView, outputRgbView, RectI{0, 0, 1, 1}) ==
                     FormatStatus::Ok,
                 "RGBA half to RGB half writeback should succeed");
    ok &= expect(near(halfBitsToFloat(outputRgb[0]), 0.25F), "half red mismatch");
    ok &= expect(near(halfBitsToFloat(outputRgb[1]), 0.5F), "half green mismatch");
    ok &= expect(near(halfBitsToFloat(outputRgb[2]), 0.75F), "half blue mismatch");
    ok &= expect(copyWindow(sourceView, outputRgbaView, RectI{0, 0, 1, 1}) ==
                     FormatStatus::Ok,
                 "RGBA half to RGBA half writeback should succeed");
    ok &= expect(near(halfBitsToFloat(outputRgba[0]), 0.25F),
                 "half RGBA red mismatch");
    ok &= expect(near(halfBitsToFloat(outputRgba[1]), 0.5F),
                 "half RGBA green mismatch");
    ok &= expect(near(halfBitsToFloat(outputRgba[2]), 0.75F),
                 "half RGBA blue mismatch");
    ok &= expect(near(halfBitsToFloat(outputRgba[3]), 0.6F),
                 "half RGBA alpha mismatch");
  }

  {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    ok &= expect(std::isnan(halfBitsToFloat(floatToHalfBits(nan))),
                 "half conversion should preserve NaN as NaN");
    ok &= expect(std::isinf(halfBitsToFloat(floatToHalfBits(inf))),
                 "half conversion should preserve infinity");
    ok &= expect(floatToHalfBits(0x1.0p-25F) == 0,
                 "half conversion should round subnormal ties to even");
    ok &= expect(floatToHalfBits(1.0F + 0x1.0p-11F) == 0x3C00,
                 "half conversion should round normal even ties down");
    ok &= expect(floatToHalfBits(1.0F + 0x3.0p-11F) == 0x3C02,
                 "half conversion should round normal odd ties up");
    ok &= expect(floatToHalfBits(2.0F - 0x1.0p-11F) == 0x4000,
                 "half conversion should carry tie rounding into exponent");
  }

  {
    std::vector<unsigned char> source = {0, 128, 255};
    std::vector<float> output(4, -1.0F);
    PixelBufferView sourceView{source.data(), RectI{0, 0, 1, 1}, 3,
                               format(kOfxImageComponentRGB, kOfxBitDepthByte)};
    PixelBufferView outputView{output.data(), RectI{0, 0, 1, 1},
                               4 * static_cast<int>(sizeof(float)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};

    ok &= expect(copyWindow(sourceView, outputView, RectI{0, 0, 1, 1}) ==
                     FormatStatus::ByteFallback,
                 "8-bit RGB input should return explicit fallback status");
    ok &= expect(near(output[0], 0.0F), "byte red should normalize");
    ok &= expect(near(output[1], 128.0F / 255.0F), "byte green should normalize");
    ok &= expect(near(output[2], 1.0F), "byte blue should normalize");
    ok &= expect(near(output[3], 1.0F), "byte RGB alpha should be opaque");
  }

  {
    std::vector<unsigned char> storage(1 + 4 * sizeof(float));
    auto* misaligned = storage.data() + 1;
    const float values[4] = {0.1F, 0.2F, 0.3F, 0.4F};
    std::memcpy(misaligned, values, sizeof(values));

    RgbaPixel pixel{};
    PixelBufferView sourceView{misaligned, RectI{0, 0, 1, 1},
                               4 * static_cast<int>(sizeof(float)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    ok &= expect(readPixel(sourceView, 0, 0, pixel) == FormatStatus::Ok,
                 "misaligned float input should be readable via memcpy");
    ok &= expectPixel(pixel, RgbaPixel{0.1F, 0.2F, 0.3F, 0.4F},
                      "misaligned float input");
  }

  {
    RgbaPixel pixel{};
    PixelBufferView missingData{nullptr, RectI{0, 0, 1, 1},
                                4 * static_cast<int>(sizeof(float)),
                                format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    ok &= expect(readPixel(missingData, 0, 0, pixel) == FormatStatus::MissingData,
                 "readPixel should report missing input data");
    ok &= expect(writePixel(missingData, 0, 0, RgbaPixel{}) == FormatStatus::MissingData,
                 "writePixel should report missing output data");
  }

  {
    std::vector<float> row0 = {0.1F, 0.2F, 0.3F, 0.4F};
    std::vector<float> row1 = {0.5F, 0.6F, 0.7F, 0.8F};
    std::vector<float> storage;
    storage.insert(storage.end(), row1.begin(), row1.end());
    storage.insert(storage.end(), row0.begin(), row0.end());
    std::vector<float> output(8, -1.0F);
    PixelBufferView sourceView{storage.data() + 4, RectI{0, 0, 1, 2},
                               -4 * static_cast<int>(sizeof(float)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    PixelBufferView outputView{output.data(), RectI{0, 0, 1, 2},
                               4 * static_cast<int>(sizeof(float)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    ok &= expect(copyWindow(sourceView, outputView, RectI{0, 0, 1, 2}) ==
                     FormatStatus::Ok,
                 "negative rowBytes should be supported");
    ok &= expect(output == std::vector<float>({0.1F, 0.2F, 0.3F, 0.4F,
                                               0.5F, 0.6F, 0.7F, 0.8F}),
                 "negative rowBytes should address rows correctly");
  }

  {
    std::vector<std::uint16_t> source(4, 1000);
    std::vector<float> output(4, -1.0F);
    PixelBufferView sourceView{source.data(), RectI{0, 0, 1, 1},
                               4 * static_cast<int>(sizeof(std::uint16_t)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthShort)};
    PixelBufferView outputView{output.data(), RectI{0, 0, 1, 1},
                               4 * static_cast<int>(sizeof(float)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};

    ok &= expect(copyWindow(sourceView, outputView, RectI{0, 0, 1, 1}) ==
                     FormatStatus::UnsupportedPixelDepth,
                 "16-bit uint input should be explicitly unsupported");
    ok &= expect(output == std::vector<float>(4, -1.0F),
                 "unsupported input should not overwrite output");
  }

  {
    std::vector<float> source = {0.1F, 0.2F, 0.3F, 0.4F};
    std::vector<float> output(4, -1.0F);
    PixelBufferView sourceView{source.data(), RectI{0, 0, 1, 1},
                               4 * static_cast<int>(sizeof(float)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    PixelBufferView outputView{output.data(), RectI{0, 0, std::numeric_limits<int>::max(), 1},
                               std::numeric_limits<int>::max(),
                               format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    ok &= expect(copyWindow(sourceView, outputView, RectI{0, 0, 1, 1}) ==
                     FormatStatus::InvalidRowBytes,
                 "overflow-sized rowBytes requirement should be invalid");
  }

  {
    std::vector<float> source = {0.1F, 0.2F, 0.3F, 0.4F};
    std::vector<std::uint16_t> output(4, 1000);
    PixelBufferView sourceView{source.data(), RectI{0, 0, 1, 1},
                               4 * static_cast<int>(sizeof(float)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    PixelBufferView outputView{output.data(), RectI{0, 0, 1, 1},
                               4 * static_cast<int>(sizeof(std::uint16_t)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthShort)};

    ok &= expect(copyWindow(sourceView, outputView, RectI{0, 0, 1, 1}) ==
                     FormatStatus::UnsupportedPixelDepth,
                 "16-bit uint output should be explicitly unsupported");
    ok &= expect(output == std::vector<std::uint16_t>(4, 1000),
                 "unsupported output should not overwrite output");
  }

  {
    std::vector<float> source = {0.51F, 0.52F, 0.53F, 0.54F};
    std::vector<float> output(3 * 3 * 4, -1.0F);
    PixelBufferView sourceView{source.data(), RectI{1, 1, 2, 2},
                               4 * static_cast<int>(sizeof(float)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    PixelBufferView outputView{output.data(), RectI{0, 0, 3, 3},
                               3 * 4 * static_cast<int>(sizeof(float)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};

    ok &= expect(copyWindow(sourceView, outputView, RectI{0, 0, 3, 3}) ==
                     FormatStatus::Ok,
                 "partial render window with out-of-bounds source should succeed");

    RgbaPixel pixel{};
    ok &= expect(readPixel(outputView, 1, 1, pixel) == FormatStatus::Ok,
                 "inside copied output should be readable");
    ok &= expectPixel(pixel, RgbaPixel{0.51F, 0.52F, 0.53F, 0.54F},
                      "inside copied output");

    ok &= expect(readPixel(outputView, 0, 0, pixel) == FormatStatus::Ok,
                 "outside source output should be readable");
    ok &= expectPixel(pixel, RgbaPixel{0.0F, 0.0F, 0.0F, 0.0F},
                      "outside source output");
  }

  {
    std::vector<float> output = {0.1F, 0.2F, 0.3F, 0.4F};
    PixelBufferView outputView{output.data(), RectI{0, 0, 1, 1},
                               4 * static_cast<int>(sizeof(float)),
                               format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    ok &= expect(fillWindowTransparent(outputView, RectI{0, 0, 1, 1}) ==
                     FormatStatus::Ok,
                 "fillWindowTransparent should fill a float RGBA window");
    ok &= expect(output == std::vector<float>(4, 0.0F),
                 "fillWindowTransparent should write transparent black");
    PixelBufferView missingOutput{nullptr, RectI{0, 0, 1, 1},
                                  4 * static_cast<int>(sizeof(float)),
                                  format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    ok &= expect(fillWindowTransparent(missingOutput, RectI{0, 0, 1, 1}) ==
                     FormatStatus::MissingData,
                 "fillWindowTransparent should report missing output data");
  }

  return ok ? 0 : 1;
}
