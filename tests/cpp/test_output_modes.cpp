#include "CorridorKeyPlugin.h"

#include "ofxParam.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef CORRIDORKEY_SOURCE_DIR
#define CORRIDORKEY_SOURCE_DIR "."
#endif

#ifndef CORRIDORKEY_PYTHON_EXECUTABLE
#define CORRIDORKEY_PYTHON_EXECUTABLE "python3"
#endif

extern "C" {
int OfxGetNumberOfPlugins(void);
OfxPlugin* OfxGetPlugin(int nth);
}

namespace {

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
  std::string stringValue;
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

OfxStatus getPropertySet(OfxImageEffectHandle effect, OfxPropertySetHandle* props) {
  *props = propsHandle(asEffect(effect)->props);
  return kOfxStatOK;
}

OfxStatus getParamSet(OfxImageEffectHandle effect, OfxParamSetHandle* paramSet) {
  *paramSet = paramSetHandle(*asEffect(effect));
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

OfxStatus paramSetValue(OfxParamHandle paramHandle, ...) {
  Param* param = asParam(paramHandle);
  va_list args;
  va_start(args, paramHandle);
  if (param != nullptr && param->type == kOfxParamTypeString) {
    const char* value = va_arg(args, const char*);
    param->stringValue = value == nullptr ? "" : value;
  }
  va_end(args);
  return param == nullptr ? kOfxStatErrBadHandle : kOfxStatOK;
}

OfxStatus paramGetValueAtTime(OfxParamHandle paramHandle, OfxTime time, ...) {
  Param* param = asParam(paramHandle);
  (void)time;
  if (param == nullptr) {
    return kOfxStatErrBadHandle;
  }
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

OfxPropertySuiteV1 g_propertySuite{};
OfxImageEffectSuiteV1 g_effectSuite{};
OfxParameterSuiteV1 g_parameterSuite{};

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

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "output modes test failed: " << message << '\n';
    return false;
  }
  return true;
}

bool near(float actual, float expected) {
  return std::fabs(actual - expected) < 0.0001F;
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

void putDouble(PropertySet& props, const char* property, std::vector<double> values) {
  props.doubles[property] = std::move(values);
}

void putInt(PropertySet& props, const char* property, std::vector<int> values) {
  props.ints[property] = std::move(values);
}

void putPointer(PropertySet& props, const char* property, void* value) {
  props.pointers[property] = {value};
}

PropertySet changedParamArgs(const char* name) {
  PropertySet args;
  setString(propsHandle(args), kOfxPropName, 0, name);
  return args;
}

PropertySet makeFloatImage(float* data, int width, int height) {
  PropertySet image;
  setString(propsHandle(image), kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);
  setString(propsHandle(image), kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthFloat);
  putPointer(image, kOfxImagePropData, data);
  putInt(image, kOfxImagePropRowBytes, {width * 4 * static_cast<int>(sizeof(float))});
  putInt(image, kOfxImagePropBounds, {0, 0, width, height});
  return image;
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

std::filesystem::path makeDevBundle(const std::string& name) {
  const std::filesystem::path sourceRoot{CORRIDORKEY_SOURCE_DIR};
  const std::filesystem::path tempRoot =
      std::filesystem::temp_directory_path() / ("corridorkey-" + name);
  std::error_code ec;
  std::filesystem::remove_all(tempRoot, ec);
  const std::filesystem::path resources =
      tempRoot / "CorridorKey.ofx.bundle" / "Contents" / "Resources";
  std::filesystem::create_directories(resources);
  std::filesystem::copy(sourceRoot / "sidecar", resources / "sidecar",
                        std::filesystem::copy_options::recursive);
  return tempRoot / "CorridorKey.ofx.bundle";
}

void installSuites() {
  g_propertySuite.propSetString = setString;
  g_propertySuite.propSetInt = setInt;
  g_propertySuite.propSetDouble = setDouble;
  g_propertySuite.propGetPointer = getPointer;
  g_propertySuite.propGetString = getString;
  g_propertySuite.propGetDouble = getDouble;
  g_propertySuite.propGetInt = getInt;
  g_propertySuite.propGetIntN = getIntN;

  g_effectSuite.getPropertySet = getPropertySet;
  g_effectSuite.getParamSet = getParamSet;
  g_effectSuite.clipGetHandle = clipGetHandle;
  g_effectSuite.clipGetImage = clipGetImage;
  g_effectSuite.clipReleaseImage = clipReleaseImage;
  g_effectSuite.abort = abortRender;

  g_parameterSuite.paramGetHandle = paramGetHandle;
  g_parameterSuite.paramSetValue = paramSetValue;
  g_parameterSuite.paramGetValueAtTime = paramGetValueAtTime;
}

Effect makeInstance(int outputMode,
                    int screenColor = 1,
                    int quality = 1,
                    double despillStrength = 5.0,
                    int backend = 0,
                    int inputColorSpace = 0) {
  Effect instance;
  instance.clips[kOfxImageEffectOutputClipName] = Clip{};
  instance.clips[kOfxImageEffectSimpleSourceClipName] = Clip{};
  instance.clips["AlphaHint"] = Clip{};

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
           "ck_support_bundle_status",
           "ck_last_error",
           "ck_guide_source_status",
           "ck_effective_quality",
       }) {
    defineStringParam(instance, name);
  }

  defineChoiceParam(instance, "ck_alpha_hint_source", 0);
  defineChoiceParam(instance, "ck_output_mode", outputMode);
  defineChoiceParam(instance, "ck_screen_color", screenColor);
  defineChoiceParam(instance, "ck_quality", quality);
  defineChoiceParam(instance, "ck_input_color_space", inputColorSpace);
  defineChoiceParam(instance, "ck_backend", backend);
  defineDoubleParam(instance, "ck_despill_strength", despillStrength);
  return instance;
}

std::vector<float> sourcePixels(int width, int height) {
  std::vector<float> pixels(static_cast<std::size_t>(width * height * 4), 0.0F);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t index = static_cast<std::size_t>((y * width + x) * 4);
      pixels[index + 0] = 0.18F + 0.11F * static_cast<float>(x);
      pixels[index + 1] = 0.62F - 0.07F * static_cast<float>(y);
      pixels[index + 2] = 0.16F + 0.03F * static_cast<float>(x + y);
      pixels[index + 3] = 1.0F;
    }
  }
  return pixels;
}

std::vector<float> alphaPixels(int width, int height, bool outOfRange = false) {
  std::vector<float> pixels(static_cast<std::size_t>(width * height * 4), 0.0F);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float alpha = (x == 1 && y == 1) ? 0.25F : ((x == 2 && y == 1) ? 0.75F : 0.5F);
      if (outOfRange && x == 1 && y == 1) {
        alpha = -0.5F;
      } else if (outOfRange && x == 2 && y == 1) {
        alpha = 1.5F;
      }
      const std::size_t index = static_cast<std::size_t>((y * width + x) * 4);
      pixels[index + 0] = 0.0F;
      pixels[index + 1] = 0.0F;
      pixels[index + 2] = 0.0F;
      pixels[index + 3] = alpha;
    }
  }
  return pixels;
}

float pixel(const std::vector<float>& pixels, int width, int x, int y, int c) {
  return pixels[static_cast<std::size_t>((y * width + x) * 4 + c)];
}

std::vector<float> renderMode(OfxPlugin* plugin,
                              int outputMode,
                              OfxStatus* renderStatus,
                              std::string* runtimeState = nullptr,
                              bool outOfRangeAlpha = false,
                              int screenColor = 1,
                              int quality = 1,
                              double despillStrength = 5.0,
                              int backend = 0,
                              std::string* modelStatus = nullptr,
                              std::string* modelSourceStatus = nullptr,
                              std::string* installStatus = nullptr,
                              std::string* backendStatus = nullptr,
                              bool attachSource = true,
                              bool attachAlpha = true,
                              int inputColorSpace = 0) {
  constexpr int width = 4;
  constexpr int height = 3;
  std::vector<float> source = sourcePixels(width, height);
  std::vector<float> alpha = alphaPixels(width, height, outOfRangeAlpha);
  std::vector<float> output(static_cast<std::size_t>(width * height * 4), -7.0F);

  PropertySet sourceImage = makeFloatImage(source.data(), width, height);
  PropertySet alphaImage = makeFloatImage(alpha.data(), width, height);
  PropertySet outputImage = makeFloatImage(output.data(), width, height);

  Effect instance = makeInstance(outputMode, screenColor, quality, despillStrength,
                                 backend, inputColorSpace);
  if (attachSource) {
    instance.clips[kOfxImageEffectSimpleSourceClipName].image = &sourceImage;
  }
  if (attachAlpha) {
    instance.clips["AlphaHint"].image = &alphaImage;
  }
  instance.clips[kOfxImageEffectOutputClipName].image = &outputImage;

  PropertySet renderArgs;
  putDouble(renderArgs, kOfxPropTime, {10.0});
  putInt(renderArgs, kOfxImageEffectPropRenderWindow, {1, 1, 3, 2});

  *renderStatus = plugin->mainEntry(kOfxImageEffectActionRender, effectHandle(instance),
                                    propsHandle(renderArgs), nullptr);
  if (runtimeState != nullptr) {
    *runtimeState = instance.params["ck_runtime_state"].stringValue;
  }
  if (modelStatus != nullptr) {
    *modelStatus = instance.params["ck_model_status"].stringValue;
  }
  if (modelSourceStatus != nullptr) {
    *modelSourceStatus = instance.params["ck_model_source_status"].stringValue;
  }
  if (installStatus != nullptr) {
    *installStatus = instance.params["ck_install_status"].stringValue;
  }
  if (backendStatus != nullptr) {
    *backendStatus = instance.params["ck_backend_status"].stringValue;
  }
  return output;
}

bool renderWindowPixelChanged(const std::vector<float>& first,
                              const std::vector<float>& second) {
  constexpr int width = 4;
  for (int c = 0; c < 4; ++c) {
    if (!near(pixel(first, width, 1, 1, c), pixel(second, width, 1, 1, c))) {
      return true;
    }
  }
  return false;
}

bool expectOutsideWindowUntouched(const std::vector<float>& output) {
  constexpr int width = 4;
  constexpr int height = 3;
  bool ok = true;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      if (x >= 1 && x < 3 && y == 1) {
        continue;
      }
      for (int c = 0; c < 4; ++c) {
        ok &= expect(near(pixel(output, width, x, y, c), -7.0F),
                     "render should not write outside render window");
      }
    }
  }
  return ok;
}

bool testInferParametersReachSidecar(OfxPlugin* plugin) {
  bool ok = true;
  OfxStatus status = kOfxStatFailed;
  std::string modelStatus;
  std::string modelSourceStatus;
  std::string installStatus;

  const std::vector<float> baseline =
      renderMode(plugin, 0, &status, nullptr, false, 1, 1, 5.0, 0,
                 &modelStatus, &modelSourceStatus, &installStatus);
  ok &= expect(status == kOfxStatOK, "baseline request render should succeed");
  ok &= expect(modelStatus == "missing",
               "successful stub infer should preserve model status fields");
  ok &= expect(modelSourceStatus == "ready",
               "successful stub infer should preserve model source status fields");
  ok &= expect(installStatus == "not_installed",
               "successful stub infer should preserve install status fields");

  const std::vector<float> blueScreen =
      renderMode(plugin, 0, &status, nullptr, false, 2, 1, 5.0);
  ok &= expect(status == kOfxStatOK, "screen color request render should succeed");
  ok &= expect(renderWindowPixelChanged(baseline, blueScreen),
               "Screen Color should affect the sidecar infer request");

  const std::vector<float> draftQuality =
      renderMode(plugin, 0, &status, nullptr, false, 1, 0, 5.0);
  ok &= expect(status == kOfxStatOK, "quality request render should succeed");
  ok &= expect(renderWindowPixelChanged(baseline, draftQuality),
               "Quality should affect the sidecar infer request");

  const std::vector<float> highDespill =
      renderMode(plugin, 0, &status, nullptr, false, 1, 1, 6.0);
  ok &= expect(status == kOfxStatOK, "despill request render should succeed");
  ok &= expect(renderWindowPixelChanged(baseline, highDespill),
               "Despill Strength should affect the sidecar infer request");

  return ok;
}

bool testSrgbInputColorSpaceLinearizesSourceBlob(OfxPlugin* plugin) {
  constexpr int width = 4;
  bool ok = true;
  OfxStatus status = kOfxStatFailed;

  const std::vector<float> linear =
      renderMode(plugin, 0, &status, nullptr, false, 1, 1, 0.0, 0, nullptr,
                 nullptr, nullptr, nullptr, true, true, 2);
  ok &= expect(status == kOfxStatOK, "Linear input color render should succeed");

  const std::vector<float> srgb =
      renderMode(plugin, 0, &status, nullptr, false, 1, 1, 0.0, 0, nullptr,
                 nullptr, nullptr, nullptr, true, true, 1);
  ok &= expect(status == kOfxStatOK, "sRGB-Rec709 input color render should succeed");

  ok &= expect(pixel(srgb, width, 1, 1, 0) + 0.001F <
                   pixel(linear, width, 1, 1, 0),
               "sRGB-Rec709 input should be decoded before fake infer sees the source blob");
  ok &= expect(near(pixel(srgb, width, 1, 1, 3),
                   pixel(linear, width, 1, 1, 3)),
               "input color conversion should preserve alpha");
  return ok;
}

bool testNonStubBackendReportsRuntimeUnavailable(OfxPlugin* plugin) {
  OfxStatus status = kOfxStatFailed;
  std::string runtimeState;
  std::string modelStatus;
  std::string modelSourceStatus;
  std::string installStatus;
  std::string backendStatus;
  const std::vector<float> output = renderMode(
      plugin, 0, &status, &runtimeState, false, 1, 1, 5.0, 2, &modelStatus,
      &modelSourceStatus, &installStatus, &backendStatus);

  bool ok = true;
  ok &= expect(status == kOfxStatOK,
               "non-stub backend should render a visible backend-unavailable status frame");
  ok &= expect(runtimeState == "error",
               "non-stub backend should update runtime state to error");
  ok &= expect(modelStatus == "missing",
               "non-stub backend should update model status");
  ok &= expect(modelSourceStatus == "ready",
               "non-stub backend should update model source status");
  ok &= expect(installStatus == "not_installed",
               "non-stub backend should update install status");
  ok &= expect(backendStatus == "blocked",
               "non-stub backend should update backend status");
  ok &= expectOutsideWindowUntouched(output);
  ok &= expect(!near(pixel(output, 4, 1, 1, 0), -7.0F),
               "backend-unavailable status should write the render window");
  return ok;
}

bool testOutputModes(OfxPlugin* plugin) {
  constexpr int width = 4;
  bool ok = true;
  OfxStatus status = kOfxStatFailed;

  const std::vector<float> processed = renderMode(plugin, 0, &status);
  ok &= expect(status == kOfxStatOK, "Processed RGBA render should succeed");
  ok &= expectOutsideWindowUntouched(processed);

  const std::vector<float> matte = renderMode(plugin, 1, &status);
  ok &= expect(status == kOfxStatOK, "Matte render should succeed");
  ok &= expectOutsideWindowUntouched(matte);

  const std::vector<float> straight = renderMode(plugin, 2, &status);
  ok &= expect(status == kOfxStatOK, "Straight FG render should succeed");
  ok &= expectOutsideWindowUntouched(straight);

  for (int x : {1, 2}) {
    const float alpha = pixel(matte, width, x, 1, 0);
    ok &= expect(near(pixel(matte, width, x, 1, 1), alpha) &&
                     near(pixel(matte, width, x, 1, 2), alpha) &&
                     near(pixel(matte, width, x, 1, 3), alpha),
                 "Matte mode should map alpha to every output component");
    ok &= expect(near(pixel(straight, width, x, 1, 3), alpha),
                 "Straight FG should preserve the fake alpha channel");
    for (int c = 0; c < 3; ++c) {
      ok &= expect(near(pixel(processed, width, x, 1, c),
                        pixel(straight, width, x, 1, c) * alpha),
                   "Processed RGBA should be premultiplied by fake alpha");
    }
    ok &= expect(near(pixel(processed, width, x, 1, 3), alpha),
                 "Processed RGBA should carry fake alpha");
  }

  const std::vector<float> alphaView = renderMode(plugin, 3, &status);
  ok &= expect(status == kOfxStatOK, "Alpha Hint View render should succeed");
  ok &= expectOutsideWindowUntouched(alphaView);
  ok &= expect(near(pixel(alphaView, width, 1, 1, 0), 0.25F) &&
                   near(pixel(alphaView, width, 1, 1, 3), 0.25F),
               "Alpha Hint View should show the resolved guide alpha");
  ok &= expect(near(pixel(alphaView, width, 2, 1, 0), 0.75F) &&
                   near(pixel(alphaView, width, 2, 1, 3), 0.75F),
               "Alpha Hint View should vary per input guide pixel");

  const std::vector<float> checker = renderMode(plugin, 4, &status);
  ok &= expect(status == kOfxStatOK, "Checker Comp render should succeed");
  ok &= expectOutsideWindowUntouched(checker);
  ok &= expect(near(pixel(checker, width, 1, 1, 3), 1.0F) &&
                   near(pixel(checker, width, 2, 1, 3), 1.0F),
               "Checker Comp should output an opaque diagnostic composite");
  ok &= expect(!near(pixel(checker, width, 1, 1, 0), pixel(processed, width, 1, 1, 0)),
               "Checker Comp should add visible checker background where alpha is partial");

  std::string statusBackendStatus;
  const std::vector<float> statusOutput = renderMode(
      plugin, 5, &status, nullptr, false, 1, 1, 5.0, 0, nullptr, nullptr,
      nullptr, &statusBackendStatus);
  ok &= expect(status == kOfxStatOK, "Status output mode should render successfully");
  ok &= expect(statusBackendStatus == "stub",
               "Status output mode should surface backend status");
  ok &= expectOutsideWindowUntouched(statusOutput);
  ok &= expect(!near(pixel(statusOutput, width, 1, 1, 0), -7.0F),
               "Status output mode should write a visible status frame");
  OfxStatus statusWithoutSource = kOfxStatFailed;
  const std::vector<float> statusOutputWithoutSource = renderMode(
      plugin, 5, &statusWithoutSource, nullptr, false, 1, 1, 5.0, 0, nullptr,
      nullptr, nullptr, nullptr, false, false);
  ok &= expect(statusWithoutSource == kOfxStatOK,
               "Status output mode should not require Source or AlphaHint images");
  ok &= expect(!near(pixel(statusOutputWithoutSource, width, 1, 1, 0), -7.0F),
               "Status output mode without source should write the render window");

  return ok;
}

bool testDiagnosticAlphaClamp(OfxPlugin* plugin) {
  constexpr int width = 4;
  bool ok = true;
  OfxStatus status = kOfxStatFailed;

  const std::vector<float> alphaView =
      renderMode(plugin, 3, &status, nullptr, true);
  ok &= expect(status == kOfxStatOK,
               "Alpha Hint View with out-of-range alpha should succeed");
  ok &= expect(near(pixel(alphaView, width, 1, 1, 0), 0.0F) &&
                   near(pixel(alphaView, width, 1, 1, 3), 0.0F),
               "Alpha Hint View should clamp negative alpha to zero");
  ok &= expect(near(pixel(alphaView, width, 2, 1, 0), 1.0F) &&
                   near(pixel(alphaView, width, 2, 1, 3), 1.0F),
               "Alpha Hint View should clamp alpha above one");

  const std::vector<float> checker =
      renderMode(plugin, 4, &status, nullptr, true);
  ok &= expect(status == kOfxStatOK,
               "Checker Comp with out-of-range alpha should succeed");
  ok &= expect(near(pixel(checker, width, 1, 1, 3), 1.0F) &&
                   near(pixel(checker, width, 2, 1, 3), 1.0F),
               "Checker Comp should remain opaque with clamped alpha");
  return ok;
}

bool testStatusFallback(OfxPlugin* plugin) {
  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  setEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE", "/Users/example/SecretShow/plate.exr");
  setEnv("CORRIDORKEY_STATUS_PROBE_PY", CORRIDORKEY_PYTHON_EXECUTABLE);

  OfxStatus status = kOfxStatFailed;
  std::string runtimeState;
  const std::vector<float> output = renderMode(plugin, 0, &status, &runtimeState);

  unsetEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE");
  unsetEnv("CORRIDORKEY_STATUS_PROBE_PY");
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");

  bool ok = true;
  ok &= expect(status == kOfxStatOK, "sidecar failure should render a status fallback frame");
  ok &= expect(runtimeState == "error", "sidecar failure should update runtime state");
  ok &= expectOutsideWindowUntouched(output);
  ok &= expect(!near(pixel(output, 4, 1, 1, 0), -7.0F),
               "status fallback should write the render window");
  return ok;
}

bool hasRequiredSupportBundleFiles(const std::filesystem::path& bundleDir) {
  return std::filesystem::is_regular_file(bundleDir / "manifest.json") &&
         std::filesystem::is_regular_file(bundleDir / "diagnostics.json") &&
         std::filesystem::is_regular_file(bundleDir / "doctor.txt") &&
         std::filesystem::is_regular_file(bundleDir / "logs" / "redacted.log") &&
         std::filesystem::is_regular_file(bundleDir / "manifest_status.json") &&
         std::filesystem::is_regular_file(bundleDir / "backend_status.json") &&
         std::filesystem::is_regular_file(bundleDir / "recent_errors.json") &&
         std::filesystem::is_regular_file(bundleDir / "redaction_proof.json");
}

bool testCopySupportBundleHostAction(OfxPlugin* plugin,
                                     const std::filesystem::path& bundleRoot) {
  const std::filesystem::path outputRoot =
      std::filesystem::temp_directory_path() /
      "corridorkey-output-modes-support-bundles";
  std::error_code ec;
  std::filesystem::remove_all(outputRoot, ec);

  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  setEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE", bundleRoot.string());
  setEnv("CORRIDORKEY_STATUS_PROBE_PY", CORRIDORKEY_PYTHON_EXECUTABLE);
  setEnv("CORRIDORKEY_SUPPORT_BUNDLE_OUTPUT", outputRoot.string());

  Effect instance = makeInstance(0);
  PropertySet inArgs = changedParamArgs("ck_copy_support_bundle");
  const OfxStatus status = plugin->mainEntry(kOfxActionInstanceChanged,
                                             effectHandle(instance),
                                             propsHandle(inArgs), nullptr);

  unsetEnv("CORRIDORKEY_SUPPORT_BUNDLE_OUTPUT");
  unsetEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE");
  unsetEnv("CORRIDORKEY_STATUS_PROBE_PY");
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");

  std::filesystem::path generatedBundle;
  if (std::filesystem::is_directory(outputRoot)) {
    for (const auto& entry : std::filesystem::directory_iterator(outputRoot)) {
      if (entry.is_directory() &&
          entry.path().filename().string().find("corridorkey-support-") == 0) {
        generatedBundle = entry.path();
        break;
      }
    }
  }

  bool ok = true;
  ok &= expect(status == kOfxStatOK,
               "Copy Support Bundle host action should remain host-safe");
  ok &= expect(instance.params["ck_support_bundle_status"].stringValue == "completed",
               "Copy Support Bundle should complete when runtime resources exist");
  ok &= expect(instance.params["ck_last_error"].stringValue.empty(),
               "Copy Support Bundle should clear last error on success");
  ok &= expect(!generatedBundle.empty(),
               "Copy Support Bundle should create a support bundle directory");
  ok &= expect(!generatedBundle.empty() &&
                   hasRequiredSupportBundleFiles(generatedBundle),
               "Copy Support Bundle should include required redacted bundle files");
  ok &= expect(std::filesystem::is_regular_file(outputRoot / "support-bundle-last.txt"),
               "Copy Support Bundle should capture local command output");
  ok &= expect(instance.params["ck_support_bundle_status"].stringValue.find(
                   outputRoot.string()) == std::string::npos,
               "Copy Support Bundle status should not expose local output paths");

  std::filesystem::remove_all(outputRoot, ec);
  return ok;
}

bool testTrailingFrameBlobFallback(OfxPlugin* plugin,
                                   const std::filesystem::path& bundleRoot) {
  setEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE", bundleRoot.string());
  setEnv("CORRIDORKEY_STATUS_PROBE_PY", CORRIDORKEY_PYTHON_EXECUTABLE);
  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  setEnv("CORRIDORKEY_TEST_FORCE_STUB_INFER", "1");
  setEnv("CORRIDORKEY_TEST_APPEND_OUTPUT_TRAILING_DATA", "1");

  OfxStatus status = kOfxStatFailed;
  std::string runtimeState;
  const std::vector<float> output = renderMode(plugin, 0, &status, &runtimeState);

  unsetEnv("CORRIDORKEY_TEST_APPEND_OUTPUT_TRAILING_DATA");
  unsetEnv("CORRIDORKEY_TEST_FORCE_STUB_INFER");
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");
  unsetEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE");
  unsetEnv("CORRIDORKEY_STATUS_PROBE_PY");

  bool ok = true;
  ok &= expect(status == kOfxStatOK,
               "corrupt sidecar output should render a status fallback frame");
  ok &= expect(runtimeState == "error",
               "corrupt sidecar output should update runtime state");
  ok &= expectOutsideWindowUntouched(output);
  ok &= expect(!near(pixel(output, 4, 1, 1, 0), -7.0F),
               "corrupt sidecar output should not be accepted as valid pixels");
  return ok;
}

bool testStraightFgDoesNotRequireProcessedBlob(OfxPlugin* plugin,
                                               const std::filesystem::path& bundleRoot) {
  setEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE", bundleRoot.string());
  setEnv("CORRIDORKEY_STATUS_PROBE_PY", CORRIDORKEY_PYTHON_EXECUTABLE);
  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  setEnv("CORRIDORKEY_TEST_FORCE_STUB_INFER", "1");
  setEnv("CORRIDORKEY_TEST_APPEND_OUTPUT_TRAILING_DATA", "1");

  OfxStatus status = kOfxStatFailed;
  std::string runtimeState;
  const std::vector<float> output = renderMode(plugin, 2, &status, &runtimeState);

  unsetEnv("CORRIDORKEY_TEST_APPEND_OUTPUT_TRAILING_DATA");
  unsetEnv("CORRIDORKEY_TEST_FORCE_STUB_INFER");
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");
  unsetEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE");
  unsetEnv("CORRIDORKEY_STATUS_PROBE_PY");

  bool ok = true;
  ok &= expect(status == kOfxStatOK,
               "Straight FG should not fail because the unused processed blob is corrupt");
  ok &= expect(runtimeState != "error",
               "Straight FG should not surface an unused processed blob error");
  ok &= expectOutsideWindowUntouched(output);
  ok &= expect(!near(pixel(output, 4, 1, 1, 0), -7.0F),
               "Straight FG should write the render window");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  installSuites();

  ok &= expect(OfxGetNumberOfPlugins() == 1, "exactly one plugin should be exported");
  OfxPlugin* plugin = OfxGetPlugin(0);
  ok &= expect(plugin != nullptr, "plugin should be available");

  OfxHost host{};
  host.fetchSuite = fetchSuite;
  plugin->setHost(&host);
  ok &= expect(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr) ==
                   kOfxStatOK,
               "plugin load should fetch fake suites");

  const std::filesystem::path bundleRoot = makeDevBundle("output-modes-test");
  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  setEnv("CORRIDORKEY_TEST_FORCE_STUB_INFER", "1");
  setEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE", bundleRoot.string());
  setEnv("CORRIDORKEY_STATUS_PROBE_PY", CORRIDORKEY_PYTHON_EXECUTABLE);
  ok &= testOutputModes(plugin);
  ok &= testInferParametersReachSidecar(plugin);
  ok &= testSrgbInputColorSpaceLinearizesSourceBlob(plugin);
  ok &= testNonStubBackendReportsRuntimeUnavailable(plugin);
  ok &= testDiagnosticAlphaClamp(plugin);
  unsetEnv("CORRIDORKEY_STATUS_PROBE_BUNDLE");
  unsetEnv("CORRIDORKEY_STATUS_PROBE_PY");
  unsetEnv("CORRIDORKEY_TEST_FORCE_STUB_INFER");
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");

  ok &= testStatusFallback(plugin);
  ok &= testCopySupportBundleHostAction(plugin, bundleRoot);
  ok &= testTrailingFrameBlobFallback(plugin, bundleRoot);
  ok &= testStraightFgDoesNotRequireProcessedBlob(plugin, bundleRoot);

  std::error_code ec;
  std::filesystem::remove_all(bundleRoot.parent_path(), ec);
  return ok ? 0 : 1;
}
