#include "core/AlphaHint.h"
#include "core/PixelConvert.h"

#include "ofxCore.h"
#include "ofxImageEffect.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "alpha hint test failed: " << message << '\n';
    return false;
  }
  return true;
}

bool near(float actual, float expected) {
  return std::abs(actual - expected) < 0.001F;
}

CorridorKey::Core::ImageFormat format(const char* components, const char* depth) {
  return CorridorKey::Core::imageFormatFromOfx(components, depth);
}

}  // namespace

int main() {
  using namespace CorridorKey::Core;

  bool ok = true;
  const RectI window{0, 0, 2, 1};

  std::vector<float> sourceRgba = {
      0.1F, 0.2F, 0.3F, 0.4F,
      0.5F, 0.6F, 0.7F, 0.8F,
  };
  PixelBufferView sourceView{sourceRgba.data(), window,
                             2 * 4 * static_cast<int>(sizeof(float)),
                             format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};

  {
    std::vector<float> externalRgba = {
        0.9F, 0.8F, 0.7F, 0.25F,
        0.1F, 0.2F, 0.3F, 0.75F,
    };
    PixelBufferView externalView{externalRgba.data(), window,
                                 2 * 4 * static_cast<int>(sizeof(float)),
                                 format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};

    const AlphaHintResult result =
        resolveAlphaHint(AlphaHintSource::External, sourceView, &externalView, window);
    ok &= expect(result.status == AlphaHintStatus::ExternalPresent,
                 "external RGBA alpha should be selected");
    ok &= expect(near(alphaHintValueAt(result.hint, 0, 0), 0.25F),
                 "external alpha first pixel mismatch");
    ok &= expect(near(alphaHintValueAt(result.hint, 1, 0), 0.75F),
                 "external alpha second pixel mismatch");
  }

  {
    const AlphaHintResult result =
        resolveAlphaHint(AlphaHintSource::SourceAlpha, sourceView, nullptr, window);
    ok &= expect(result.status == AlphaHintStatus::SourceAlphaUsed,
                 "source alpha should be selected");
    ok &= expect(near(alphaHintValueAt(result.hint, 0, 0), 0.4F),
                 "source alpha first pixel mismatch");
    ok &= expect(near(alphaHintValueAt(result.hint, 1, 0), 0.8F),
                 "source alpha second pixel mismatch");
  }

  {
    std::vector<float> externalRgb = {
        0.2F, 0.9F, 0.7F,
        0.6F, 0.1F, 0.3F,
    };
    PixelBufferView externalView{externalRgb.data(), window,
                                 2 * 3 * static_cast<int>(sizeof(float)),
                                 format(kOfxImageComponentRGB, kOfxBitDepthFloat)};

    const AlphaHintResult result =
        resolveAlphaHint(AlphaHintSource::RedChannel, sourceView, &externalView, window);
    ok &= expect(result.status == AlphaHintStatus::RedChannelUsed,
                 "red-channel hint should be selected");
    ok &= expect(near(alphaHintValueAt(result.hint, 0, 0), 0.2F),
                 "red-channel first pixel mismatch");
    ok &= expect(near(alphaHintValueAt(result.hint, 1, 0), 0.6F),
                 "red-channel second pixel mismatch");
  }

  {
    std::vector<float> externalRgb = {
        0.0F, 0.9F, 0.7F,
        0.0F, 0.8F, 0.6F,
    };
    PixelBufferView externalView{externalRgb.data(), window,
                                 2 * 3 * static_cast<int>(sizeof(float)),
                                 format(kOfxImageComponentRGB, kOfxBitDepthFloat)};

    const AlphaHintResult result =
        resolveAlphaHint(AlphaHintSource::RedChannel, sourceView, &externalView, window);
    ok &= expect(result.status == AlphaHintStatus::RedChannelUsed,
                 "all-black red channel should remain a valid matte");
    ok &= expect(near(alphaHintValueAt(result.hint, 0, 0), 0.0F),
                 "all-black red channel should not fall back to opaque");
    ok &= expect(near(alphaHintValueAt(result.hint, 1, 0), 0.0F),
                 "red-channel mode should not infer from green or blue");
  }

  {
    std::vector<float> externalAlpha = {0.2F, 0.6F};
    PixelBufferView externalView{externalAlpha.data(), window,
                                 2 * static_cast<int>(sizeof(float)),
                                 format(kOfxImageComponentAlpha, kOfxBitDepthFloat)};

    const AlphaHintResult result =
        resolveAlphaHint(AlphaHintSource::External, sourceView, &externalView, window);
    ok &= expect(result.status == AlphaHintStatus::ExternalPresent,
                 "single-channel alpha hint should be selected");
    ok &= expect(near(alphaHintValueAt(result.hint, 0, 0), 0.2F),
                 "single-channel alpha first pixel mismatch");
    ok &= expect(near(alphaHintValueAt(result.hint, 1, 0), 0.6F),
                 "single-channel alpha second pixel mismatch");
  }

  {
    std::vector<std::uint16_t> externalAlpha = {65535, 0};
    PixelBufferView externalView{externalAlpha.data(), window,
                                 2 * static_cast<int>(sizeof(std::uint16_t)),
                                 format(kOfxImageComponentAlpha, kOfxBitDepthShort)};

    const AlphaHintResult result =
        resolveAlphaHint(AlphaHintSource::External, sourceView, &externalView, window);
    ok &= expect(result.status == AlphaHintStatus::InvalidHintFallback,
                 "16-bit alpha hint should use invalid-hint fallback");
    ok &= expect(result.formatStatus == FormatStatus::UnsupportedPixelDepth,
                 "invalid hint fallback should preserve the format failure");
    ok &= expect(alphaHintStatusIsWarning(result.status),
                 "invalid hint fallback should be a visible warning status");
    ok &= expect(near(alphaHintValueAt(result.hint, 0, 0), 1.0F),
                 "16-bit alpha hint fallback should remain opaque");
    ok &= expect(std::string(alphaHintStatusLabel(result.status)) ==
                     "invalid hint; rough fallback used",
                 "invalid fallback label should be user-visible");
    ok &= expect(std::string(alphaHintStatusCode(result.status)) ==
                     "invalid_hint_fallback",
                 "invalid fallback status code should be stable");
  }

  {
    const AlphaHintResult result =
        resolveAlphaHint(AlphaHintSource::External, sourceView, nullptr, window);
    ok &= expect(result.status == AlphaHintStatus::MissingHintFallback,
                 "missing external clip should use missing-hint fallback");
    ok &= expect(alphaHintStatusIsWarning(result.status),
                 "missing external clip should be a visible warning status");
    ok &= expect(near(alphaHintValueAt(result.hint, 1, 0), 1.0F),
                 "missing external clip fallback should be deterministic");
    ok &= expect(std::string(alphaHintStatusLabel(result.status)) ==
                     "missing hint; rough fallback used",
                 "missing fallback label should be user-visible");
    ok &= expect(std::string(alphaHintStatusCode(result.status)) ==
                     "missing_hint_fallback",
                 "missing fallback status code should be stable");
  }

  {
    std::vector<float> sourceRgb = {
        0.1F, 0.2F, 0.3F,
        0.5F, 0.6F, 0.7F,
    };
    PixelBufferView rgbSourceView{sourceRgb.data(), window,
                                  2 * 3 * static_cast<int>(sizeof(float)),
                                  format(kOfxImageComponentRGB, kOfxBitDepthFloat)};
    const AlphaHintResult result =
        resolveAlphaHint(AlphaHintSource::SourceAlpha, rgbSourceView, nullptr, window);
    ok &= expect(result.status == AlphaHintStatus::MissingHintFallback,
                 "RGB source without alpha should use missing-hint fallback");
  }

  {
    const AlphaHintResult result =
        resolveAlphaHint(AlphaHintSource::RoughFallback, sourceView, nullptr, window);
    ok &= expect(result.status == AlphaHintStatus::RoughFallbackUsed,
                 "explicit rough fallback should be reported");
    ok &= expect(near(alphaHintValueAt(result.hint, 0, 0), 1.0F),
                 "rough fallback should be deterministic");
    ok &= expect(std::string(alphaHintStatusLabel(result.status)) ==
                     "rough fallback used",
                 "rough fallback label should be user-visible");
    ok &= expect(std::string(alphaHintStatusCode(result.status)) ==
                     "rough_fallback_used",
                 "rough fallback status code should be stable");
  }

  {
    std::vector<unsigned char> externalRgb = {128, 0, 0};
    PixelBufferView externalView{externalRgb.data(), RectI{0, 0, 1, 1}, 3,
                                 format(kOfxImageComponentRGB, kOfxBitDepthByte)};
    const AlphaHintResult result =
        resolveAlphaHint(AlphaHintSource::RedChannel, sourceView, &externalView,
                         RectI{0, 0, 1, 1});
    ok &= expect(result.formatStatus == FormatStatus::ByteFallback,
                 "8-bit red-channel hint should preserve fallback format status");
    ok &= expect(near(alphaHintValueAt(result.hint, 0, 0), 128.0F / 255.0F),
                 "8-bit red-channel hint should normalize red only");
  }

  {
    std::vector<float> row0 = {0.25F, 0.0F, 0.0F, 1.0F};
    std::vector<float> row1 = {0.75F, 0.0F, 0.0F, 1.0F};
    std::vector<float> storage;
    storage.insert(storage.end(), row1.begin(), row1.end());
    storage.insert(storage.end(), row0.begin(), row0.end());
    PixelBufferView negativeStrideView{storage.data() + 4, RectI{0, 0, 1, 2},
                                       -4 * static_cast<int>(sizeof(float)),
                                       format(kOfxImageComponentRGBA, kOfxBitDepthFloat)};
    const AlphaHintResult result =
        resolveAlphaHint(AlphaHintSource::RedChannel, negativeStrideView,
                         &negativeStrideView, RectI{0, 0, 1, 2});
    ok &= expect(result.status == AlphaHintStatus::RedChannelUsed,
                 "negative rowBytes red-channel hint should be readable");
    ok &= expect(near(alphaHintValueAt(result.hint, 0, 0), 0.25F),
                 "negative rowBytes first red-channel pixel mismatch");
    ok &= expect(near(alphaHintValueAt(result.hint, 0, 1), 0.75F),
                 "negative rowBytes second red-channel pixel mismatch");
  }

  return ok ? 0 : 1;
}
