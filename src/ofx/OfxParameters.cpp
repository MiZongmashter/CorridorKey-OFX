#include "CorridorKeyPlugin.h"

#include "ofxParam.h"

#include <array>
#include <string>
#include <vector>

namespace CorridorKey::OFX {

OfxParameterSuiteV1* parameterSuite();

namespace {

struct FieldDefinition {
  std::string name;
  std::string label;
  std::string defaultValue;
};

struct ChoiceDefinition {
  std::string name;
  std::string label;
  int defaultValue;
  std::vector<std::string> options;
};

const std::array<FieldDefinition, 20>& statusFields() {
  static const std::array<FieldDefinition, 20> fields = {{
      {"ck_plugin_version", "Plugin Version", "0.1.0"},
      {"ck_sidecar_version", "Sidecar Version", "unknown"},
      {"ck_runtime_state", "Sidecar State", "not connected"},
      {"ck_checkpoint_version", "Model Version", "not_loaded"},
      {"ck_model_status", "Model Status", "missing"},
      {"ck_model_source_status", "Model Source Status", "ready"},
      {"ck_install_status", "Install Status", "not_installed"},
      {"ck_download_status", "Download Status", "not_started"},
      {"ck_download_progress", "Download Progress", "0/0"},
      {"ck_backend_status", "Backend Status", "stub"},
      {"ck_compute_device", "GPU", "unknown"},
      {"ck_memory", "VRAM", "unknown"},
      {"ck_cache", "Cache", "disabled"},
      {"ck_warmup", "Warmup", "cold"},
      {"ck_doctor_status", "Doctor Status", "not run"},
      {"ck_log_folder_status", "Log Folder", "manual fallback"},
      {"ck_support_bundle_status", "Support Bundle", "not created"},
      {"ck_last_error", "Last Error", ""},
      {"ck_guide_source_status", "Guide Source Status", "missing hint"},
      {"ck_effective_quality", "Effective Quality", "not evaluated"},
  }};
  return fields;
}

OfxStatus defineReadonlyString(OfxParamSetHandle paramSet,
                               const FieldDefinition& field) {
  OfxPropertySetHandle props = nullptr;
  const OfxStatus status =
      parameterSuite()->paramDefine(paramSet, kOfxParamTypeString, field.name.c_str(),
                                    &props);
  if (status != kOfxStatOK) {
    return status;
  }
  propertySuite()->propSetString(props, kOfxPropLabel, 0, field.label.c_str());
  propertySuite()->propSetString(props, kOfxParamPropScriptName, 0, field.name.c_str());
  propertySuite()->propSetString(props, kOfxParamPropStringMode, 0,
                                 kOfxParamStringIsLabel);
  propertySuite()->propSetString(props, kOfxParamPropDefault, 0,
                                 field.defaultValue.c_str());
  propertySuite()->propSetInt(props, kOfxParamPropEnabled, 0, 0);
  propertySuite()->propSetInt(props, kOfxParamPropPersistant, 0, 0);
  propertySuite()->propSetInt(props, kOfxParamPropEvaluateOnChange, 0, 0);
  propertySuite()->propSetInt(props, kOfxParamPropPluginMayWrite, 0, 1);
  return kOfxStatOK;
}

OfxStatus defineChoice(OfxParamSetHandle paramSet,
                       const ChoiceDefinition& definition) {
  OfxPropertySetHandle props = nullptr;
  const OfxStatus status =
      parameterSuite()->paramDefine(paramSet, kOfxParamTypeChoice,
                                    definition.name.c_str(), &props);
  if (status != kOfxStatOK) {
    return status;
  }

  propertySuite()->propSetString(props, kOfxPropLabel, 0, definition.label.c_str());
  propertySuite()->propSetString(props, kOfxParamPropScriptName, 0,
                                 definition.name.c_str());
  propertySuite()->propSetInt(props, kOfxParamPropDefault, 0, definition.defaultValue);
  for (std::size_t index = 0; index < definition.options.size(); ++index) {
    propertySuite()->propSetString(props, kOfxParamPropChoiceOption,
                                   static_cast<int>(index),
                                   definition.options[index].c_str());
  }
  return kOfxStatOK;
}

OfxStatus defineDoubleRange(OfxParamSetHandle paramSet,
                            const std::string& name,
                            const std::string& label,
                            double defaultValue,
                            double minimum,
                            double maximum) {
  OfxPropertySetHandle props = nullptr;
  const OfxStatus status =
      parameterSuite()->paramDefine(paramSet, kOfxParamTypeDouble, name.c_str(), &props);
  if (status != kOfxStatOK) {
    return status;
  }

  propertySuite()->propSetString(props, kOfxPropLabel, 0, label.c_str());
  propertySuite()->propSetString(props, kOfxParamPropScriptName, 0, name.c_str());
  if (propertySuite()->propSetDouble != nullptr) {
    propertySuite()->propSetDouble(props, kOfxParamPropDefault, 0, defaultValue);
    propertySuite()->propSetDouble(props, kOfxParamPropMin, 0, minimum);
    propertySuite()->propSetDouble(props, kOfxParamPropMax, 0, maximum);
    propertySuite()->propSetDouble(props, kOfxParamPropDisplayMin, 0, minimum);
    propertySuite()->propSetDouble(props, kOfxParamPropDisplayMax, 0, maximum);
    propertySuite()->propSetDouble(props, kOfxParamPropIncrement, 0, 0.1);
  }
  propertySuite()->propSetInt(props, kOfxParamPropDigits, 0, 2);
  return kOfxStatOK;
}

OfxStatus definePushButton(OfxParamSetHandle paramSet,
                           const std::string& name,
                           const std::string& label) {
  OfxPropertySetHandle props = nullptr;
  const OfxStatus status =
      parameterSuite()->paramDefine(paramSet, kOfxParamTypePushButton, name.c_str(),
                                    &props);
  if (status != kOfxStatOK) {
    return status;
  }

  propertySuite()->propSetString(props, kOfxPropLabel, 0, label.c_str());
  propertySuite()->propSetString(props, kOfxParamPropScriptName, 0, name.c_str());
  return kOfxStatOK;
}

OfxStatus defineMainPage(OfxParamSetHandle paramSet,
                         const std::vector<std::string>& children) {
  OfxPropertySetHandle props = nullptr;
  const OfxStatus status =
      parameterSuite()->paramDefine(paramSet, kOfxParamTypePage, "Main", &props);
  if (status != kOfxStatOK) {
    return status;
  }

  propertySuite()->propSetString(props, kOfxPropLabel, 0, "Main");
  for (std::size_t index = 0; index < children.size(); ++index) {
    propertySuite()->propSetString(props, kOfxParamPropPageChild,
                                   static_cast<int>(index),
                                   children[index].c_str());
  }
  return kOfxStatOK;
}

}  // namespace

OfxStatus defineParameters(OfxImageEffectHandle effect) {
  if (effectSuite() == nullptr || propertySuite() == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }
  if (parameterSuite() == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }

  OfxPropertySetHandle effectProps = nullptr;
  OfxStatus status = effectSuite()->getPropertySet(effect, &effectProps);
  if (status != kOfxStatOK) {
    return status;
  }
  propertySuite()->propSetString(effectProps, kOfxPluginPropParamPageOrder, 0, "Main");

  OfxParamSetHandle paramSet = nullptr;
  status = effectSuite()->getParamSet(effect, &paramSet);
  if (status != kOfxStatOK) {
    return status;
  }

  const std::vector<ChoiceDefinition> choices = {
      {"ck_output_mode",
       "Output Mode",
       0,
       {"Processed RGBA", "Matte", "Straight FG", "Alpha Hint View",
        "Checker Comp", "Status"}},
      {"ck_screen_color", "Screen Color", 0, {"Auto", "Green", "Blue"}},
      {"ck_quality", "Quality", 1, {"Draft 512", "High 1024", "Full 2048"}},
      {"ck_input_color_space",
       "Input Color Space",
       0,
       {"Host Managed", "sRGB-Rec709 Gamma", "Linear"}},
      {"ck_alpha_hint_source",
       "Alpha Hint Source",
       0,
       {"External", "Source Alpha", "Red Channel", "Rough Fallback"}},
      {"ck_backend",
       "Backend",
       0,
       {"Auto", "CUDA", "CPU", "MPS", "MLX"}},
  };

  for (const ChoiceDefinition& choice : choices) {
    status = defineChoice(paramSet, choice);
    if (status != kOfxStatOK) {
      return status;
    }
  }

  status = defineDoubleRange(paramSet, "ck_despill_strength", "Despill Strength",
                             5.0, 0.0, 10.0);
  if (status != kOfxStatOK) {
    return status;
  }

  status = definePushButton(paramSet, "ck_prepare_warmup", "Prepare/Warm Up");
  if (status != kOfxStatOK) {
    return status;
  }
  status = definePushButton(paramSet, "ck_download_missing_models",
                            "Download Missing Models");
  if (status != kOfxStatOK) {
    return status;
  }
  status = definePushButton(paramSet, "ck_run_doctor", "Run Doctor");
  if (status != kOfxStatOK) {
    return status;
  }
  status = definePushButton(paramSet, "ck_open_log_folder", "Open Log Folder");
  if (status != kOfxStatOK) {
    return status;
  }
  status = definePushButton(paramSet, "ck_copy_support_bundle", "Copy Support Bundle");
  if (status != kOfxStatOK) {
    return status;
  }
  for (const FieldDefinition& field : statusFields()) {
    status = defineReadonlyString(paramSet, field);
    if (status != kOfxStatOK) {
      return status;
    }
  }
  return defineMainPage(paramSet, {
                                      "ck_output_mode",
                                      "ck_screen_color",
                                      "ck_quality",
                                      "ck_input_color_space",
                                      "ck_alpha_hint_source",
                                      "ck_guide_source_status",
                                      "ck_backend",
                                      "ck_despill_strength",
                                      "ck_prepare_warmup",
                                      "ck_download_missing_models",
                                      "ck_run_doctor",
                                      "ck_doctor_status",
                                      "ck_open_log_folder",
                                      "ck_log_folder_status",
                                      "ck_copy_support_bundle",
                                      "ck_support_bundle_status",
                                      "ck_plugin_version",
                                      "ck_runtime_state",
                                      "ck_model_status",
                                      "ck_download_status",
                                      "ck_download_progress",
                                      "ck_effective_quality",
                                      "ck_backend_status",
                                      "ck_last_error",
                                  });
}

}  // namespace CorridorKey::OFX
