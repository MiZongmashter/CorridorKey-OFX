#include "core/ImageFormat.h"

#include "ofxCore.h"
#include "ofxImageEffect.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "image format test failed: " << message << '\n';
    return false;
  }
  return true;
}

bool near(double actual, double expected) {
  return std::abs(actual - expected) < 0.000001;
}

}  // namespace

int main() {
  using namespace CorridorKey::Core;

  bool ok = true;

  const ImageFormat rgbaFloat =
      imageFormatFromOfx(kOfxImageComponentRGBA, kOfxBitDepthFloat);
  ok &= expect(rgbaFloat.components == PixelComponents::RGBA,
               "RGBA components should parse");
  ok &= expect(rgbaFloat.depth == PixelDepth::Float, "float depth should parse");
  ok &= expect(componentCount(rgbaFloat.components) == 4,
               "RGBA should have four components");
  ok &= expect(bytesPerComponent(rgbaFloat.depth) == 4,
               "float should be four bytes per component");

  const ImageFormat rgbHalf =
      imageFormatFromOfx(kOfxImageComponentRGB, kOfxBitDepthHalf);
  ok &= expect(inputFormatStatus(rgbHalf) == FormatStatus::Ok,
               "RGB half should be supported as input");
  ok &= expect(outputFormatStatus(rgbHalf) == FormatStatus::Ok,
               "RGB half should be supported as output");

  const ImageFormat byteInput =
      imageFormatFromOfx(kOfxImageComponentRGB, kOfxBitDepthByte);
  ok &= expect(inputFormatStatus(byteInput) == FormatStatus::ByteFallback,
               "8-bit input should be an explicit fallback");
  ok &= expect(outputFormatStatus(byteInput) == FormatStatus::UnsupportedPixelDepth,
               "8-bit output should be explicitly unsupported");

  const ImageFormat shortInput =
      imageFormatFromOfx(kOfxImageComponentRGBA, kOfxBitDepthShort);
  ok &= expect(inputFormatStatus(shortInput) == FormatStatus::UnsupportedPixelDepth,
               "16-bit uint input should be explicitly unsupported");
  ok &= expect(outputFormatStatus(shortInput) == FormatStatus::UnsupportedPixelDepth,
               "16-bit uint output should be explicitly unsupported");

  const ImageFormat alpha =
      imageFormatFromOfx(kOfxImageComponentAlpha, kOfxBitDepthFloat);
  ok &= expect(alpha.components == PixelComponents::Alpha,
               "Alpha components should parse");
  ok &= expect(componentCount(alpha.components) == 1,
               "Alpha should have one component");
  ok &= expect(inputFormatStatus(alpha) == FormatStatus::UnsupportedComponents,
               "generic image input should not accept alpha-only components");

  const PixelAspectRatio par{1.333333};
  ok &= expect(near(passThroughPixelAspectRatio(par).value, par.value),
               "PAR helper should preserve source PAR for passthrough");

  ok &= expect(renderFieldingStatus(parseFielding(kOfxImageFieldNone)) ==
                   FormatStatus::Ok,
               "unfielded rendering should be supported");
  ok &= expect(renderFieldingStatus(parseFielding(kOfxImageFieldLower)) ==
                   FormatStatus::UnsupportedFielding,
               "lower-field rendering should be explicit unsupported status");
  ok &= expect(renderFieldingStatus(parseFielding(kOfxImageFieldUpper)) ==
                   FormatStatus::UnsupportedFielding,
               "upper-field rendering should be explicit unsupported status");
  ok &= expect(renderFieldingStatus(parseFielding(kOfxImageFieldBoth)) ==
                   FormatStatus::UnsupportedFielding,
               "both-field rendering should be explicit unsupported status");
  ok &= expect(renderFieldingStatus(parseFielding(kOfxImageFieldSingle)) ==
                   FormatStatus::UnsupportedFielding,
               "single-field rendering should be explicit unsupported status");
  ok &= expect(renderFieldingStatus(parseFielding(kOfxImageFieldDoubled)) ==
                   FormatStatus::UnsupportedFielding,
               "doubled-field rendering should be explicit unsupported status");
  ok &= expect(renderFieldingStatus(parseFielding(nullptr)) ==
                   FormatStatus::UnsupportedFielding,
               "unknown fielding should be explicit unsupported status");

  ok &= expect(isSuccess(FormatStatus::Ok), "Ok should be a success status");
  ok &= expect(isSuccess(FormatStatus::ByteFallback),
               "ByteFallback should be a degraded success status");
  ok &= expect(!isSuccess(FormatStatus::UnsupportedPixelDepth),
               "unsupported depth should not be a success status");

  return ok ? 0 : 1;
}
