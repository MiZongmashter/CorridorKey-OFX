#include "CorridorKeyPlugin.h"

#include "ofxParam.h"

#include <algorithm>
#include <cstdarg>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

extern "C" {
int OfxGetNumberOfPlugins(void);
OfxPlugin* OfxGetPlugin(int nth);
}

namespace {

inline constexpr const char* kAlphaHintClipName = "AlphaHint";

struct PropertySet {
  std::map<std::string, std::vector<std::string>> strings;
  std::map<std::string, std::vector<int>> ints;
  std::map<std::string, std::vector<double>> doubles;
  std::map<std::string, std::vector<void*>> pointers;
};

struct Clip {
  PropertySet props;
  PropertySet* image = nullptr;
};

struct Param {
  PropertySet props;
  std::string name;
  std::string type;
  std::string stringValue;
  int intValue = 0;
};

struct Effect {
  PropertySet props;
  std::map<std::string, Clip> clips;
  std::map<std::string, Param> params;
};

std::vector<std::string> g_renderParamSetValueNames;
bool g_inRenderAction = false;

PropertySet* asProps(OfxPropertySetHandle handle) {
  return reinterpret_cast<PropertySet*>(handle);
}

Effect* asEffect(OfxImageEffectHandle handle) {
  return reinterpret_cast<Effect*>(handle);
}

Clip* asClip(OfxImageClipHandle handle) {
  return reinterpret_cast<Clip*>(handle);
}

Param* asParam(OfxParamHandle handle) {
  return reinterpret_cast<Param*>(handle);
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

OfxStatus getInt(OfxPropertySetHandle handle, const char* property, int index, int* value) {
  auto& values = asProps(handle)->ints[property];
  if (index < 0 || static_cast<int>(values.size()) <= index) {
    return kOfxStatErrBadIndex;
  }
  *value = values[static_cast<std::size_t>(index)];
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

OfxStatus getPointer(OfxPropertySetHandle handle, const char* property, int index,
                     void** value) {
  auto& values = asProps(handle)->pointers[property];
  if (index < 0 || static_cast<int>(values.size()) <= index) {
    return kOfxStatErrBadIndex;
  }
  *value = values[static_cast<std::size_t>(index)];
  return kOfxStatOK;
}

OfxStatus getIntN(OfxPropertySetHandle handle, const char* property, int count,
                  int* value) {
  auto& values = asProps(handle)->ints[property];
  if (count < 0 || static_cast<int>(values.size()) < count) {
    return kOfxStatErrBadIndex;
  }
  std::copy(values.begin(), values.begin() + count, value);
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

OfxStatus clipGetHandle(OfxImageEffectHandle effect, const char* name,
                        OfxImageClipHandle* clip, OfxPropertySetHandle* propertySet) {
  auto found = asEffect(effect)->clips.find(name);
  if (found == asEffect(effect)->clips.end()) {
    return kOfxStatErrBadHandle;
  }
  *clip = reinterpret_cast<OfxImageClipHandle>(&found->second);
  if (propertySet != nullptr) {
    *propertySet = propsHandle(found->second.props);
  }
  return kOfxStatOK;
}

OfxStatus clipGetImage(OfxImageClipHandle clip, OfxTime, const OfxRectD*,
                       OfxPropertySetHandle* imageHandle) {
  if (asClip(clip)->image == nullptr) {
    return kOfxStatFailed;
  }
  *imageHandle = propsHandle(*asClip(clip)->image);
  return kOfxStatOK;
}

OfxStatus clipReleaseImage(OfxPropertySetHandle) {
  return kOfxStatOK;
}

int abortRender(OfxImageEffectHandle) {
  return 0;
}

OfxStatus paramDefine(OfxParamSetHandle paramSet, const char* paramType,
                      const char* name, OfxPropertySetHandle* propertySet) {
  Param& param = asParamSet(paramSet)->params[name];
  param.name = name;
  param.type = paramType;
  if (propertySet != nullptr) {
    *propertySet = propsHandle(param.props);
  }
  return kOfxStatOK;
}

OfxStatus paramGetHandle(OfxParamSetHandle paramSet, const char* name,
                         OfxParamHandle* param,
                         OfxPropertySetHandle* propertySet) {
  auto found = asParamSet(paramSet)->params.find(name);
  if (found == asParamSet(paramSet)->params.end()) {
    return kOfxStatErrBadHandle;
  }
  *param = reinterpret_cast<OfxParamHandle>(&found->second);
  if (propertySet != nullptr) {
    *propertySet = propsHandle(found->second.props);
  }
  return kOfxStatOK;
}

OfxStatus paramSetValue(OfxParamHandle paramHandle, ...) {
  Param* param = asParam(paramHandle);
  if (param == nullptr) {
    return kOfxStatErrBadHandle;
  }
  if (g_inRenderAction) {
    g_renderParamSetValueNames.push_back(param->name);
  }

  va_list args;
  va_start(args, paramHandle);
  if (param->type == kOfxParamTypeString) {
    const char* value = va_arg(args, const char*);
    param->stringValue = value == nullptr ? "" : value;
  }
  va_end(args);
  return kOfxStatOK;
}

OfxStatus paramGetValueAtTime(OfxParamHandle paramHandle, OfxTime time, ...) {
  Param* param = asParam(paramHandle);
  if (param == nullptr) {
    return kOfxStatErrBadHandle;
  }
  (void)time;

  va_list args;
  va_start(args, time);
  if (param->type == kOfxParamTypeChoice) {
    int* value = va_arg(args, int*);
    *value = param->intValue;
  }
  va_end(args);
  return kOfxStatOK;
}

OfxPropertySuiteV1 makePropertySuite() {
  OfxPropertySuiteV1 suite{};
  suite.propSetString = setString;
  suite.propSetInt = setInt;
  suite.propGetPointer = getPointer;
  suite.propGetString = getString;
  suite.propGetDouble = getDouble;
  suite.propGetInt = getInt;
  suite.propGetIntN = getIntN;
  return suite;
}

OfxImageEffectSuiteV1 makeEffectSuite() {
  OfxImageEffectSuiteV1 suite{};
  suite.getPropertySet = getPropertySet;
  suite.getParamSet = getParamSet;
  suite.clipDefine = clipDefine;
  suite.clipGetHandle = clipGetHandle;
  suite.clipGetImage = clipGetImage;
  suite.clipReleaseImage = clipReleaseImage;
  suite.abort = abortRender;
  return suite;
}

OfxParameterSuiteV1 makeParameterSuite() {
  OfxParameterSuiteV1 suite{};
  suite.paramDefine = paramDefine;
  suite.paramGetHandle = paramGetHandle;
  suite.paramSetValue = paramSetValue;
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

void putDouble(PropertySet& props, const char* property, std::vector<double> values) {
  props.doubles[property] = std::move(values);
}

void putInt(PropertySet& props, const char* property, std::vector<int> values) {
  props.ints[property] = std::move(values);
}

void putPointer(PropertySet& props, const char* property, void* value) {
  props.pointers[property] = {value};
}

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ofx general context test failed: " << message << '\n';
    return false;
  }
  return true;
}

bool contains(const std::vector<std::string>& values, const char* value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

void beginRenderTracking() {
  g_renderParamSetValueNames.clear();
  g_inRenderAction = true;
}

void endRenderTracking() {
  g_inRenderAction = false;
}

bool expectOnlyGuideStatusWrite(const std::string& message) {
  return expect(g_renderParamSetValueNames ==
                    std::vector<std::string>{"ck_guide_source_status"},
                message);
}

std::string guideSourceStatus(const Effect& instance) {
  const auto found = instance.params.find("ck_guide_source_status");
  return found == instance.params.end() ? "" : found->second.stringValue;
}

void defineRuntimeStatusParams(Effect& instance) {
  const std::vector<std::string> names = {
      "ck_runtime_state",
      "ck_checkpoint_version",
      "ck_model_status",
      "ck_model_source_status",
      "ck_install_status",
      "ck_backend",
      "ck_compute_device",
      "ck_memory",
      "ck_cache",
      "ck_warmup",
      "ck_last_error",
      "ck_guide_source_status",
  };
  for (const std::string& name : names) {
    Param& param = instance.params[name];
    param = Param{};
    param.name = name;
    param.type = kOfxParamTypeString;
  }
}

void defineAlphaChoice(Effect& instance, int value) {
  Param& param = instance.params["ck_alpha_hint_source"];
  param = Param{};
  param.name = "ck_alpha_hint_source";
  param.type = kOfxParamTypeChoice;
  param.intValue = value;
}

void defineOutputModeChoice(Effect& instance, int value) {
  Param& param = instance.params["ck_output_mode"];
  param = Param{};
  param.name = "ck_output_mode";
  param.type = kOfxParamTypeChoice;
  param.intValue = value;
}

PropertySet makeFloatImage(float* data, int components, int width, int height) {
  PropertySet image;
  setString(propsHandle(image), kOfxImageEffectPropComponents, 0,
            components == 4 ? kOfxImageComponentRGBA
                            : (components == 3 ? kOfxImageComponentRGB
                                               : kOfxImageComponentAlpha));
  setString(propsHandle(image), kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthFloat);
  putPointer(image, kOfxImagePropData, data);
  putInt(image, kOfxImagePropRowBytes,
         {width * components * static_cast<int>(sizeof(float))});
  putInt(image, kOfxImagePropBounds, {0, 0, width, height});
  return image;
}

bool testDescribeGeneralContext(OfxPlugin* plugin) {
  Effect descriptor;
  PropertySet generalContext;
  setString(propsHandle(generalContext), kOfxImageEffectPropContext, 0,
            kOfxImageEffectContextGeneral);

  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxActionDescribe, effectHandle(descriptor), nullptr,
                                 nullptr) == kOfxStatOK,
               "describe should succeed");
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                 effectHandle(descriptor), propsHandle(generalContext),
                                 nullptr) == kOfxStatOK,
               "describe in General context should succeed");

  const auto& contexts = descriptor.props.strings[kOfxImageEffectPropSupportedContexts];
  ok &= expect(contains(contexts, kOfxImageEffectContextFilter),
               "Filter context should remain supported");
  ok &= expect(contains(contexts, kOfxImageEffectContextGeneral),
               "General context should be supported");
  ok &= expect(descriptor.clips.count(kOfxImageEffectOutputClipName) == 1,
               "Output clip should be defined");
  ok &= expect(descriptor.clips.count(kOfxImageEffectSimpleSourceClipName) == 1,
               "Source clip should be defined");
  ok &= expect(descriptor.clips.count(kAlphaHintClipName) == 1,
               "AlphaHint clip should be defined in General context");
  ok &= expect(descriptor.clips[kAlphaHintClipName]
                   .props.ints[kOfxImageClipPropOptional][0] == 1,
               "AlphaHint clip should be optional");
  ok &= expect(contains(descriptor.clips[kAlphaHintClipName]
                            .props.strings[kOfxImageEffectPropSupportedComponents],
                        kOfxImageComponentRGBA),
               "AlphaHint clip should support RGBA alpha");
  ok &= expect(contains(descriptor.clips[kAlphaHintClipName]
                            .props.strings[kOfxImageEffectPropSupportedComponents],
                        kOfxImageComponentRGB),
               "AlphaHint clip should support RGB red-channel hints");
  ok &= expect(contains(descriptor.clips[kAlphaHintClipName]
                            .props.strings[kOfxImageEffectPropSupportedComponents],
                        kOfxImageComponentAlpha),
               "AlphaHint clip should support single-channel alpha mattes");
  ok &= expect(descriptor.params.count("ck_alpha_hint_source") == 1,
               "Alpha Hint Source parameter should be defined");
  ok &= expect(descriptor.params["ck_alpha_hint_source"]
                   .props.strings[kOfxParamPropChoiceOption][0] == "External",
               "Alpha Hint Source should expose External choice");
  ok &= expect(descriptor.params["ck_alpha_hint_source"]
                   .props.strings[kOfxParamPropChoiceOption][2] == "Red Channel",
               "Alpha Hint Source should expose Red Channel choice");
  ok &= expect(descriptor.params.count("ck_guide_source_status") == 1,
               "Guide Source Status field should be defined");
  ok &= expect(descriptor.params.count("Main") == 1,
               "General context should expose the Main parameter page");
  ok &= expect(contains(descriptor.params["Main"].props.strings[kOfxParamPropPageChild],
                        "ck_alpha_hint_source"),
               "Main page should expose General context controls");
  ok &= expect(contains(descriptor.params["Main"].props.strings[kOfxParamPropPageChild],
                        "ck_guide_source_status"),
               "Main page should expose Guide Source Status");
  ok &= expect(descriptor.props.strings[kOfxPluginPropParamPageOrder][0] == "Main",
               "General context should declare Main as the visible page order");
  return ok;
}

bool testDescribeResolveFusionGeneralContext(OfxPlugin* plugin) {
  Effect descriptor;
  PropertySet generalContext;
  setString(propsHandle(generalContext), kOfxImageEffectPropContext, 0,
            "OfxImageEffectContextGeneral_b44ab5d5-7c10-4abb-9b65-ff91d00469fd_1");

  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxActionDescribe, effectHandle(descriptor), nullptr,
                                 nullptr) == kOfxStatOK,
               "describe should succeed");
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                 effectHandle(descriptor), propsHandle(generalContext),
                                 nullptr) == kOfxStatOK,
               "Resolve Fusion suffixed General context should succeed");
  ok &= expect(descriptor.clips.count(kAlphaHintClipName) == 1,
               "suffixed General context should define AlphaHint clip");
  ok &= expect(descriptor.params.count("Main") == 1,
               "suffixed General context should expose the Main parameter page");
  ok &= expect(contains(descriptor.params["Main"].props.strings[kOfxParamPropPageChild],
                        "ck_output_mode"),
               "suffixed General Main page should expose output controls");
  ok &= expect(descriptor.props.strings[kOfxPluginPropParamPageOrder][0] == "Main",
               "suffixed General context should declare Main page order");
  return ok;
}

bool testFilterContextDoesNotDefineAlphaHint(OfxPlugin* plugin) {
  Effect descriptor;
  PropertySet filterContext;
  setString(propsHandle(filterContext), kOfxImageEffectPropContext, 0,
            kOfxImageEffectContextFilter);

  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxActionDescribe, effectHandle(descriptor), nullptr,
                                 nullptr) == kOfxStatOK,
               "describe should succeed for filter test");
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                 effectHandle(descriptor), propsHandle(filterContext),
                                 nullptr) == kOfxStatOK,
               "describe in Filter context should succeed");
  ok &= expect(descriptor.clips.count(kAlphaHintClipName) == 0,
               "Filter context should not define AlphaHint");
  return ok;
}

bool testRenderUpdatesExternalStatus(OfxPlugin* plugin) {
  Effect instance;
  defineRuntimeStatusParams(instance);
  defineAlphaChoice(instance, 0);
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};
  instance.clips[kAlphaHintClipName] = Clip{};

  constexpr int width = 2;
  constexpr int height = 1;
  std::vector<float> source = {
      0.1F, 0.2F, 0.3F, 0.4F,
      0.5F, 0.6F, 0.7F, 0.8F,
  };
  std::vector<float> output(source.size(), -1.0F);
  std::vector<float> alphaHint = {
      0.0F, 0.0F, 0.0F, 0.25F,
      0.0F, 0.0F, 0.0F, 0.75F,
  };

  PropertySet sourceImage = makeFloatImage(source.data(), 4, width, height);
  PropertySet outputImage = makeFloatImage(output.data(), 4, width, height);
  PropertySet alphaImage = makeFloatImage(alphaHint.data(), 4, width, height);
  instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;
  instance.clips[kAlphaHintClipName].image = &alphaImage;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {7.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

  bool ok = true;
  beginRenderTracking();
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "General render with external AlphaHint should succeed");
  endRenderTracking();
  ok &= expect(output == source, "general-context render should preserve passthrough output");
  ok &= expect(guideSourceStatus(instance) == "external present",
               "external AlphaHint render should update Guide Source Status");
  ok &= expectOnlyGuideStatusWrite(
      "external AlphaHint render should only update Guide Source Status");
  return ok;
}

bool testRenderUsesSingleChannelAlphaHint(OfxPlugin* plugin) {
  Effect instance;
  defineRuntimeStatusParams(instance);
  defineAlphaChoice(instance, 0);
  defineOutputModeChoice(instance, 3);
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};
  instance.clips[kAlphaHintClipName] = Clip{};

  constexpr int width = 2;
  constexpr int height = 1;
  std::vector<float> source = {
      0.1F, 0.2F, 0.3F, 0.4F,
      0.5F, 0.6F, 0.7F, 0.8F,
  };
  std::vector<float> alphaHint = {0.2F, 0.6F};
  std::vector<float> output(source.size(), -1.0F);

  PropertySet sourceImage = makeFloatImage(source.data(), 4, width, height);
  PropertySet alphaImage = makeFloatImage(alphaHint.data(), 1, width, height);
  PropertySet outputImage = makeFloatImage(output.data(), 4, width, height);
  instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
  instance.clips[kAlphaHintClipName].image = &alphaImage;
  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {7.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

  bool ok = true;
  beginRenderTracking();
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "single-channel AlphaHint view render should succeed");
  endRenderTracking();
  ok &= expect(output == std::vector<float>({0.2F, 0.2F, 0.2F, 0.2F,
                                             0.6F, 0.6F, 0.6F, 0.6F}),
               "single-channel AlphaHint should drive Alpha Hint View");
  ok &= expect(guideSourceStatus(instance) == "external present",
               "single-channel AlphaHint should report external status");
  ok &= expectOnlyGuideStatusWrite(
      "single-channel AlphaHint render should only update Guide Source Status");
  return ok;
}

bool testRenderUpdatesMissingFallbackStatus(OfxPlugin* plugin) {
  Effect instance;
  defineRuntimeStatusParams(instance);
  defineAlphaChoice(instance, 0);
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};
  instance.clips[kAlphaHintClipName] = Clip{};

  constexpr int width = 1;
  constexpr int height = 1;
  std::vector<float> source = {0.1F, 0.2F, 0.3F, 0.4F};
  std::vector<float> output(source.size(), -1.0F);
  PropertySet sourceImage = makeFloatImage(source.data(), 4, width, height);
  PropertySet outputImage = makeFloatImage(output.data(), 4, width, height);
  instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;
  instance.clips[kAlphaHintClipName].image = nullptr;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {7.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

  bool ok = true;
  beginRenderTracking();
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "missing AlphaHint render should succeed with fallback status");
  endRenderTracking();
  ok &= expect(output == source,
               "missing AlphaHint fallback should not change passthrough output");
  ok &= expect(guideSourceStatus(instance) == "missing hint; rough fallback used",
               "missing AlphaHint should update fallback status");
  ok &= expectOnlyGuideStatusWrite(
      "missing AlphaHint render should only update Guide Source Status");
  return ok;
}

bool testRenderAcceptsBlackRedChannelMatte(OfxPlugin* plugin) {
  Effect instance;
  defineRuntimeStatusParams(instance);
  defineAlphaChoice(instance, 2);
  defineOutputModeChoice(instance, 3);
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};
  instance.clips[kAlphaHintClipName] = Clip{};

  constexpr int width = 1;
  constexpr int height = 1;
  std::vector<float> source = {0.1F, 0.2F, 0.3F, 0.4F};
  std::vector<float> output(source.size(), -1.0F);
  std::vector<float> alphaHint = {0.0F, 0.9F, 0.8F};
  PropertySet sourceImage = makeFloatImage(source.data(), 4, width, height);
  PropertySet outputImage = makeFloatImage(output.data(), 4, width, height);
  PropertySet alphaImage = makeFloatImage(alphaHint.data(), 3, width, height);
  instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;
  instance.clips[kAlphaHintClipName].image = &alphaImage;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {7.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

  bool ok = true;
  beginRenderTracking();
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "black red-channel hint render should succeed");
  endRenderTracking();
  ok &= expect(output == std::vector<float>({0.0F, 0.0F, 0.0F, 0.0F}),
               "black red-channel hint should drive Alpha Hint View without luminance");
  ok &= expect(guideSourceStatus(instance) == "red channel used",
               "red-channel render should update Guide Source Status");
  ok &= expectOnlyGuideStatusWrite(
      "red-channel render should only update Guide Source Status");
  return ok;
}

bool testRenderUsesSourceAlphaStatus(OfxPlugin* plugin) {
  Effect instance;
  defineRuntimeStatusParams(instance);
  defineAlphaChoice(instance, 1);
  defineOutputModeChoice(instance, 3);
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};
  instance.clips[kAlphaHintClipName] = Clip{};

  constexpr int width = 1;
  constexpr int height = 1;
  std::vector<float> source = {0.1F, 0.2F, 0.3F, 0.4F};
  std::vector<float> output(source.size(), -1.0F);
  PropertySet sourceImage = makeFloatImage(source.data(), 4, width, height);
  PropertySet outputImage = makeFloatImage(output.data(), 4, width, height);
  instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;
  instance.clips[kAlphaHintClipName].image = nullptr;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {7.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

  bool ok = true;
  beginRenderTracking();
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "source alpha hint render should succeed");
  endRenderTracking();
  ok &= expect(output == std::vector<float>({0.4F, 0.4F, 0.4F, 0.4F}),
               "source alpha should drive Alpha Hint View");
  ok &= expect(guideSourceStatus(instance) == "source alpha used",
               "source alpha should update Guide Source Status");
  ok &= expectOnlyGuideStatusWrite(
      "source alpha render should only update Guide Source Status");
  return ok;
}

bool testRenderUsesRoughFallbackStatus(OfxPlugin* plugin) {
  Effect instance;
  defineRuntimeStatusParams(instance);
  defineAlphaChoice(instance, 3);
  defineOutputModeChoice(instance, 3);
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};
  instance.clips[kAlphaHintClipName] = Clip{};

  constexpr int width = 1;
  constexpr int height = 1;
  std::vector<float> source = {0.1F, 0.2F, 0.3F, 0.4F};
  std::vector<float> output(source.size(), -1.0F);
  PropertySet sourceImage = makeFloatImage(source.data(), 4, width, height);
  PropertySet outputImage = makeFloatImage(output.data(), 4, width, height);
  instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;
  instance.clips[kAlphaHintClipName].image = nullptr;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {7.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

  bool ok = true;
  beginRenderTracking();
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "rough fallback hint render should succeed");
  endRenderTracking();
  ok &= expect(output == std::vector<float>({1.0F, 1.0F, 1.0F, 1.0F}),
               "rough fallback should drive deterministic Alpha Hint View");
  ok &= expect(guideSourceStatus(instance) == "rough fallback used",
               "rough fallback should update Guide Source Status");
  ok &= expectOnlyGuideStatusWrite(
      "rough fallback render should only update Guide Source Status");
  return ok;
}

bool testRenderUpdatesInvalidFallbackStatus(OfxPlugin* plugin) {
  Effect instance;
  defineRuntimeStatusParams(instance);
  defineAlphaChoice(instance, 0);
  defineOutputModeChoice(instance, 3);
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};
  instance.clips[kAlphaHintClipName] = Clip{};

  constexpr int width = 1;
  constexpr int height = 1;
  std::vector<float> source = {0.1F, 0.2F, 0.3F, 0.4F};
  std::vector<float> output(source.size(), -1.0F);
  std::vector<float> rgbHint = {0.2F, 0.3F, 0.4F};
  PropertySet sourceImage = makeFloatImage(source.data(), 4, width, height);
  PropertySet outputImage = makeFloatImage(output.data(), 4, width, height);
  PropertySet alphaImage = makeFloatImage(rgbHint.data(), 3, width, height);
  instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;
  instance.clips[kAlphaHintClipName].image = &alphaImage;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {7.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

  bool ok = true;
  beginRenderTracking();
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "invalid external AlphaHint render should succeed with fallback");
  endRenderTracking();
  ok &= expect(output == std::vector<float>({1.0F, 1.0F, 1.0F, 1.0F}),
               "invalid external AlphaHint should use deterministic fallback");
  ok &= expect(guideSourceStatus(instance) == "invalid hint; rough fallback used",
               "invalid external AlphaHint should update fallback status");
  ok &= expectOnlyGuideStatusWrite(
      "invalid external AlphaHint render should only update Guide Source Status");
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

  ok &= testDescribeGeneralContext(plugin);
  ok &= testDescribeResolveFusionGeneralContext(plugin);
  ok &= testFilterContextDoesNotDefineAlphaHint(plugin);
  ok &= testRenderUpdatesExternalStatus(plugin);
  ok &= testRenderUsesSingleChannelAlphaHint(plugin);
  ok &= testRenderUpdatesMissingFallbackStatus(plugin);
  ok &= testRenderAcceptsBlackRedChannelMatte(plugin);
  ok &= testRenderUsesSourceAlphaStatus(plugin);
  ok &= testRenderUsesRoughFallbackStatus(plugin);
  ok &= testRenderUpdatesInvalidFallbackStatus(plugin);

  return ok ? 0 : 1;
}
