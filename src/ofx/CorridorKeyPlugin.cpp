#include "CorridorKeyPlugin.h"

#include "ofxParam.h"

#include <cstring>
#include <new>

namespace CorridorKey::OFX {
namespace {

OfxHost* g_host = nullptr;
OfxImageEffectSuiteV1* g_effectSuite = nullptr;
OfxParameterSuiteV1* g_parameterSuite = nullptr;
OfxPropertySuiteV1* g_propertySuite = nullptr;

}  // namespace

OfxImageEffectSuiteV1* effectSuite() {
  return g_effectSuite;
}

OfxPropertySuiteV1* propertySuite() {
  return g_propertySuite;
}

OfxParameterSuiteV1* parameterSuite() {
  return g_parameterSuite;
}

void setHost(OfxHost* host) {
  g_host = host;
}

OfxStatus onLoad() {
  if (g_host == nullptr || g_host->fetchSuite == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }

  g_effectSuite = static_cast<OfxImageEffectSuiteV1*>(
      const_cast<void*>(g_host->fetchSuite(g_host->host, kOfxImageEffectSuite, 1)));
  g_propertySuite = static_cast<OfxPropertySuiteV1*>(
      const_cast<void*>(g_host->fetchSuite(g_host->host, kOfxPropertySuite, 1)));
  g_parameterSuite = static_cast<OfxParameterSuiteV1*>(
      const_cast<void*>(g_host->fetchSuite(g_host->host, kOfxParameterSuite, 1)));

  if (g_effectSuite == nullptr || g_propertySuite == nullptr ||
      g_parameterSuite == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }
  return kOfxStatOK;
}

OfxStatus mainEntry(const char* action,
                    const void* handle,
                    OfxPropertySetHandle inArgs,
                    OfxPropertySetHandle outArgs) {
  try {
    if (action == nullptr) {
      return kOfxStatErrValue;
    }

    const auto effect = reinterpret_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (std::strcmp(action, kOfxActionLoad) == 0) {
      return onLoad();
    }
    if (std::strcmp(action, kOfxActionDescribe) == 0) {
      return describe(effect);
    }
    if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
      return describeInContext(effect, inArgs);
    }
    if (std::strcmp(action, kOfxActionCreateInstance) == 0 ||
        std::strcmp(action, kOfxActionDestroyInstance) == 0 ||
        std::strcmp(action, kOfxActionBeginInstanceChanged) == 0 ||
        std::strcmp(action, kOfxActionEndInstanceChanged) == 0) {
      return kOfxStatOK;
    }
    if (std::strcmp(action, kOfxImageEffectActionRender) == 0) {
      return render(effect, inArgs, outArgs);
    }
    if (std::strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0) {
      return getClipPreferences(effect, inArgs, outArgs);
    }
    if (std::strcmp(action, kOfxImageEffectActionBeginSequenceRender) == 0) {
      return beginSequenceRender(effect, inArgs, outArgs);
    }
    if (std::strcmp(action, kOfxImageEffectActionEndSequenceRender) == 0) {
      return endSequenceRender(effect, inArgs, outArgs);
    }
    if (std::strcmp(action, kOfxActionInstanceChanged) == 0) {
      return instanceChanged(effect, inArgs, outArgs);
    }
  } catch (const std::bad_alloc&) {
    return kOfxStatErrMemory;
  } catch (...) {
    return kOfxStatErrUnknown;
  }

  return kOfxStatReplyDefault;
}

}  // namespace CorridorKey::OFX

extern "C" {

OfxExport int OfxGetNumberOfPlugins(void) {
  return 1;
}

OfxExport OfxPlugin* OfxGetPlugin(int nth) {
  static OfxPlugin plugin = {
      kOfxImageEffectPluginApi,
      1,
      CorridorKey::OFX::kPluginIdentifier,
      CorridorKey::OFX::kPluginVersionMajor,
      CorridorKey::OFX::kPluginVersionMinor,
      CorridorKey::OFX::setHost,
      CorridorKey::OFX::mainEntry,
  };

  if (nth == 0) {
    return &plugin;
  }
  return nullptr;
}

}  // extern "C"
