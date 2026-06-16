#include "CorridorKeyPlugin.h"

#include "ofxParam.h"

#include <algorithm>
#include <cstdarg>
#include <iostream>
#include <map>
#include <string>
#include <vector>

extern "C" {
int OfxGetNumberOfPlugins(void);
OfxPlugin* OfxGetPlugin(int nth);
}

namespace {

struct PropertySet {
  std::map<std::string, std::vector<std::string>> strings;
  std::map<std::string, std::vector<double>> doubles;
  std::map<std::string, std::vector<int>> ints;
};

struct Clip {
  PropertySet props;
};

struct Param {
  PropertySet props;
  std::string type;
  int intValue = 0;
  OfxTime lastRequestedTime = -1.0;
};

struct Effect {
  PropertySet props;
  std::map<std::string, Clip> clips;
  std::map<std::string, Param> params;
};

PropertySet* asProps(OfxPropertySetHandle handle) {
  return reinterpret_cast<PropertySet*>(handle);
}

Effect* asEffect(OfxImageEffectHandle handle) {
  return reinterpret_cast<Effect*>(handle);
}

Effect* asParamSet(OfxParamSetHandle handle) {
  return reinterpret_cast<Effect*>(handle);
}

OfxPropertySetHandle propsHandle(PropertySet& props) {
  return reinterpret_cast<OfxPropertySetHandle>(&props);
}

OfxImageEffectHandle effectHandle(Effect& effect) {
  return reinterpret_cast<OfxImageEffectHandle>(&effect);
}

OfxParamSetHandle paramSetHandle(Effect& effect) {
  return reinterpret_cast<OfxParamSetHandle>(&effect);
}

OfxStatus setString(OfxPropertySetHandle handle, const char* property, int index,
                    const char* value) {
  auto& values = asProps(handle)->strings[property];
  if (index < 0) {
    return kOfxStatErrBadIndex;
  }
  if (static_cast<int>(values.size()) <= index) {
    values.resize(static_cast<std::size_t>(index + 1));
  }
  values[static_cast<std::size_t>(index)] = value;
  return kOfxStatOK;
}

OfxStatus setInt(OfxPropertySetHandle handle, const char* property, int index, int value) {
  auto& values = asProps(handle)->ints[property];
  if (index < 0) {
    return kOfxStatErrBadIndex;
  }
  if (static_cast<int>(values.size()) <= index) {
    values.resize(static_cast<std::size_t>(index + 1));
  }
  values[static_cast<std::size_t>(index)] = value;
  return kOfxStatOK;
}

OfxStatus getString(OfxPropertySetHandle handle, const char* property, int index,
                    char** value) {
  auto& values = asProps(handle)->strings[property];
  if (index < 0 || static_cast<int>(values.size()) <= index) {
    return kOfxStatErrBadIndex;
  }
  *value = values[static_cast<std::size_t>(index)].data();
  return kOfxStatOK;
}

OfxStatus getDouble(OfxPropertySetHandle handle, const char* property, int index,
                    double* value) {
  auto& values = asProps(handle)->doubles[property];
  if (index < 0 || static_cast<int>(values.size()) <= index) {
    return kOfxStatErrBadIndex;
  }
  *value = values[static_cast<std::size_t>(index)];
  return kOfxStatOK;
}

OfxStatus getPropertySet(OfxImageEffectHandle effect, OfxPropertySetHandle* propHandle) {
  *propHandle = propsHandle(asEffect(effect)->props);
  return kOfxStatOK;
}

OfxStatus getParamSet(OfxImageEffectHandle effect, OfxParamSetHandle* paramSet) {
  *paramSet = paramSetHandle(*asEffect(effect));
  return kOfxStatOK;
}

OfxStatus clipDefine(OfxImageEffectHandle effect, const char* name,
                     OfxPropertySetHandle* propertySet) {
  Clip& clip = asEffect(effect)->clips[name];
  *propertySet = propsHandle(clip.props);
  return kOfxStatOK;
}

OfxStatus paramDefine(OfxParamSetHandle paramSet, const char* paramType,
                      const char* name, OfxPropertySetHandle* propertySet) {
  Param& param = asParamSet(paramSet)->params[name];
  param.type = paramType;
  if (propertySet != nullptr) {
    *propertySet = propsHandle(param.props);
  }
  return kOfxStatOK;
}

OfxStatus paramGetHandle(OfxParamSetHandle paramSet, const char* name,
                         OfxParamHandle* param,
                         OfxPropertySetHandle* props) {
  auto found = asParamSet(paramSet)->params.find(name);
  if (found == asParamSet(paramSet)->params.end()) {
    return kOfxStatErrBadHandle;
  }
  *param = reinterpret_cast<OfxParamHandle>(&found->second);
  if (props != nullptr) {
    *props = propsHandle(found->second.props);
  }
  return kOfxStatOK;
}

OfxStatus paramGetValueAtTime(OfxParamHandle paramHandle, OfxTime time, ...) {
  Param* param = reinterpret_cast<Param*>(paramHandle);
  if (param == nullptr) {
    return kOfxStatErrBadHandle;
  }
  param->lastRequestedTime = time;
  va_list args;
  va_start(args, time);
  int* value = va_arg(args, int*);
  *value = param->intValue;
  va_end(args);
  return kOfxStatOK;
}

OfxPropertySuiteV1 makePropertySuite() {
  OfxPropertySuiteV1 suite{};
  suite.propSetString = setString;
  suite.propSetInt = setInt;
  suite.propGetString = getString;
  suite.propGetDouble = getDouble;
  return suite;
}

OfxImageEffectSuiteV1 makeEffectSuite() {
  OfxImageEffectSuiteV1 suite{};
  suite.getPropertySet = getPropertySet;
  suite.getParamSet = getParamSet;
  suite.clipDefine = clipDefine;
  return suite;
}

OfxParameterSuiteV1 makeParameterSuite() {
  OfxParameterSuiteV1 suite{};
  suite.paramDefine = paramDefine;
  suite.paramGetHandle = paramGetHandle;
  suite.paramGetValueAtTime = paramGetValueAtTime;
  return suite;
}

OfxPropertySuiteV1 g_propertySuite = makePropertySuite();
OfxImageEffectSuiteV1 g_effectSuite = makeEffectSuite();
OfxParameterSuiteV1 g_parameterSuite = makeParameterSuite();

const void* fetchSuite(OfxPropertySetHandle, const char* suiteName, int version) {
  if (version != 1) {
    return nullptr;
  }
  if (std::string(suiteName) == kOfxPropertySuite) {
    return &g_propertySuite;
  }
  if (std::string(suiteName) == kOfxImageEffectSuite) {
    return &g_effectSuite;
  }
  if (std::string(suiteName) == kOfxParameterSuite) {
    return &g_parameterSuite;
  }
  return nullptr;
}

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "ofx clip preferences test failed: " << message << '\n';
    return false;
  }
  return true;
}

bool contains(const std::vector<std::string>& values, const char* value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool testContext(OfxPlugin* plugin, const char* context) {
  Effect descriptor;
  PropertySet contextArgs;
  setString(propsHandle(contextArgs), kOfxImageEffectPropContext, 0, context);

  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxActionDescribe, effectHandle(descriptor), nullptr,
                                 nullptr) == kOfxStatOK,
               "describe must succeed");
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                 effectHandle(descriptor), propsHandle(contextArgs),
                                 nullptr) == kOfxStatOK,
               "describe in context must succeed");

  Clip& output = descriptor.clips[kOfxImageEffectOutputClipName];
  ok &= expect(contains(output.props.strings[kOfxImageEffectPropSupportedComponents],
                        kOfxImageComponentRGBA),
               "Output must support RGBA for processed linear premultiplied pixels");
  return ok;
}

bool testOutputPremultiplicationPreference(OfxPlugin* plugin,
                                           int outputMode,
                                           const char* expected) {
  Effect instance;
  instance.params["ck_output_mode"] = Param{};
  instance.params["ck_output_mode"].type = kOfxParamTypeChoice;
  instance.params["ck_output_mode"].intValue = outputMode;

  PropertySet outArgs;
  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionGetClipPreferences,
                                 effectHandle(instance), nullptr, propsHandle(outArgs)) ==
                   kOfxStatOK,
               "clip preferences action should succeed");
  ok &= expect(outArgs.strings[kOfxImageEffectPropPreMultiplication].size() == 1,
               "clip preferences should declare output premultiplication");
  ok &= expect(outArgs.strings[kOfxImageEffectPropPreMultiplication][0] == expected,
               "output mode should declare matching premultiplication");
  return ok;
}

bool testClipPreferencesUseActionTime(OfxPlugin* plugin) {
  Effect instance;
  Param& outputMode = instance.params["ck_output_mode"];
  outputMode.type = kOfxParamTypeChoice;
  outputMode.intValue = 1;

  PropertySet inArgs;
  inArgs.doubles[kOfxPropTime] = {42.0};
  PropertySet outArgs;

  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionGetClipPreferences,
                                 effectHandle(instance), propsHandle(inArgs),
                                 propsHandle(outArgs)) == kOfxStatOK,
               "clip preferences action with time should succeed");
  ok &= expect(outputMode.lastRequestedTime == 42.0,
               "clip preferences should read output mode at action time");
  ok &= expect(outArgs.strings[kOfxImageEffectPropPreMultiplication][0] ==
                   kOfxImageUnPreMultiplied,
               "action-time output mode should drive premultiplication");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= expect(OfxGetNumberOfPlugins() == 1, "exactly one plugin should be exported");

  OfxPlugin* plugin = OfxGetPlugin(0);
  ok &= expect(plugin != nullptr, "first plugin should exist");
  OfxHost host{};
  host.fetchSuite = fetchSuite;
  plugin->setHost(&host);
  ok &= expect(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr) == kOfxStatOK,
               "load should fetch OFX suites");

  ok &= testContext(plugin, kOfxImageEffectContextFilter);
  ok &= testContext(plugin, kOfxImageEffectContextGeneral);
  ok &= testOutputPremultiplicationPreference(plugin, 0, kOfxImagePreMultiplied);
  ok &= testOutputPremultiplicationPreference(plugin, 1, kOfxImageUnPreMultiplied);
  ok &= testOutputPremultiplicationPreference(plugin, 2, kOfxImageUnPreMultiplied);
  ok &= testOutputPremultiplicationPreference(plugin, 3, kOfxImageUnPreMultiplied);
  ok &= testOutputPremultiplicationPreference(plugin, 4, kOfxImageOpaque);
  ok &= testOutputPremultiplicationPreference(plugin, 5, kOfxImageOpaque);
  ok &= testClipPreferencesUseActionTime(plugin);
  return ok ? 0 : 1;
}
