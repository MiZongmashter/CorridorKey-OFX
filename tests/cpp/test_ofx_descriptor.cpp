#include "CorridorKeyPlugin.h"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

extern "C" {
int OfxGetNumberOfPlugins(void);
OfxPlugin* OfxGetPlugin(int nth);
}

int g_runtimeProbeCalls = 0;
int g_stubInferCalls = 0;
int g_warmupCalls = 0;
int g_lastRuntimeProbeTimeoutMs = 0;
int g_lastStubInferTimeoutMs = 0;
int g_lastWarmupTimeoutMs = 0;

struct RuntimeStatusWire {
  bool ok = false;
  bool restarted = false;
  char state[64] = {};
  char checkpoint[64] = {};
  char modelStatus[64] = {};
  char modelSourceStatus[64] = {};
  char installStatus[64] = {};
  char downloadStatus[64] = {};
  char downloadProgress[64] = {};
  char compute[64] = {};
  char backend[64] = {};
  char backendStatus[64] = {};
  char warmup[64] = {};
  char cache[64] = {};
  char memory[64] = {};
  char errorCode[64] = {};
  char lastError[512] = {};
};

struct RuntimeInferWire {
  bool ok = false;
  char processedPath[1024] = {};
  char straightPath[1024] = {};
  char alphaPath[1024] = {};
  char jobId[128] = {};
  char backend[64] = {};
  char backendStatus[64] = {};
  char checkpoint[64] = {};
  char modelStatus[64] = {};
  char modelSourceStatus[64] = {};
  char installStatus[64] = {};
  char cache[64] = {};
  char errorCode[64] = {};
  char lastError[512] = {};
  char requestedQuality[64] = {};
  char effectiveQuality[64] = {};
  char queueTimeMs[64] = {};
  char oom[16] = {};
  char downgradedQuality[16] = {};
  char finalFailure[16] = {};
};

void copyWireString(char* destination, std::size_t size, const char* value) {
  if (size == 0) {
    return;
  }
  std::strncpy(destination, value, size - 1);
  destination[size - 1] = '\0';
}

extern "C" int CorridorKeyProbeRuntimeStatus(const char*, const char*,
                                             int timeoutMilliseconds,
                                             void* outputBuffer,
                                             std::size_t outputSize) {
  ++g_runtimeProbeCalls;
  g_lastRuntimeProbeTimeoutMs = timeoutMilliseconds;
  RuntimeStatusWire result{};
  copyWireString(result.state, sizeof(result.state), "error");
  copyWireString(result.errorCode, sizeof(result.errorCode), "runtime_unavailable");
  copyWireString(result.lastError, sizeof(result.lastError), "sidecar unavailable");
  if (outputBuffer != nullptr && outputSize > 0) {
    std::memset(outputBuffer, 0, outputSize);
    std::memcpy(outputBuffer, &result, std::min(outputSize, sizeof(result)));
  }
  return 0;
}

extern "C" int CorridorKeyRunStubInfer(const char*, const char*,
                                       int timeoutMilliseconds, const char*,
                                       const char*, int, int, int, int, const char*,
                                       const char*, int, int, int, int, double, int,
                                       int, int (*)(void*), void*, void* outputBuffer,
                                       std::size_t outputSize) {
  ++g_stubInferCalls;
  g_lastStubInferTimeoutMs = timeoutMilliseconds;
  RuntimeInferWire result{};
  copyWireString(result.errorCode, sizeof(result.errorCode), "runtime_unavailable");
  copyWireString(result.lastError, sizeof(result.lastError), "sidecar unavailable");
  if (outputBuffer != nullptr && outputSize > 0) {
    std::memset(outputBuffer, 0, outputSize);
    std::memcpy(outputBuffer, &result, std::min(outputSize, sizeof(result)));
  }
  return 0;
}

extern "C" int CorridorKeyRunWarmup(const char*, const char*,
                                    int timeoutMilliseconds, const char*, int, int,
                                    int (*)(void*), void*, void*, std::size_t) {
  ++g_warmupCalls;
  g_lastWarmupTimeoutMs = timeoutMilliseconds;
  return 0;
}

namespace {

constexpr const char* kAlphaHintClipName = "AlphaHint";

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
  std::string type;
  std::string value;
  int intValue = 0;
  double doubleValue = 0.0;
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

Clip* asClip(OfxImageClipHandle handle) {
  return reinterpret_cast<Clip*>(handle);
}

Param* asParam(OfxParamHandle handle) {
  return reinterpret_cast<Param*>(handle);
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

Effect* asParamSet(OfxParamSetHandle handle) {
  return reinterpret_cast<Effect*>(handle);
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

OfxStatus setDouble(OfxPropertySetHandle handle, const char* property, int index,
                    double value) {
  auto& values = asProps(handle)->doubles[property];
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
  va_list args;
  va_start(args, paramHandle);
  if (param->type == kOfxParamTypeString) {
    const char* value = va_arg(args, const char*);
    param->value = value == nullptr ? "" : value;
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
  } else if (param->type == kOfxParamTypeDouble) {
    double* value = va_arg(args, double*);
    *value = param->doubleValue;
  }
  va_end(args);
  return kOfxStatOK;
}

OfxPropertySuiteV1 makePropertySuite() {
  OfxPropertySuiteV1 suite{};
  suite.propSetString = setString;
  suite.propSetInt = setInt;
  suite.propSetDouble = setDouble;
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
bool g_returnParameterSuite = true;

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
    if (!g_returnParameterSuite) {
      return nullptr;
    }
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

void defineStringParam(Effect& effect, const std::string& name) {
  effect.params[name] = Param{};
  effect.params[name].type = kOfxParamTypeString;
}

void defineChoiceParam(Effect& effect, const std::string& name, int value) {
  effect.params[name] = Param{};
  effect.params[name].type = kOfxParamTypeChoice;
  effect.params[name].intValue = value;
}

void defineDoubleParam(Effect& effect, const std::string& name, double value) {
  effect.params[name] = Param{};
  effect.params[name].type = kOfxParamTypeDouble;
  effect.params[name].doubleValue = value;
}

void defineRuntimeRenderParams(Effect& effect) {
  for (const std::string& name : {
           "ck_runtime_state",
           "ck_checkpoint_version",
           "ck_model_status",
           "ck_model_source_status",
           "ck_install_status",
           "ck_backend_status",
           "ck_compute_device",
           "ck_memory",
           "ck_cache",
           "ck_warmup",
           "ck_last_error",
           "ck_guide_source_status",
           "ck_effective_quality",
       }) {
    defineStringParam(effect, name);
  }
  defineChoiceParam(effect, "ck_output_mode", 0);
  defineChoiceParam(effect, "ck_alpha_hint_source", 0);
  defineChoiceParam(effect, "ck_screen_color", 0);
  defineChoiceParam(effect, "ck_quality", 1);
  defineChoiceParam(effect, "ck_input_color_space", 0);
  defineChoiceParam(effect, "ck_backend", 0);
  defineDoubleParam(effect, "ck_despill_strength", 5.0);
}

void putPointer(PropertySet& props, const char* property, void* value);

PropertySet makeFloatRgbaImage(float* pixels, int width, int height) {
  PropertySet image;
  setString(propsHandle(image), kOfxImageEffectPropComponents, 0,
            kOfxImageComponentRGBA);
  setString(propsHandle(image), kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthFloat);
  putPointer(image, kOfxImagePropData, pixels);
  putInt(image, kOfxImagePropRowBytes, {width * 4 * static_cast<int>(sizeof(float))});
  putInt(image, kOfxImagePropBounds, {0, 0, width, height});
  return image;
}

PropertySet changedParamArgs(const char* name) {
  PropertySet args;
  setString(propsHandle(args), kOfxPropName, 0, name);
  return args;
}

void putPointer(PropertySet& props, const char* property, void* value) {
  props.pointers[property] = {value};
}

void setEnv(const char* key, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(key, value.c_str());
#else
  setenv(key, value.c_str(), 1);
#endif
}

void unsetEnv(const char* key) {
#if defined(_WIN32)
  _putenv_s(key, "");
#else
  unsetenv(key);
#endif
}

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ofx descriptor test failed: " << message << '\n';
    return false;
  }
  return true;
}

bool contains(const std::vector<std::string>& values, const char* value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool testDescriptorAndScannerSafety(OfxPlugin* plugin) {
  const int runtimeProbeCallsBefore = g_runtimeProbeCalls;
  const int stubInferCallsBefore = g_stubInferCalls;
  const int warmupCallsBefore = g_warmupCalls;
  Effect globalDescriptor;

  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxActionDescribe, effectHandle(globalDescriptor), nullptr,
                                 nullptr) == kOfxStatOK,
               "describe should succeed");
  std::vector<Effect> contextDescriptors(2);
  const std::vector<const char*> scannerContexts = {
      kOfxImageEffectContextFilter,
      kOfxImageEffectContextGeneral,
  };

  for (std::size_t index = 0; index < scannerContexts.size(); ++index) {
    PropertySet context;
    setString(propsHandle(context), kOfxImageEffectPropContext, 0,
              scannerContexts[index]);
    ok &= expect(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                   effectHandle(contextDescriptors[index]),
                                   propsHandle(context), nullptr) == kOfxStatOK,
                 std::string("describe in scanner context should succeed: ") +
                     scannerContexts[index]);
  }

  Effect& contextDescriptor = contextDescriptors[0];

  const auto& contexts =
      globalDescriptor.props.strings[kOfxImageEffectPropSupportedContexts];
  ok &= expect(contains(contexts, kOfxImageEffectContextFilter),
               "Filter context must be supported");
  ok &= expect(contains(contexts, kOfxImageEffectContextGeneral),
               "General context must be supported for hosts that scan it");
  ok &= expect(!contains(contexts, kOfxImageEffectContextTransition),
               "Transition context must not be required");
  ok &= expect(globalDescriptor.props.strings[kOfxPropLabel][0] ==
                   CorridorKey::OFX::kPluginLabel,
               "plugin label should be stable");
  ok &= expect(globalDescriptor.props.strings[kOfxImageEffectPluginPropGrouping][0] ==
                   CorridorKey::OFX::kPluginGrouping,
               "plugin grouping should be stable");
  ok &= expect(globalDescriptor.props.ints[kOfxImageEffectPropSupportsTiles][0] == 0,
               "global descriptor must disable tiled renders for sidecar inference");
  ok &= expect(globalDescriptor.props.ints[kOfxImageEffectPluginPropHostFrameThreading][0] == 0,
               "global descriptor must disable host frame threading");
  ok &= expect(globalDescriptor.props.strings[kOfxImageEffectPluginRenderThreadSafety][0] ==
                   kOfxImageEffectRenderUnsafe,
               "global descriptor must serialize render calls");
  ok &= expect(globalDescriptor.props.strings.count(
                    kOfxImageEffectPropClipPreferencesSlaveParam) == 0,
               "scanner-safe descriptor should not force early Resolve param lookup");
  ok &= expect(contextDescriptor.props.ints[kOfxImageEffectPropSupportsTiles][0] == 0,
               "context descriptor must disable tiled renders");
  ok &= expect(contextDescriptor.props.ints[kOfxImageEffectPluginPropHostFrameThreading][0] == 0,
               "context descriptor must disable host frame threading");
  ok &= expect(contextDescriptor.props.strings[kOfxImageEffectPluginRenderThreadSafety][0] ==
                   kOfxImageEffectRenderUnsafe,
               "context descriptor must serialize render calls");
  ok &= expect(contains(contextDescriptor.props.strings[
                           kOfxImageEffectPropClipPreferencesSlaveParam],
                        "ck_output_mode"),
               "output mode should refresh clip preferences for premultiplication");
  ok &= expect(globalDescriptor.params.empty(),
               "global describe should leave context parameters to DescribeInContext");
  ok &= expect(contextDescriptor.clips.count(kOfxImageEffectOutputClipName) == 1,
               "Output clip should be defined");
  ok &= expect(contextDescriptor.clips.count(kOfxImageEffectSimpleSourceClipName) == 1,
               "Source clip should be defined");
  ok &= expect(contextDescriptor.clips[kOfxImageEffectOutputClipName]
                   .props.ints[kOfxImageEffectPropSupportsTiles][0] == 0,
               "Output clip should request full-frame images");
  ok &= expect(contextDescriptor.clips[kOfxImageEffectSimpleSourceClipName]
                   .props.ints[kOfxImageEffectPropSupportsTiles][0] == 0,
               "Source clip should request full-frame images");
  ok &= expect(contextDescriptors[1].clips[kAlphaHintClipName]
                   .props.ints[kOfxImageEffectPropSupportsTiles][0] == 0,
               "AlphaHint clip should request full-frame images");
  ok &= expect(contextDescriptor.params.count("ck_runtime_state") == 1,
               "runtime state status field should be defined");
  ok &= expect(contextDescriptor.params.count("ck_checkpoint_version") == 1,
               "checkpoint version status field should be defined");
  ok &= expect(contextDescriptor.params.count("ck_backend_status") == 1,
               "backend status field should be defined");
  ok &= expect(contextDescriptor.params.count("ck_compute_device") == 1,
               "compute status field should be defined");
  ok &= expect(contextDescriptor.params.count("ck_memory") == 1,
               "memory status field should be defined");
  ok &= expect(contextDescriptor.params.count("ck_cache") == 1,
               "cache status field should be defined");
  ok &= expect(contextDescriptor.params.count("ck_warmup") == 1,
               "warmup status field should be defined");
  ok &= expect(contextDescriptor.params.count("ck_last_error") == 1,
               "last error status field should be defined");
  ok &= expect(contextDescriptor.params.count("Main") == 1,
               "Resolve should receive an explicit Main parameter page");
  ok &= expect(contains(contextDescriptor.params["Main"].props.strings[kOfxParamPropPageChild],
                        "ck_output_mode"),
               "Main page should expose output controls");
  ok &= expect(contains(contextDescriptor.params["Main"].props.strings[kOfxParamPropPageChild],
                        "ck_model_status"),
               "Main page should expose model status");
  ok &= expect(contains(contextDescriptor.params["Main"].props.strings[kOfxParamPropPageChild],
                        "ck_download_status"),
               "Main page should expose download status");
  ok &= expect(contextDescriptor.props.strings[kOfxPluginPropParamPageOrder][0] == "Main",
               "context descriptor should declare Main as the visible page order");
  ok &= expect(g_runtimeProbeCalls == runtimeProbeCallsBefore,
               "describe/scan must not probe runtime status or import Python");
  ok &= expect(g_stubInferCalls == stubInferCallsBefore,
               "describe/scan must not launch sidecar inference");
  ok &= expect(g_warmupCalls == warmupCallsBefore,
               "describe/scan must not warm up models or query compute state");
  return ok;
}

bool testMissingParameterSuiteFailsLoad(OfxPlugin* plugin, OfxHost& host) {
  g_returnParameterSuite = false;
  plugin->setHost(&host);
  const bool ok = expect(
      plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr) ==
          kOfxStatErrMissingHostFeature,
      "load should fail instead of exposing a zero-parameter descriptor");
  g_returnParameterSuite = true;
  plugin->setHost(&host);
  return ok && expect(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr) ==
                          kOfxStatOK,
                      "load should recover when the parameter suite is available");
}

bool testInstanceLifecycleActions(OfxPlugin* plugin) {
  Effect instance;

  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxActionCreateInstance, effectHandle(instance),
                                 nullptr, nullptr) == kOfxStatOK,
               "create instance should succeed in real hosts");
  ok &= expect(plugin->mainEntry(kOfxActionBeginInstanceChanged,
                                 effectHandle(instance), nullptr, nullptr) ==
                   kOfxStatOK,
               "begin instance changed should succeed");
  ok &= expect(plugin->mainEntry(kOfxActionEndInstanceChanged,
                                 effectHandle(instance), nullptr, nullptr) ==
                   kOfxStatOK,
               "end instance changed should succeed");
  ok &= expect(plugin->mainEntry(kOfxActionDestroyInstance, effectHandle(instance),
                                 nullptr, nullptr) == kOfxStatOK,
               "destroy instance should succeed");
  return ok;
}

bool testFloatRgbaPassthrough(OfxPlugin* plugin) {
  Effect instance;
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};

  constexpr int width = 2;
  constexpr int height = 2;
  std::vector<float> source = {
      0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F, 0.8F,
      0.9F, 1.0F, 0.8F, 0.7F, 0.6F, 0.5F, 0.4F, 0.3F,
  };
  std::vector<float> output(source.size(), -1.0F);

  PropertySet sourceImage;
  PropertySet outputImage;
  setString(propsHandle(sourceImage), kOfxImageEffectPropComponents, 0,
            kOfxImageComponentRGBA);
  setString(propsHandle(sourceImage), kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthFloat);
  setString(propsHandle(outputImage), kOfxImageEffectPropComponents, 0,
            kOfxImageComponentRGBA);
  setString(propsHandle(outputImage), kOfxImageEffectPropPixelDepth, 0,
            kOfxBitDepthFloat);
  putPointer(sourceImage, kOfxImagePropData, source.data());
  putPointer(outputImage, kOfxImagePropData, output.data());
  putInt(sourceImage, kOfxImagePropRowBytes, {width * 4 * static_cast<int>(sizeof(float))});
  putInt(outputImage, kOfxImagePropRowBytes, {width * 4 * static_cast<int>(sizeof(float))});
  putInt(sourceImage, kOfxImagePropBounds, {0, 0, width, height});
  putInt(outputImage, kOfxImagePropBounds, {0, 0, width, height});

  instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {12.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "render should succeed for float RGBA");
  ok &= expect(output == source, "render should copy Source RGBA to Output RGBA");
  return ok;
}

bool testRenderFailureWritesStatusFrame(OfxPlugin* plugin) {
  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  setEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE", "/Users/alice/SecretShow/shot010/plate.exr");
  setEnv("CORRIDORKEY_STATUS_PROBE_PY", CORRIDORKEY_PYTHON_EXECUTABLE);
  setEnv("CORRIDORKEY_DIAGNOSTIC_LOG",
         "/tmp/corridorkey-ofx-test-render-failure.jsonl");

  Effect instance;
  defineRuntimeRenderParams(instance);
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};

  constexpr int width = 96;
  constexpr int height = 48;
  std::vector<float> source(width * height * 4, 0.25F);
  std::vector<float> output(source.size(), -1.0F);

  PropertySet sourceImage;
  PropertySet outputImage;
  setString(propsHandle(sourceImage), kOfxImageEffectPropComponents, 0,
            kOfxImageComponentRGBA);
  setString(propsHandle(sourceImage), kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthFloat);
  setString(propsHandle(outputImage), kOfxImageEffectPropComponents, 0,
            kOfxImageComponentRGBA);
  setString(propsHandle(outputImage), kOfxImageEffectPropPixelDepth, 0,
            kOfxBitDepthFloat);
  putPointer(sourceImage, kOfxImagePropData, source.data());
  putPointer(outputImage, kOfxImagePropData, output.data());
  putInt(sourceImage, kOfxImagePropRowBytes, {width * 4 * static_cast<int>(sizeof(float))});
  putInt(outputImage, kOfxImagePropRowBytes, {width * 4 * static_cast<int>(sizeof(float))});
  putInt(sourceImage, kOfxImagePropBounds, {0, 0, width, height});
  putInt(outputImage, kOfxImagePropBounds, {0, 0, width, height});

  instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {12.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

  bool ok = true;
  const int stubInferCallsBefore = g_stubInferCalls;
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "sidecar failure render should return a host-safe status frame");
  ok &= expect(output != source, "sidecar failure should not silently passthrough");
  ok &= expect(g_stubInferCalls > stubInferCallsBefore,
               "render failure should cover the full parameter infer path");

  int brightPixels = 0;
  for (std::size_t index = 0; index < output.size(); index += 4) {
    if (output[index] > 0.75F && output[index + 1] > 0.75F &&
        output[index + 2] > 0.75F) {
      ++brightPixels;
    }
  }
  ok &= expect(brightPixels > 80,
               "rendered sidecar failure frame should contain readable text");

  unsetEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE");
  unsetEnv("CORRIDORKEY_STATUS_PROBE_PY");
  unsetEnv("CORRIDORKEY_DIAGNOSTIC_LOG");
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");
  return ok;
}

bool testRuntimeCallbacksUseHostSafeTimeouts(OfxPlugin* plugin) {
  constexpr int kMinimumHostSafeTimeoutMs = 5000;

  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  setEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE", "/tmp/CorridorKey.ofx.bundle");
  setEnv("CORRIDORKEY_STATUS_PROBE_PY", CORRIDORKEY_PYTHON_EXECUTABLE);
  setEnv("CORRIDORKEY_DIAGNOSTIC_LOG",
         "/tmp/corridorkey-ofx-test-runtime-callbacks.jsonl");

  bool ok = true;

  {
    Effect instance;
    defineRuntimeRenderParams(instance);
    instance.clips[kOfxImageEffectOutputClipName] = Clip{};
    instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};

    constexpr int width = 4;
    constexpr int height = 2;
    std::vector<float> source(width * height * 4, 0.25F);
    std::vector<float> output(source.size(), -1.0F);
    PropertySet sourceImage = makeFloatRgbaImage(source.data(), width, height);
    PropertySet outputImage = makeFloatRgbaImage(output.data(), width, height);
    instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
    instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;

    PropertySet renderArgs;
    putDouble(renderArgs, kOfxPropTime, {12.0});
    putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

    g_lastStubInferTimeoutMs = 0;
    ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                   propsHandle(renderArgs), nullptr) == kOfxStatOK,
                 "infer timeout probe render should remain host-safe");
    ok &= expect(g_lastStubInferTimeoutMs >= kMinimumHostSafeTimeoutMs,
                 "render infer should allow slow but responsive sidecar stdout");
  }

  {
    Effect instance;
    defineRuntimeRenderParams(instance);
    instance.params["ck_output_mode"].intValue = 5;
    instance.clips[kOfxImageEffectOutputClipName] = Clip{};
    instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};

    constexpr int width = 4;
    constexpr int height = 2;
    std::vector<float> output(width * height * 4, -1.0F);
    PropertySet outputImage = makeFloatRgbaImage(output.data(), width, height);
    instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;

    PropertySet renderArgs;
    putDouble(renderArgs, kOfxPropTime, {12.0});
    putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

    g_lastRuntimeProbeTimeoutMs = 0;
    ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                   propsHandle(renderArgs), nullptr) == kOfxStatOK,
                 "status output timeout probe render should remain host-safe");
    ok &= expect(g_lastRuntimeProbeTimeoutMs >= kMinimumHostSafeTimeoutMs,
                 "status probe should allow slow but responsive sidecar stdout");
  }

  {
    Effect instance;
    defineRuntimeRenderParams(instance);
    PropertySet inArgs = changedParamArgs("ck_prepare_warmup");

    g_lastWarmupTimeoutMs = 0;
    ok &= expect(plugin->mainEntry(kOfxActionInstanceChanged, effectHandle(instance),
                                   propsHandle(inArgs), nullptr) == kOfxStatOK,
                 "warmup timeout probe should remain host-safe");
    ok &= expect(g_lastWarmupTimeoutMs >= kMinimumHostSafeTimeoutMs,
                 "warmup should allow slow but responsive sidecar stdout");
  }

  unsetEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE");
  unsetEnv("CORRIDORKEY_STATUS_PROBE_PY");
  unsetEnv("CORRIDORKEY_DIAGNOSTIC_LOG");
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");
  return ok;
}

bool testInferTimeoutScalesForLargeRenderWindows(OfxPlugin* plugin) {
  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  setEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE", "/tmp/CorridorKey.ofx.bundle");
  setEnv("CORRIDORKEY_STATUS_PROBE_PY", CORRIDORKEY_PYTHON_EXECUTABLE);
  setEnv("CORRIDORKEY_DIAGNOSTIC_LOG",
         "/tmp/corridorkey-ofx-test-large-window.jsonl");

  Effect instance;
  defineRuntimeRenderParams(instance);
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};

  constexpr int width = 1024;
  constexpr int height = 1024;
  std::vector<float> source(width * height * 4, 0.25F);
  std::vector<float> output(source.size(), -1.0F);
  PropertySet sourceImage = makeFloatRgbaImage(source.data(), width, height);
  PropertySet outputImage = makeFloatRgbaImage(output.data(), width, height);
  instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {12.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, width, height});

  g_lastStubInferTimeoutMs = 0;
  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "large render-window infer should remain host-safe");
  ok &= expect(g_lastStubInferTimeoutMs >= 9000,
               "large render-window infer should get more than the minimum timeout");

  unsetEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE");
  unsetEnv("CORRIDORKEY_STATUS_PROBE_PY");
  unsetEnv("CORRIDORKEY_DIAGNOSTIC_LOG");
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");
  return ok;
}

bool testMissingSourceImageRendersTransparentBlack(OfxPlugin* plugin) {
  Effect instance;
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};

  constexpr int width = 2;
  constexpr int height = 1;
  std::vector<float> output(width * height * 4, -1.0F);

  PropertySet outputImage;
  setString(propsHandle(outputImage), kOfxImageEffectPropComponents, 0,
            kOfxImageComponentRGBA);
  setString(propsHandle(outputImage), kOfxImageEffectPropPixelDepth, 0,
            kOfxBitDepthFloat);
  putPointer(outputImage, kOfxImagePropData, output.data());
  putInt(outputImage, kOfxImagePropRowBytes, {width * 4 * static_cast<int>(sizeof(float))});
  putInt(outputImage, kOfxImagePropBounds, {0, 0, width, height});

  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;
  instance.clips[kOfxImageEffectSimpleSourceClipName].image = nullptr;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {12.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {1, 0, width, height});

  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "missing source image should render successfully");
  ok &= expect(output == std::vector<float>({-1.0F, -1.0F, -1.0F, -1.0F,
                                             0.0F, 0.0F, 0.0F, 0.0F}),
               "missing source image should fill only the render window");
  return ok;
}

bool testFieldedRenderReturnsUnsupported(OfxPlugin* plugin) {
  Effect instance;
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {12.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {0, 0, 1, 1});
  setString(propsHandle(renderArgs), kOfxImageEffectPropFieldToRender, 0,
            kOfxImageFieldLower);

  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) ==
                   kOfxStatErrUnsupported,
               "fielded render should return explicit unsupported status");
  return ok;
}

bool testEmptyRenderWindowIsNoOp(OfxPlugin* plugin) {
  Effect instance;
  defineRuntimeRenderParams(instance);

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {12.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {2, 2, 2, 2});

  const int stubInferCallsBefore = g_stubInferCalls;
  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                 propsHandle(renderArgs), nullptr) == kOfxStatOK,
               "empty render window should be a no-op");
  ok &= expect(g_stubInferCalls == stubInferCallsBefore,
               "empty render window should not call sidecar inference");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= expect(OfxGetNumberOfPlugins() == 1, "exactly one plugin should be exported");

  OfxPlugin* plugin = OfxGetPlugin(0);
  ok &= expect(plugin != nullptr, "first plugin should exist");
  ok &= expect(OfxGetPlugin(1) == nullptr, "only one plugin should exist");
  ok &= expect(std::string(plugin->pluginApi) == kOfxImageEffectPluginApi,
               "plugin API should be ImageEffect");
  ok &= expect(std::string(plugin->pluginIdentifier) == CorridorKey::OFX::kPluginIdentifier,
               "plugin identifier should be stable");
  ok &= expect(plugin->pluginVersionMajor == CorridorKey::OFX::kPluginVersionMajor,
               "plugin major version should be stable");
  ok &= expect(plugin->pluginVersionMinor == CorridorKey::OFX::kPluginVersionMinor,
               "plugin minor version should be stable");

  OfxHost host{};
  host.fetchSuite = fetchSuite;
  plugin->setHost(&host);
  ok &= expect(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr) == kOfxStatOK,
               "load should fetch only core OFX suites");

  ok &= testMissingParameterSuiteFailsLoad(plugin, host);
  ok &= testDescriptorAndScannerSafety(plugin);
  ok &= testInstanceLifecycleActions(plugin);
  ok &= testFloatRgbaPassthrough(plugin);
  ok &= testRenderFailureWritesStatusFrame(plugin);
  ok &= testRuntimeCallbacksUseHostSafeTimeouts(plugin);
  ok &= testInferTimeoutScalesForLargeRenderWindows(plugin);
  ok &= testMissingSourceImageRendersTransparentBlack(plugin);
  ok &= testFieldedRenderReturnsUnsupported(plugin);
  ok &= testEmptyRenderWindowIsNoOp(plugin);

  return ok ? 0 : 1;
}
