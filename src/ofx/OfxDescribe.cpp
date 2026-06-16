#include "CorridorKeyPlugin.h"

#include <cstring>

namespace CorridorKey::OFX {
namespace {

inline constexpr const char* kAlphaHintClipName = "AlphaHint";

OfxStatus requireSuites() {
  return effectSuite() != nullptr && propertySuite() != nullptr &&
                 parameterSuite() != nullptr
             ? kOfxStatOK
             : kOfxStatErrMissingHostFeature;
}

void configureRenderScheduling(OfxPropertySetHandle props) {
  propertySuite()->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
  propertySuite()->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 0);
  propertySuite()->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0,
                                 kOfxImageEffectRenderUnsafe);
}

OfxStatus defineClip(OfxImageEffectHandle effect,
                     const char* name,
                     bool optional = false,
                     bool supportsRgb = false) {
  OfxPropertySetHandle props = nullptr;
  const OfxStatus status = effectSuite()->clipDefine(effect, name, &props);
  if (status != kOfxStatOK) {
    return status;
  }

  propertySuite()->propSetString(props, kOfxImageEffectPropSupportedComponents, 0,
                                 kOfxImageComponentRGBA);
  if (supportsRgb) {
    propertySuite()->propSetString(props, kOfxImageEffectPropSupportedComponents, 1,
                                   kOfxImageComponentRGB);
    propertySuite()->propSetString(props, kOfxImageEffectPropSupportedComponents, 2,
                                   kOfxImageComponentAlpha);
  }
  propertySuite()->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0,
                                 kOfxBitDepthFloat);
  propertySuite()->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
  if (optional) {
    propertySuite()->propSetInt(props, kOfxImageClipPropOptional, 0, 1);
  }
  return kOfxStatOK;
}

}  // namespace

OfxStatus describe(OfxImageEffectHandle effect) {
  if (requireSuites() != kOfxStatOK) {
    return kOfxStatErrMissingHostFeature;
  }

  OfxPropertySetHandle props = nullptr;
  const OfxStatus status = effectSuite()->getPropertySet(effect, &props);
  if (status != kOfxStatOK) {
    return status;
  }

  propertySuite()->propSetString(props, kOfxPropLabel, 0, kPluginLabel);
  propertySuite()->propSetString(props, kOfxImageEffectPluginPropGrouping, 0,
                                 kPluginGrouping);
  propertySuite()->propSetString(props, kOfxImageEffectPropSupportedContexts, 0,
                                 kOfxImageEffectContextFilter);
  propertySuite()->propSetString(props, kOfxImageEffectPropSupportedContexts, 1,
                                 kOfxImageEffectContextGeneral);
  propertySuite()->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0,
                                 kOfxBitDepthFloat);
  propertySuite()->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
  configureRenderScheduling(props);
  return kOfxStatOK;
}

OfxStatus describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
  if (requireSuites() != kOfxStatOK) {
    return kOfxStatErrMissingHostFeature;
  }

  char* context = nullptr;
  if (inArgs != nullptr) {
    propertySuite()->propGetString(inArgs, kOfxImageEffectPropContext, 0, &context);
  }
  const bool isFilterContext =
      context == nullptr || std::strcmp(context, kOfxImageEffectContextFilter) == 0;
  const bool isGeneralContext =
      context != nullptr &&
      std::strncmp(context, kOfxImageEffectContextGeneral,
                   std::strlen(kOfxImageEffectContextGeneral)) == 0;
  if (!isFilterContext && !isGeneralContext) {
    return kOfxStatErrUnsupported;
  }

  OfxPropertySetHandle props = nullptr;
  OfxStatus status = effectSuite()->getPropertySet(effect, &props);
  if (status != kOfxStatOK) {
    return status;
  }
  propertySuite()->propSetString(props, kOfxImageEffectPropClipPreferencesSlaveParam,
                                 0, "ck_output_mode");
  configureRenderScheduling(props);

  status = defineClip(effect, kOfxImageEffectOutputClipName);
  if (status != kOfxStatOK) {
    return status;
  }
  status = defineClip(effect, kOfxImageEffectSimpleSourceClipName);
  if (status != kOfxStatOK) {
    return status;
  }
  if (isGeneralContext) {
    status = defineClip(effect, kAlphaHintClipName, true, true);
    if (status != kOfxStatOK) {
      return status;
    }
  }
  return defineParameters(effect);
}

}  // namespace CorridorKey::OFX
