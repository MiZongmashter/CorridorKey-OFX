#include "core/ColorAlphaContract.h"

#include <cmath>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "color alpha contract test failed: " << message << '\n';
    return false;
  }
  return true;
}

bool near(float actual, float expected, float tolerance = 0.00001F) {
  return std::fabs(actual - expected) <= tolerance;
}

bool finitePixel(const CorridorKey::Core::RgbaPixel& pixel) {
  return std::isfinite(pixel.r) && std::isfinite(pixel.g) &&
         std::isfinite(pixel.b) && std::isfinite(pixel.a);
}

bool pixelNear(const CorridorKey::Core::RgbaPixel& actual,
               const CorridorKey::Core::RgbaPixel& expected,
               float tolerance = 0.00001F) {
  return near(actual.r, expected.r, tolerance) &&
         near(actual.g, expected.g, tolerance) &&
         near(actual.b, expected.b, tolerance) &&
         near(actual.a, expected.a, tolerance);
}

bool testInputColorModes() {
  using namespace CorridorKey::Core;

  const RgbaPixel encoded{0.5F, 0.04045F, 1.0F, 0.25F};
  const RgbaPixel hostManaged = convertInputToLinear(encoded, InputColorMode::HostManaged);
  const RgbaPixel linear = convertInputToLinear(encoded, InputColorMode::Linear);
  const RgbaPixel decoded = convertInputToLinear(encoded, InputColorMode::SrgbRec709Gamma);

  bool ok = true;
  ok &= expect(pixelNear(hostManaged, encoded), "host-managed input must pass through");
  ok &= expect(pixelNear(linear, encoded), "linear input must pass through");
  ok &= expect(near(decoded.r, 0.21404114F), "sRGB 0.5 must decode to linear");
  ok &= expect(near(decoded.g, 0.0031308F), "sRGB toe must decode before alpha math");
  ok &= expect(near(decoded.b, 1.0F), "sRGB white must remain linear white");
  ok &= expect(near(decoded.a, 0.25F), "input alpha must stay linear");
  return ok;
}

bool testPremultiplyAndUnpremultiply() {
  using namespace CorridorKey::Core;

  bool ok = true;
  ok &= expect(pixelNear(premultiply(RgbaPixel{0.8F, 0.4F, 0.2F, 0.0F}),
                         RgbaPixel{0.0F, 0.0F, 0.0F, 0.0F}),
               "alpha zero premultiply must write transparent black");
  ok &= expect(pixelNear(premultiply(RgbaPixel{0.8F, 0.4F, 0.2F, 0.5F}),
                         RgbaPixel{0.4F, 0.2F, 0.1F, 0.5F}),
               "alpha 0.5 premultiply must multiply RGB once");
  ok &= expect(pixelNear(premultiply(RgbaPixel{0.8F, 0.4F, 0.2F, 1.0F}),
                         RgbaPixel{0.8F, 0.4F, 0.2F, 1.0F}),
               "alpha 1 premultiply must preserve RGB");

  const RgbaPixel zeroStraight = unpremultiply(RgbaPixel{0.2F, 0.3F, 0.4F, 0.0F});
  ok &= expect(pixelNear(zeroStraight, RgbaPixel{0.0F, 0.0F, 0.0F, 0.0F}),
               "zero-alpha unpremultiply must return transparent black");
  ok &= expect(finitePixel(zeroStraight),
               "zero-alpha unpremultiply must not produce NaN or Inf");
  ok &= expect(pixelNear(unpremultiply(RgbaPixel{0.4F, 0.2F, 0.1F, 0.5F}),
                         RgbaPixel{0.8F, 0.4F, 0.2F, 0.5F}),
               "alpha 0.5 unpremultiply must recover straight RGB");

  const RgbaPixel premultiplied{0.4F, 0.2F, 0.1F, 0.5F};
  ok &= expect(pixelNear(toPremultiplied(premultiplied, AlphaState::Premultiplied),
                         premultiplied),
               "already premultiplied pixels must not be multiplied twice");
  ok &= expect(pixelNear(toStraight(RgbaPixel{0.8F, 0.4F, 0.2F, 0.5F},
                                  AlphaState::Straight),
                         RgbaPixel{0.8F, 0.4F, 0.2F, 0.5F}),
               "straight pixels must stay straight when requested");
  return ok;
}

bool testOutputModePixels() {
  using namespace CorridorKey::Core;

  const RgbaPixel encoded{0.5F, 0.5F, 0.5F, 1.0F};
  const RgbaPixel linear = convertInputToLinear(encoded, InputColorMode::SrgbRec709Gamma);

  bool ok = true;
  ok &= expect(pixelNear(processedRgba(linear, 0.5F),
                         RgbaPixel{0.10702057F, 0.10702057F, 0.10702057F, 0.5F}),
               "processed RGBA must decode gamma before multiplying alpha");
  ok &= expect(pixelNear(straightForeground(RgbaPixel{0.8F, 0.4F, 0.2F, 1.0F},
                                           0.5F),
                         RgbaPixel{0.8F, 0.4F, 0.2F, 0.5F}),
               "straight FG must not be affected by output premultiply state");
  ok &= expect(pixelNear(mattePixel(0.5F), RgbaPixel{0.5F, 0.5F, 0.5F, 0.5F}),
               "matte output must carry linear alpha");
  ok &= expect(pixelNear(mattePixel(0.0F), RgbaPixel{0.0F, 0.0F, 0.0F, 0.0F}),
               "zero matte must remain linear transparent black");
  ok &= expect(pixelNear(mattePixel(1.0F), RgbaPixel{1.0F, 1.0F, 1.0F, 1.0F}),
               "full matte must remain linear white");
  return ok;
}

bool testOutputContracts() {
  using namespace CorridorKey::Core;

  bool ok = true;
  ok &= expect(outputPremultiplication(OutputMode::ProcessedRgba) ==
                   Premultiplication::Premultiplied,
               "processed RGBA must be declared premultiplied");
  ok &= expect(outputPremultiplication(OutputMode::StraightForeground) ==
                   Premultiplication::Unpremultiplied,
               "straight FG must be declared unpremultiplied");
  ok &= expect(outputPremultiplication(OutputMode::Matte) ==
                   Premultiplication::Unpremultiplied,
               "matte must be declared unpremultiplied");
  ok &= expect(outputPremultiplication(OutputMode::AlphaHintView) ==
                   Premultiplication::Unpremultiplied,
               "alpha hint view must be declared unpremultiplied");
  ok &= expect(outputPremultiplication(OutputMode::CheckerComp) ==
                   Premultiplication::Opaque,
               "checker comp must be declared opaque");
  ok &= expect(outputPremultiplication(OutputMode::StatusFrame) ==
                   Premultiplication::Opaque,
               "status frames must be declared opaque");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testInputColorModes();
  ok &= testPremultiplyAndUnpremultiply();
  ok &= testOutputModePixels();
  ok &= testOutputContracts();
  return ok ? 0 : 1;
}
