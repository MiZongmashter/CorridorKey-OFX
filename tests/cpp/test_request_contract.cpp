#include "CorridorKeyPlugin.h"
#include "sidecar_client/SidecarProtocol.h"

#include "ofxParam.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

extern "C" {
int OfxGetNumberOfPlugins(void);
OfxPlugin* OfxGetPlugin(int nth);
}

namespace {

struct PropertySet {
  std::map<std::string, std::vector<std::string>> strings;
  std::map<std::string, std::vector<int>> ints;
  std::map<std::string, std::vector<double>> doubles;
};

struct Param {
  PropertySet props;
  std::string type;
};

struct Clip {
  PropertySet props;
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

OfxStatus getPropertySet(OfxImageEffectHandle effect, OfxPropertySetHandle* props) {
  *props = propsHandle(asEffect(effect)->props);
  return kOfxStatOK;
}

OfxStatus getParamSet(OfxImageEffectHandle effect, OfxParamSetHandle* paramSet) {
  *paramSet = paramSetHandle(*asEffect(effect));
  return kOfxStatOK;
}

OfxStatus paramDefine(OfxParamSetHandle paramSet, const char* paramType,
                      const char* name, OfxPropertySetHandle* props) {
  Param& param = asParamSet(paramSet)->params[name];
  param.type = paramType;
  if (props != nullptr) {
    *props = propsHandle(param.props);
  }
  return kOfxStatOK;
}

OfxStatus clipDefine(OfxImageEffectHandle effect, const char* name,
                     OfxPropertySetHandle* propertySet) {
  Clip& clip = asEffect(effect)->clips[name];
  if (propertySet != nullptr) {
    *propertySet = propsHandle(clip.props);
  }
  return kOfxStatOK;
}

OfxPropertySuiteV1 makePropertySuite() {
  OfxPropertySuiteV1 suite{};
  suite.propSetString = setString;
  suite.propSetInt = setInt;
  suite.propSetDouble = setDouble;
  suite.propGetString = getString;
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
    std::cerr << "request contract test failed: " << message << '\n';
    return false;
  }
  return true;
}

template <typename Fn>
bool expectThrows(Fn fn, const std::string& message) {
  try {
    fn();
  } catch (const CorridorKey::Sidecar::ProtocolError&) {
    return true;
  }
  std::cerr << "request contract test failed: " << message << '\n';
  return false;
}

bool hasOptions(const Param& param, const std::vector<std::string>& expected) {
  const auto found = param.props.strings.find(kOfxParamPropChoiceOption);
  return found != param.props.strings.end() && found->second == expected;
}

bool hasDefaultString(const Param& param, const std::string& expected) {
  const auto found = param.props.strings.find(kOfxParamPropDefault);
  return found != param.props.strings.end() && !found->second.empty() &&
         found->second.front() == expected;
}

bool hasPageChild(const Param& param, const std::string& expected) {
  const auto found = param.props.strings.find(kOfxParamPropPageChild);
  return found != param.props.strings.end() &&
         std::find(found->second.begin(), found->second.end(), expected) !=
             found->second.end();
}

bool testParameterSurface(OfxPlugin* plugin) {
  Effect descriptor;
  PropertySet context;
  setString(propsHandle(context), kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);

  bool ok = true;
  ok &= expect(plugin->mainEntry(kOfxActionDescribe, effectHandle(descriptor), nullptr,
                                 nullptr) == kOfxStatOK,
               "describe should succeed");
  ok &= expect(plugin->mainEntry(kOfxImageEffectActionDescribeInContext,
                                 effectHandle(descriptor), propsHandle(context),
                                 nullptr) == kOfxStatOK,
               "describe in context should define parameters");

  const auto& params = descriptor.params;
  ok &= expect(params.at("ck_output_mode").type == kOfxParamTypeChoice,
               "Output Mode should be a choice");
  ok &= expect(hasOptions(params.at("ck_output_mode"),
                         {"Processed RGBA", "Matte", "Straight FG",
                          "Alpha Hint View", "Checker Comp", "Status"}),
               "Output Mode choices should match PRD P0 modes");
  ok &= expect(params.at("ck_screen_color").type == kOfxParamTypeChoice,
               "Screen Color should be a choice");
  ok &= expect(hasOptions(params.at("ck_screen_color"), {"Auto", "Green", "Blue"}),
               "Screen Color choices should match PRD P0 modes");
  ok &= expect(params.at("ck_quality").type == kOfxParamTypeChoice,
               "Quality should be a choice");
  ok &= expect(hasOptions(params.at("ck_quality"),
                         {"Draft 512", "High 1024", "Full 2048"}),
               "Quality choices should match PRD P0 modes");
  ok &= expect(params.at("ck_input_color_space").type == kOfxParamTypeChoice,
               "Input Color Space should be a choice");
  ok &= expect(hasOptions(params.at("ck_input_color_space"),
                         {"Host Managed", "sRGB-Rec709 Gamma", "Linear"}),
               "Input Color Space choices should match PRD P0 modes");
  ok &= expect(params.at("ck_alpha_hint_source").type == kOfxParamTypeChoice,
               "Alpha Hint Source should remain a choice");
  ok &= expect(hasOptions(params.at("ck_alpha_hint_source"),
                         {"External", "Source Alpha", "Red Channel", "Rough Fallback"}),
               "Alpha Hint Source choices should match PRD P0 modes");
  ok &= expect(params.at("ck_backend").type == kOfxParamTypeChoice,
               "Backend should be a choice");
  ok &= expect(hasOptions(params.at("ck_backend"), {"Auto", "CUDA", "CPU", "MPS", "MLX"}),
               "Backend choices should match PRD P0 modes");

  const Param& despill = params.at("ck_despill_strength");
  ok &= expect(despill.type == kOfxParamTypeDouble,
               "Despill Strength should be a double");
  ok &= expect(despill.props.doubles.at(kOfxParamPropMin).at(0) == 0.0,
               "Despill Strength minimum should be 0");
  ok &= expect(despill.props.doubles.at(kOfxParamPropMax).at(0) == 10.0,
               "Despill Strength maximum should be 10");

  ok &= expect(params.at("ck_effective_quality").type == kOfxParamTypeString,
               "Effective Quality should be a read-only status string");
  ok &= expect(hasPageChild(params.at("Main"), "ck_effective_quality"),
               "Effective Quality should be visible on the Main page");
  ok &= expect(params.at("ck_model_status").type == kOfxParamTypeString,
               "Model Status should be a read-only status string");
  ok &= expect(params.at("ck_model_source_status").type == kOfxParamTypeString,
               "Model Source Status should be a read-only status string");
  ok &= expect(params.at("ck_install_status").type == kOfxParamTypeString,
               "Install Status should be a read-only status string");
  ok &= expect(hasDefaultString(params.at("ck_checkpoint_version"), "not_loaded"),
               "Model Version default should match sidecar wire value");
  ok &= expect(hasDefaultString(params.at("ck_model_status"), "missing"),
               "Model Status default should match sidecar wire value");
  ok &= expect(hasDefaultString(params.at("ck_model_source_status"), "ready"),
               "Model Source Status default should match sidecar wire value");
  ok &= expect(hasDefaultString(params.at("ck_install_status"), "not_installed"),
               "Install Status default should match sidecar wire value");
  ok &= expect(hasDefaultString(params.at("ck_download_status"), "not_started"),
               "Download Status default should match sidecar wire value");
  ok &= expect(params.at("ck_prepare_warmup").type == kOfxParamTypePushButton,
               "Prepare/Warm Up should be a button");
  ok &= expect(params.at("ck_download_missing_models").type == kOfxParamTypePushButton,
               "Download Missing Models should be a button");
  ok &= expect(params.at("ck_download_status").type == kOfxParamTypeString,
               "Download Status should be a read-only status string");
  ok &= expect(params.at("ck_download_progress").type == kOfxParamTypeString,
               "Download Progress should be a read-only status string");
  ok &= expect(hasPageChild(params.at("Main"), "ck_download_status"),
               "Download Status should be visible on the Main page");
  ok &= expect(hasPageChild(params.at("Main"), "ck_download_progress"),
               "Download Progress should be visible on the Main page");
  ok &= expect(params.at("ck_run_doctor").type == kOfxParamTypePushButton,
               "Run Doctor should be a button");
  ok &= expect(params.at("ck_open_log_folder").type == kOfxParamTypePushButton,
               "Open Log Folder should be a button");
  ok &= expect(params.at("ck_copy_support_bundle").type == kOfxParamTypePushButton,
               "Copy Support Bundle should be a button");
  ok &= expect(params.at("ck_doctor_status").type == kOfxParamTypeString,
               "Doctor Status should be a read-only status string");
  ok &= expect(hasPageChild(params.at("Main"), "ck_doctor_status"),
               "Doctor Status should be visible on the Main page");
  ok &= expect(params.at("ck_log_folder_status").type == kOfxParamTypeString,
               "Log Folder should be a read-only status string");
  ok &= expect(hasPageChild(params.at("Main"), "ck_log_folder_status"),
               "Log Folder status should be visible on the Main page");
  ok &= expect(params.at("ck_support_bundle_status").type == kOfxParamTypeString,
               "Support Bundle should be a read-only status string");
  ok &= expect(hasPageChild(params.at("Main"), "ck_support_bundle_status"),
               "Support Bundle status should be visible on the Main page");

  ok &= expect(params.count("ck_refiner_strength") == 0,
               "Refiner Strength should not be in the current parameter surface");
  ok &= expect(params.count("ck_auto_cleanup") == 0,
               "Auto Cleanup should not be in the current parameter surface");
  ok &= expect(params.count("ck_island_size_threshold") == 0,
               "Island Size Threshold should not be in the current parameter surface");
  return ok;
}

bool testEnumAndRangeValidation() {
  using namespace CorridorKey::Sidecar;

  bool ok = true;
  const std::vector<std::string> outputModes = {
      "processed_rgba",
      "matte",
      "straight_fg",
      "alpha_hint_view",
      "checker_comp",
      "status",
  };
  for (std::size_t index = 0; index < outputModes.size(); ++index) {
    ok &= expect(std::string(outputModeToken(static_cast<int>(index))) ==
                     outputModes[index],
                 "Output Mode choice should map to the sidecar token");
  }
  ok &= expect(std::string(screenColorToken(2)) == "blue",
               "Screen Color choice should map to blue");
  ok &= expect(std::string(qualityToken(2)) == "full_2048",
               "Quality choice should map to full_2048");
  ok &= expect(std::string(inputColorSpaceToken(1)) == "srgb_rec709",
               "Input Color Space choice should map to srgb_rec709");
  ok &= expect(std::string(alphaHintSourceToken(3)) == "rough_fallback",
               "Alpha Hint Source choice should map to rough_fallback");
  ok &= expect(std::string(backendToken(4)) == "mlx",
               "Backend choice should map to mlx");
  ok &= expect(isValidDespillStrength(0.0) && isValidDespillStrength(10.0),
               "Despill Strength bounds should be accepted");
  ok &= expect(!isValidDespillStrength(-0.1) && !isValidDespillStrength(10.1),
               "Despill Strength outside 0..10 should be rejected");

  ok &= expectThrows([]() { (void)outputModeToken(6); },
                     "invalid Output Mode choice should throw");
  ok &= expectThrows([]() { (void)screenColorToken(3); },
                     "invalid Screen Color choice should throw");
  ok &= expectThrows([]() { (void)qualityToken(-1); },
                     "invalid Quality choice should throw");
  ok &= expectThrows([]() { (void)inputColorSpaceToken(4); },
                     "invalid Input Color Space choice should throw");
  ok &= expectThrows([]() { (void)alphaHintSourceToken(4); },
                     "invalid Alpha Hint Source choice should throw");
  ok &= expectThrows([]() { (void)backendToken(5); },
                     "invalid Backend choice should throw");
  return ok;
}

std::filesystem::path contractTempRoot() {
  return std::filesystem::temp_directory_path() / "corridorkey-render-request-contract";
}

CorridorKey::Sidecar::InferRequestContract makeContract(
    double despill,
    std::filesystem::path root = contractTempRoot()) {
  return CorridorKey::Sidecar::InferRequestContract{
      "job-request-contract",
      "frame-42",
      10,
      20,
      74,
      92,
      (root / "source-frame.ckfb").string(),
      (root / "alpha-hint-frame.ckfb").string(),
      "external",
      "green",
      "high_1024",
      "host_managed",
      despill,
      "stub",
      "processed_rgba",
  };
}

std::map<std::string, std::string> goldenPayload() {
  return {
      {"alpha_hint_frame_blob_path",
       "/tmp/corridorkey-render-request-contract/alpha-hint-frame.ckfb"},
      {"alpha_hint_source", "external"},
      {"backend", "stub"},
      {"despill_strength", "5"},
      {"frame_id", "frame-42"},
      {"input_color_space", "host_managed"},
      {"job_id", "job-request-contract"},
      {"output_mode", "processed_rgba"},
      {"quality", "high_1024"},
      {"render_window_x1", "10"},
      {"render_window_x2", "74"},
      {"render_window_y1", "20"},
      {"render_window_y2", "92"},
      {"screen_color", "green"},
      {"source_frame_blob_path",
       "/tmp/corridorkey-render-request-contract/source-frame.ckfb"},
  };
}

bool testDeterministicInferSerialization() {
  using namespace CorridorKey::Sidecar;

  bool ok = true;
  ok &= expect(requestHashForPayload(goldenPayload()) == "645daac84e998b34",
               "request hash should match the cross-language golden value");
  const InferRequestContract contract = makeContract(5.0);
  const SidecarRequest request = makeInferRequest("req-infer-1", contract);
  const std::string first = encodeRequest(request);
  const std::string second = encodeRequest(request);

  ok &= expect(first == second, "infer request serialization should be deterministic");
  ok &= expect(first.find("\"command\":\"infer\"") != std::string::npos,
               "infer request should use infer command");
  ok &= expect(first.find("\"job_id\":\"job-request-contract\"") != std::string::npos,
               "infer request should include a cancellable job id");
  ok &= expect(first.find("\"source_frame_blob_path\"") != std::string::npos,
               "infer request should include source temp frame blob path");
  ok &= expect(first.find("\"alpha_hint_frame_blob_path\"") != std::string::npos,
               "infer request should include alpha hint temp frame blob path");
  ok &= expect(first.find("source_media_path") == std::string::npos,
               "infer request must not include raw source media path");
  ok &= expect(first.find("project_path") == std::string::npos,
               "infer request must not include project path");

  const auto payloadA = inferRequestPayload(makeContract(5.0));
  const auto payloadB = inferRequestPayload(makeContract(5.1));
  auto changedJob = payloadA;
  changedJob["job_id"] = "job-request-contract-2";
  ok &= expect(payloadA.at("despill_strength") == "5",
               "Despill Strength should be serialized in the request payload");
  ok &= expect(payloadA.at("request_hash") != payloadB.at("request_hash"),
               "Despill Strength should affect the request hash");
  ok &= expect(payloadA.at("request_hash") != requestHashForPayload(changedJob),
               "job_id should affect the request hash");

  InferRequestContract rawPathContract = contract;
  rawPathContract.sourceFrameBlobPath = "/Users/example/project/plate.exr";
  ok &= expectThrows(
      [&rawPathContract]() { (void)inferRequestPayload(rawPathContract); },
      "raw media-like paths outside temp storage should be rejected");
  InferRequestContract unscopedTempContract = contract;
  unscopedTempContract.sourceFrameBlobPath =
      (std::filesystem::temp_directory_path() / "unscoped-frame.ckfb").string();
  ok &= expectThrows(
      [&unscopedTempContract]() { (void)inferRequestPayload(unscopedTempContract); },
      "temp paths outside a CorridorKey per-job namespace should be rejected");
  InferRequestContract filenameNamespaceContract = contract;
  filenameNamespaceContract.sourceFrameBlobPath =
      (std::filesystem::temp_directory_path() / "corridorkey-plate.exr").string();
  ok &= expectThrows(
      [&filenameNamespaceContract]() { (void)inferRequestPayload(filenameNamespaceContract); },
      "temp paths must not accept CorridorKey-looking raw filenames");
  InferRequestContract wrongExtensionContract = contract;
  wrongExtensionContract.sourceFrameBlobPath =
      (std::filesystem::temp_directory_path() / "corridorkey-render-request-contract" /
       "plate.exr")
          .string();
  ok &= expectThrows(
      [&wrongExtensionContract]() { (void)inferRequestPayload(wrongExtensionContract); },
      "temp frame blob paths should require the ckfb extension");
  InferRequestContract wrongNamespaceContract = contract;
  wrongNamespaceContract.sourceFrameBlobPath =
      (std::filesystem::temp_directory_path() / "corridorkey-cache" /
       "source-frame.ckfb")
          .string();
  ok &= expectThrows(
      [&wrongNamespaceContract]() { (void)inferRequestPayload(wrongNamespaceContract); },
      "temp frame blob paths should require a render job namespace");
  InferRequestContract splitJobContract = contract;
  splitJobContract.alphaHintFrameBlobPath =
      (std::filesystem::temp_directory_path() / "corridorkey-render-other-contract" /
       "alpha-hint-frame.ckfb")
          .string();
  ok &= expectThrows(
      [&splitJobContract]() { (void)inferRequestPayload(splitJobContract); },
      "source and alpha frame blobs should share one render job namespace");

  return ok;
}

bool testTempFrameBlobPathAliases() {
  using namespace CorridorKey::Sidecar;

  const std::filesystem::path tempRoot = std::filesystem::temp_directory_path();
  const std::filesystem::path root = tempRoot / "corridorkey-render-request-contract";
  bool ok = true;
  ok &= expect(tempFrameBlobJobPath((root / "source-frame.ckfb").string()).has_value(),
               "temp frame blob path should accept the raw temp path");

  std::error_code ec;
  const std::filesystem::path resolvedTempRoot =
      std::filesystem::weakly_canonical(tempRoot, ec);
  if (!ec && resolvedTempRoot != tempRoot) {
    ok &= expect(
        tempFrameBlobJobPath(
            (resolvedTempRoot / "corridorkey-render-request-contract" /
             "source-frame.ckfb")
                .string())
            .has_value(),
        "temp frame blob path should accept resolved temp path aliases");
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = true;

  ok &= expect(OfxGetNumberOfPlugins() == 1, "exactly one plugin should be exported");
  OfxPlugin* plugin = OfxGetPlugin(0);
  ok &= expect(plugin != nullptr, "plugin should be available");

  OfxHost host{};
  host.fetchSuite = fetchSuite;
  plugin->setHost(&host);
  ok &= expect(plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr) ==
                   kOfxStatOK,
               "plugin load should fetch fake suites");

  ok &= testParameterSurface(plugin);
  ok &= testEnumAndRangeValidation();
  ok &= testDeterministicInferSerialization();
  ok &= testTempFrameBlobPathAliases();
  return ok ? 0 : 1;
}
