#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace {

constexpr const char* kExpectedPluginIdentifier = "com.corridorkey.openfx";
constexpr const char* kExpectedPluginLabel = "CorridorKey OFX";
constexpr const char* kExpectedPluginGrouping = "Keyer";

struct PropertySet {
  std::map<std::string, std::vector<std::string>> strings;
  std::map<std::string, std::vector<int>> ints;
  std::map<std::string, std::vector<double>> doubles;
  std::map<std::string, std::vector<void*>> pointers;
};

struct Clip {
  PropertySet props;
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
  values[static_cast<std::size_t>(index)] = value == nullptr ? "" : value;
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

OfxStatus paramDefine(OfxParamSetHandle paramSet, const char* paramType,
                      const char* name, OfxPropertySetHandle* propertySet) {
  Param& param = asParamSet(paramSet)->params[name];
  param.type = paramType == nullptr ? "" : paramType;
  if (propertySet != nullptr) {
    *propertySet = propsHandle(param.props);
  }
  return kOfxStatOK;
}

OfxStatus paramGetHandle(OfxParamSetHandle paramSet, const char* name,
                         OfxParamHandle* param, OfxPropertySetHandle* propertySet) {
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

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ofx binary scan metadata test failed: " << message << '\n';
    return false;
  }
  return true;
}

bool contains(const std::vector<std::string>& values, const char* value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool describeContext(OfxPlugin* plugin, const char* contextName, Effect& descriptor) {
  PropertySet context;
  setString(propsHandle(context), kOfxImageEffectPropContext, 0, contextName);
  return expect(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                  effectHandle(descriptor), propsHandle(context), nullptr) ==
                    kOfxStatOK,
                std::string("describe metadata should succeed for context: ") + contextName);
}

using GetNumberOfPluginsFn = int (*)();
using GetPluginFn = OfxPlugin* (*)(int);

}  // namespace

int main() {
#if defined(_WIN32)
  std::cout << "ofx binary scan metadata test skipped: Windows dynamic loader path is not implemented.\n";
  return 0;
#else
  void* library = dlopen(CORRIDORKEY_PLUGIN_BINARY, RTLD_NOW | RTLD_LOCAL);
  if (library == nullptr) {
    std::cerr << "ofx binary scan metadata test failed: dlopen final OFX binary failed: "
              << dlerror() << '\n';
    return 1;
  }

  auto closeLibrary = [&library]() {
    if (library != nullptr) {
      dlclose(library);
      library = nullptr;
    }
  };

  dlerror();
  auto getNumberOfPlugins =
      reinterpret_cast<GetNumberOfPluginsFn>(dlsym(library, "OfxGetNumberOfPlugins"));
  const char* symbolError = dlerror();
  if (symbolError != nullptr || getNumberOfPlugins == nullptr) {
    std::cerr << "ofx binary scan metadata test failed: dlsym OfxGetNumberOfPlugins failed: "
              << (symbolError == nullptr ? "missing symbol" : symbolError) << '\n';
    closeLibrary();
    return 1;
  }

  dlerror();
  auto getPlugin = reinterpret_cast<GetPluginFn>(dlsym(library, "OfxGetPlugin"));
  symbolError = dlerror();
  if (symbolError != nullptr || getPlugin == nullptr) {
    std::cerr << "ofx binary scan metadata test failed: dlsym OfxGetPlugin failed: "
              << (symbolError == nullptr ? "missing symbol" : symbolError) << '\n';
    closeLibrary();
    return 1;
  }

  bool ok = true;
  ok &= expect(getNumberOfPlugins() == 1, "final binary should export exactly one plugin");

  OfxPlugin* plugin = getPlugin(0);
  ok &= expect(plugin != nullptr, "final binary should return plugin 0");
  ok &= expect(getPlugin(1) == nullptr, "final binary should not return plugin 1");

  if (plugin != nullptr) {
    ok &= expect(std::string(plugin->pluginApi) == kOfxImageEffectPluginApi,
                 "final binary plugin API should be ImageEffect");
    ok &= expect(std::string(plugin->pluginIdentifier) == kExpectedPluginIdentifier,
                 "final binary plugin identifier should be stable");

    OfxHost host{};
    host.fetchSuite = fetchSuite;
    plugin->setHost(&host);
    ok &= expect(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr) == kOfxStatOK,
                 "final binary load action should fetch metadata suites");

    Effect globalDescriptor;
    ok &= expect(plugin->mainEntry(kOfxActionDescribe, effectHandle(globalDescriptor), nullptr,
                                   nullptr) == kOfxStatOK,
                 "final binary describe metadata action should succeed");

    Effect filterDescriptor;
    Effect generalDescriptor;
    ok &= describeContext(plugin, kOfxImageEffectContextFilter, filterDescriptor);
    ok &= describeContext(plugin, kOfxImageEffectContextGeneral, generalDescriptor);

    const auto& contexts =
        globalDescriptor.props.strings[kOfxImageEffectPropSupportedContexts];
    ok &= expect(contains(contexts, kOfxImageEffectContextFilter),
                 "final binary metadata should support Filter context");
    ok &= expect(contains(contexts, kOfxImageEffectContextGeneral),
                 "final binary metadata should support General context");
    ok &= expect(globalDescriptor.props.strings[kOfxPropLabel][0] == kExpectedPluginLabel,
                 "final binary label metadata should be stable");
    ok &= expect(globalDescriptor.props.strings[kOfxImageEffectPluginPropGrouping][0] ==
                     kExpectedPluginGrouping,
                 "final binary grouping metadata should be stable");
    ok &= expect(filterDescriptor.clips.count(kOfxImageEffectOutputClipName) == 1,
                 "final binary Filter metadata should define Output clip");
    ok &= expect(filterDescriptor.clips.count(kOfxImageEffectSimpleSourceClipName) == 1,
                 "final binary Filter metadata should define Source clip");
    ok &= expect(generalDescriptor.clips.count(kOfxImageEffectOutputClipName) == 1,
                 "final binary General metadata should define Output clip");
    ok &= expect(generalDescriptor.clips.count(kOfxImageEffectSimpleSourceClipName) == 1,
                 "final binary General metadata should define Source clip");
  }

  closeLibrary();
  return ok ? 0 : 1;
#endif
}
