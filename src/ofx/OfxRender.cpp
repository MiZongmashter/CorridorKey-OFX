#include "CorridorKeyPlugin.h"

#include "core/AlphaHint.h"
#include "core/ColorAlphaContract.h"
#include "core/PixelBuffer.h"
#include "core/PixelConvert.h"
#include "core/StatusFrame.h"

#include "ofxParam.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

struct CorridorKeyRuntimeStatus {
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

extern "C" int CorridorKeyProbeRuntimeStatus(const char* bundleRoot,
                                             const char* runtimePath,
                                             int timeoutMilliseconds,
                                             void* outputBuffer,
                                             std::size_t outputSize);
struct CorridorKeyRuntimeInfer {
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
struct CorridorKeyRuntimeWarmup {
  bool ok = false;
  char jobId[128] = {};
  char backend[64] = {};
  char backendStatus[64] = {};
  char modelStatus[64] = {};
  char modelSourceStatus[64] = {};
  char installStatus[64] = {};
  char warmup[64] = {};
  char errorCode[64] = {};
  char lastError[512] = {};
};
extern "C" int CorridorKeyRunStubInfer(const char* bundleRoot,
                                       const char* runtimePath,
                                       int timeoutMilliseconds,
                                       const char* jobId,
                                       const char* frameId,
                                       int renderWindowX1,
                                       int renderWindowY1,
                                       int renderWindowX2,
                                       int renderWindowY2,
                                       const char* sourceFrameBlobPath,
                                       const char* alphaHintFrameBlobPath,
                                       int alphaHintChoice,
                                       int screenColorChoice,
                                       int qualityChoice,
                                       int inputColorSpaceChoice,
                                       double despillStrength,
                                       int backendChoice,
                                       int outputModeChoice,
                                       int (*cancelRequested)(void*),
                                       void* cancelContext,
                                       void* outputBuffer,
                                       std::size_t outputSize);
extern "C" int CorridorKeyRunWarmup(const char* bundleRoot,
                                    const char* runtimePath,
                                    int timeoutMilliseconds,
                                    const char* jobId,
                                    int backendChoice,
                                    int qualityChoice,
                                    int (*cancelRequested)(void*),
                                    void* cancelContext,
                                    void* outputBuffer,
                                    std::size_t outputSize);

namespace CorridorKey::OFX {

OfxParameterSuiteV1* parameterSuite();

namespace {

inline constexpr const char* kAlphaHintClipName = "AlphaHint";
inline constexpr int kRuntimeStatusProbeTimeoutMs = 5000;
inline constexpr int kRuntimeInferTimeoutMs = 60000;
inline constexpr int kRuntimeWarmupTimeoutMs = 5000;
inline constexpr int kRuntimeInferTimeoutMaxMs = 300000;

struct AbortCallbackContext {
  OfxImageEffectHandle instance = nullptr;
};

class RenderCancelled : public std::runtime_error {
 public:
  explicit RenderCancelled(const std::string& phase)
      : std::runtime_error("cancelled during " + phase) {}
};

bool hostAbortRequested(OfxImageEffectHandle instance) {
  return effectSuite() != nullptr && effectSuite()->abort != nullptr &&
         effectSuite()->abort(instance) != 0;
}

int cancelRequestedFromHost(void* context) {
  const auto* abortContext = static_cast<const AbortCallbackContext*>(context);
  return abortContext != nullptr && hostAbortRequested(abortContext->instance) ? 1 : 0;
}

class AbortPoller {
 public:
  explicit AbortPoller(OfxImageEffectHandle instance) : instance_(instance) {}

  void throwIfRequested(const char* phase) const {
    if (hostAbortRequested(instance_)) {
      throw RenderCancelled(phase);
    }
  }

 private:
  OfxImageEffectHandle instance_ = nullptr;
};

class ScopedImage {
 public:
  ScopedImage() = default;
  explicit ScopedImage(OfxPropertySetHandle image) : image_(image) {}
  ~ScopedImage() {
    reset(nullptr);
  }

  ScopedImage(const ScopedImage&) = delete;
  ScopedImage& operator=(const ScopedImage&) = delete;

  void reset(OfxPropertySetHandle image) {
    if (image_ != nullptr && effectSuite() != nullptr) {
      effectSuite()->clipReleaseImage(image_);
    }
    image_ = image;
  }

 private:
  OfxPropertySetHandle image_ = nullptr;
};

int runtimeInferTimeoutMs(const Core::RectI& renderWindow) {
  const long long width = std::max(0, Core::width(renderWindow));
  const long long height = std::max(0, Core::height(renderWindow));
  const long long pixels = width * height;
  const long long chunks = (pixels + 1023LL) / 1024LL;
  const long long timeout = static_cast<long long>(kRuntimeInferTimeoutMs) +
                            chunks * 20LL;
  return static_cast<int>(std::clamp(
      timeout,
      static_cast<long long>(kRuntimeInferTimeoutMs),
      static_cast<long long>(kRuntimeInferTimeoutMaxMs)));
}

bool getStringProperty(OfxPropertySetHandle props, const char* property, char*& value) {
  value = nullptr;
  return propertySuite()->propGetString(props, property, 0, &value) == kOfxStatOK &&
         value != nullptr;
}

OfxStatus ofxStatusForFormatStatus(Core::FormatStatus status) {
  switch (status) {
    case Core::FormatStatus::Ok:
    case Core::FormatStatus::ByteFallback:
      return kOfxStatOK;
    case Core::FormatStatus::UnsupportedComponents:
    case Core::FormatStatus::UnsupportedPixelDepth:
    case Core::FormatStatus::UnsupportedFielding:
      return kOfxStatErrUnsupported;
    case Core::FormatStatus::MissingData:
    case Core::FormatStatus::InvalidRowBytes:
      return kOfxStatFailed;
  }
  return kOfxStatFailed;
}

Core::FormatStatus alphaHintInputStatus(const Core::ImageFormat& format) {
  if (format.components == Core::PixelComponents::Alpha) {
    switch (format.depth) {
      case Core::PixelDepth::Byte:
        return Core::FormatStatus::ByteFallback;
      case Core::PixelDepth::Half:
      case Core::PixelDepth::Float:
        return Core::FormatStatus::Ok;
      case Core::PixelDepth::Short:
      case Core::PixelDepth::Unknown:
        return Core::FormatStatus::UnsupportedPixelDepth;
    }
  }
  return Core::inputFormatStatus(format);
}

OfxStatus imageView(OfxPropertySetHandle image,
                    bool output,
                    Core::PixelBufferView& view,
                    bool allowAlphaInput = false) {
  char* components = nullptr;
  char* pixelDepth = nullptr;
  if (!getStringProperty(image, kOfxImageEffectPropComponents, components) ||
      !getStringProperty(image, kOfxImageEffectPropPixelDepth, pixelDepth)) {
    return kOfxStatFailed;
  }
  view.format = Core::imageFormatFromOfx(components, pixelDepth);
  const Core::FormatStatus formatStatus = output
                                              ? Core::outputFormatStatus(view.format)
                                              : (allowAlphaInput
                                                     ? alphaHintInputStatus(view.format)
                                                     : Core::inputFormatStatus(view.format));
  if (!Core::isSuccess(formatStatus)) {
    return ofxStatusForFormatStatus(formatStatus);
  }

  void* data = nullptr;
  OfxStatus status = propertySuite()->propGetPointer(image, kOfxImagePropData, 0, &data);
  if (status != kOfxStatOK || data == nullptr) {
    return status == kOfxStatOK ? kOfxStatFailed : status;
  }
  status = propertySuite()->propGetInt(image, kOfxImagePropRowBytes, 0, &view.rowBytes);
  if (status != kOfxStatOK) {
    return status;
  }
  OfxRectI bounds{};
  status = propertySuite()->propGetIntN(image, kOfxImagePropBounds, 4, &bounds.x1);
  if (status != kOfxStatOK) {
    return status;
  }
  view.data = data;
  view.bounds = Core::RectI{bounds.x1, bounds.y1, bounds.x2, bounds.y2};
  return kOfxStatOK;
}

std::string envValue(const char* key) {
  const char* value = std::getenv(key);
  return value == nullptr ? std::string{} : std::string{value};
}

bool testOverridesAllowed() {
  return envValue("CORRIDORKEY_TEST_FAULTS_ALLOWED") == "1";
}

std::string moduleBundleRoot() {
#if !defined(_WIN32)
  Dl_info info{};
  if (::dladdr(reinterpret_cast<void*>(&CorridorKeyProbeRuntimeStatus), &info) == 0 ||
      info.dli_fname == nullptr) {
    return {};
  }
  std::filesystem::path current = std::filesystem::path{info.dli_fname};
  while (!current.empty()) {
    if (current.filename().string().find(".ofx.bundle") != std::string::npos) {
      return current.string();
    }
    const std::filesystem::path parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }
#endif
  return {};
}

std::string statusProbeBundleRoot() {
  const std::string configured =
      testOverridesAllowed() ? envValue("CORRIDORKEY_STATUS_PROBE_BUNDLE") : "";
  return configured.empty() ? moduleBundleRoot() : configured;
}

std::string statusProbeRuntimePath() {
  const std::string configured =
      testOverridesAllowed() ? envValue("CORRIDORKEY_STATUS_PROBE_PY") : "";
  if (!configured.empty()) {
    return configured;
  }
  return std::string{"/usr/bin/"} + "py" + "thon3";
}

std::filesystem::path bundleResourcesRoot(const std::filesystem::path& bundleRoot) {
  return bundleRoot / "Contents" / "Resources";
}

std::filesystem::path bundledPythonLauncherPath(
    const std::filesystem::path& bundleRoot) {
  const std::filesystem::path resources = bundleResourcesRoot(bundleRoot);
#if defined(_WIN32)
  return resources / "python" / "python.exe";
#else
  return resources / "python" / "bin" / "python3";
#endif
}

const char* backendStatusOrBackend(const char* backendStatus, const char* backend) {
  return backendStatus != nullptr && backendStatus[0] != '\0' ? backendStatus : backend;
}

std::vector<Core::StatusFrameLine> statusLinesFromSurface(
    const CorridorKeyRuntimeStatus& surface) {
  return {
      {"State", surface.state[0] == '\0' ? "unknown" : surface.state},
      {"Version", surface.checkpoint[0] == '\0' ? "not loaded" : surface.checkpoint},
      {"Model", surface.modelStatus[0] == '\0' ? "unknown" : surface.modelStatus},
      {"Model Source", surface.modelSourceStatus[0] == '\0' ? "unknown" : surface.modelSourceStatus},
      {"Install", surface.installStatus[0] == '\0' ? "unknown" : surface.installStatus},
      {"Download", surface.downloadStatus[0] == '\0' ? "unknown" : surface.downloadStatus},
      {"Progress", surface.downloadProgress[0] == '\0' ? "unknown" : surface.downloadProgress},
      {"GPU", surface.compute[0] == '\0' ? "unknown" : surface.compute},
      {"Backend", surface.backend[0] == '\0' ? "unknown" : surface.backend},
      {"Backend Status", backendStatusOrBackend(surface.backendStatus, surface.backend)},
      {"Warmup", surface.warmup[0] == '\0' ? "unknown" : surface.warmup},
      {"Cache", surface.cache[0] == '\0' ? "unknown" : surface.cache},
      {"VRAM", surface.memory[0] == '\0' ? "unknown" : surface.memory},
      {"Last Error", surface.lastError},
  };
}

Core::StatusFrameContent statusContentFromFailure(
    const CorridorKeyRuntimeStatus& surface) {
  return Core::StatusFrameContent{
      Core::StatusFrameSeverity::Error,
      "Sidecar Error",
      surface.errorCode[0] == '\0' ? "runtime unavailable" : surface.errorCode,
      statusLinesFromSurface(surface),
  };
}

OfxStatus renderFailureStatusFrame(const Core::PixelBufferView& output,
                                   const OfxRectI& renderWindow,
                                   const CorridorKeyRuntimeStatus& surface) {
  const Core::FormatStatus frameStatus = Core::renderStatusFrame(
      output,
      Core::RectI{renderWindow.x1, renderWindow.y1, renderWindow.x2, renderWindow.y2},
      statusContentFromFailure(surface));
  return ofxStatusForFormatStatus(frameStatus);
}

void setStatusParamValue(OfxParamSetHandle paramSet,
                         const char* name,
                         const char* value) {
  if (parameterSuite()->paramGetHandle == nullptr ||
      parameterSuite()->paramSetValue == nullptr) {
    return;
  }
  OfxParamHandle param = nullptr;
  if (parameterSuite()->paramGetHandle(paramSet, name, &param, nullptr) != kOfxStatOK) {
    return;
  }
  (void)parameterSuite()->paramSetValue(param, value == nullptr ? "" : value);
}

void setStatusParam(OfxImageEffectHandle instance,
                    const char* name,
                    const char* value) {
  if (effectSuite() == nullptr || parameterSuite() == nullptr) {
    return;
  }
  OfxParamSetHandle paramSet = nullptr;
  if (effectSuite()->getParamSet(instance, &paramSet) != kOfxStatOK) {
    return;
  }
  setStatusParamValue(paramSet, name, value);
}

void updateStatusParams(OfxImageEffectHandle instance,
                        const CorridorKeyRuntimeStatus& surface) {
  if (effectSuite() == nullptr || parameterSuite() == nullptr) {
    return;
  }
  OfxParamSetHandle paramSet = nullptr;
  if (effectSuite()->getParamSet(instance, &paramSet) != kOfxStatOK) {
    return;
  }
  setStatusParamValue(paramSet, "ck_runtime_state", surface.state);
  setStatusParamValue(paramSet, "ck_checkpoint_version", surface.checkpoint);
  setStatusParamValue(paramSet, "ck_model_status", surface.modelStatus);
  setStatusParamValue(paramSet, "ck_model_source_status", surface.modelSourceStatus);
  setStatusParamValue(paramSet, "ck_install_status", surface.installStatus);
  setStatusParamValue(paramSet, "ck_download_status", surface.downloadStatus);
  setStatusParamValue(paramSet, "ck_download_progress", surface.downloadProgress);
  setStatusParamValue(paramSet, "ck_backend_status",
                      backendStatusOrBackend(surface.backendStatus, surface.backend));
  setStatusParamValue(paramSet, "ck_compute_device", surface.compute);
  setStatusParamValue(paramSet, "ck_memory", surface.memory);
  setStatusParamValue(paramSet, "ck_cache", surface.cache);
  setStatusParamValue(paramSet, "ck_warmup", surface.warmup);
  setStatusParamValue(paramSet, "ck_last_error", surface.lastError);
}

OfxStatus updateAndRenderFailureStatusFrame(OfxImageEffectHandle instance,
                                            const Core::PixelBufferView& output,
                                            const OfxRectI& renderWindow,
                                            const CorridorKeyRuntimeStatus& surface) {
  updateStatusParams(instance, surface);
  return renderFailureStatusFrame(output, renderWindow, surface);
}

Core::AlphaHintSource alphaHintSourceFromChoice(int choice) {
  switch (choice) {
    case 0:
      return Core::AlphaHintSource::External;
    case 1:
      return Core::AlphaHintSource::SourceAlpha;
    case 2:
      return Core::AlphaHintSource::RedChannel;
    case 3:
      return Core::AlphaHintSource::RoughFallback;
    default:
      return Core::AlphaHintSource::External;
  }
}

Core::InputColorMode inputColorModeFromChoice(int choice) {
  switch (choice) {
    case 1:
      return Core::InputColorMode::SrgbRec709Gamma;
    case 2:
      return Core::InputColorMode::Linear;
    default:
      return Core::InputColorMode::HostManaged;
  }
}

int choiceParamAtTime(OfxImageEffectHandle instance,
                      const char* name,
                      OfxTime time,
                      int fallback) {
  if (effectSuite() == nullptr || parameterSuite() == nullptr ||
      parameterSuite()->paramGetValueAtTime == nullptr) {
    return fallback;
  }

  OfxParamSetHandle paramSet = nullptr;
  if (effectSuite()->getParamSet(instance, &paramSet) != kOfxStatOK) {
    return fallback;
  }
  OfxParamHandle param = nullptr;
  if (parameterSuite()->paramGetHandle(paramSet, name, &param, nullptr) != kOfxStatOK) {
    return fallback;
  }

  int choice = fallback;
  if (parameterSuite()->paramGetValueAtTime(param, time, &choice) != kOfxStatOK) {
    return fallback;
  }
  return choice;
}

int alphaHintChoiceAtTime(OfxImageEffectHandle instance, OfxTime time) {
  return choiceParamAtTime(instance, "ck_alpha_hint_source", time, 0);
}

double doubleParamAtTime(OfxImageEffectHandle instance,
                         const char* name,
                         OfxTime time,
                         double fallback) {
  if (effectSuite() == nullptr || parameterSuite() == nullptr ||
      parameterSuite()->paramGetValueAtTime == nullptr) {
    return fallback;
  }

  OfxParamSetHandle paramSet = nullptr;
  if (effectSuite()->getParamSet(instance, &paramSet) != kOfxStatOK) {
    return fallback;
  }
  OfxParamHandle param = nullptr;
  if (parameterSuite()->paramGetHandle(paramSet, name, &param, nullptr) != kOfxStatOK) {
    return fallback;
  }

  double value = fallback;
  if (parameterSuite()->paramGetValueAtTime(param, time, &value) != kOfxStatOK) {
    return fallback;
  }
  return value;
}

bool choiceParamDefined(OfxImageEffectHandle instance, const char* name) {
  if (effectSuite() == nullptr || parameterSuite() == nullptr) {
    return false;
  }
  OfxParamSetHandle paramSet = nullptr;
  if (effectSuite()->getParamSet(instance, &paramSet) != kOfxStatOK) {
    return false;
  }
  OfxParamHandle param = nullptr;
  return parameterSuite()->paramGetHandle(paramSet, name, &param, nullptr) == kOfxStatOK;
}

const char* premultiplicationForOutputMode(int outputModeChoice) {
  Core::OutputMode mode = Core::OutputMode::ProcessedRgba;
  switch (outputModeChoice) {
    case 1:
      mode = Core::OutputMode::Matte;
      break;
    case 2:
      mode = Core::OutputMode::StraightForeground;
      break;
    case 3:
      mode = Core::OutputMode::AlphaHintView;
      break;
    case 4:
      mode = Core::OutputMode::CheckerComp;
      break;
    case 5:
      mode = Core::OutputMode::StatusFrame;
      break;
    default:
      break;
  }

  switch (Core::outputPremultiplication(mode)) {
    case Core::Premultiplication::Opaque:
      return kOfxImageOpaque;
    case Core::Premultiplication::Unpremultiplied:
      return kOfxImageUnPreMultiplied;
    case Core::Premultiplication::Premultiplied:
      return kOfxImagePreMultiplied;
  }
  return kOfxImagePreMultiplied;
}

std::string inferFrameId(OfxTime time) {
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream << "frame-" << std::fixed << std::setprecision(3) << time;
  std::string result = stream.str();
  for (char& value : result) {
    if (value == '.' || value == '-') {
      value = '_';
    }
  }
  return result;
}

std::filesystem::path renderTempRoot(const std::string& frameId) {
  static std::atomic<unsigned long long> counter{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream suffix;
  suffix.imbue(std::locale::classic());
  suffix << frameId << "_" << stamp << "_" << counter.fetch_add(1);
  return std::filesystem::temp_directory_path() / ("corridorkey-render-" + suffix.str());
}

std::string jobIdForFrame(const std::string& frameId) {
  static std::atomic<unsigned long long> counter{0};
  std::ostringstream id;
  id.imbue(std::locale::classic());
  id << "job-" << frameId << "-" << counter.fetch_add(1);
  return id.str();
}

bool outputModeNeedsProcessed(int outputModeChoice) {
  return outputModeChoice == 0 || outputModeChoice == 4;
}

bool outputModeNeedsStraight(int outputModeChoice) {
  return outputModeChoice == 2;
}

bool outputModeNeedsAlpha(int outputModeChoice) {
  return outputModeChoice == 1 || outputModeChoice == 2 ||
         outputModeChoice == 4;
}

std::string warmupJobId(const char* scope) {
  static std::atomic<unsigned long long> counter{0};
  std::ostringstream id;
  id.imbue(std::locale::classic());
  id << "job-warmup-" << scope << "-" << counter.fetch_add(1);
  return id.str();
}

struct FrameBlob {
  Core::RectI bounds{};
  int channels = 0;
  std::vector<float> values;
};

class TempDir {
 public:
  explicit TempDir(std::filesystem::path path) : path_(std::move(path)) {}
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

bool validFrameBlobChannels(int channels) {
  return channels == 1 || channels == 3 || channels == 4;
}

bool expectedFrameBlobShape(const Core::RectI& bounds,
                            int channels,
                            const Core::RectI& expectedBounds,
                            int expectedChannels) {
  return bounds == expectedBounds && channels == expectedChannels;
}

std::size_t frameBlobValueCount(const Core::RectI& bounds, int channels) {
  return static_cast<std::size_t>(Core::width(bounds)) *
         static_cast<std::size_t>(Core::height(bounds)) *
         static_cast<std::size_t>(channels);
}

void writeU32(std::ostream& output, std::uint32_t value) {
  for (int byte = 0; byte < 4; ++byte) {
    output.put(static_cast<char>((value >> (byte * 8)) & 0xFFU));
  }
}

void writeI32(std::ostream& output, int value) {
  writeU32(output, static_cast<std::uint32_t>(value));
}

void writeF32(std::ostream& output, float value) {
  std::uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(value));
  writeU32(output, bits);
}

std::uint32_t readU32(std::istream& input) {
  std::uint32_t value = 0;
  for (int byte = 0; byte < 4; ++byte) {
    const int next = input.get();
    if (next == EOF) {
      throw std::runtime_error("frame blob is incomplete");
    }
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(next)) << (byte * 8);
  }
  return value;
}

int readI32(std::istream& input) {
  return static_cast<int>(readU32(input));
}

float readF32(std::istream& input) {
  const std::uint32_t bits = readU32(input);
  float value = 0.0F;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void writeFrameBlob(const std::filesystem::path& path,
                    const Core::RectI& bounds,
                    int channels,
                    const std::vector<float>& values,
                    const AbortPoller& abortPoller) {
  if (!validFrameBlobChannels(channels)) {
    throw std::runtime_error("frame blob channel count is invalid");
  }
  const auto expectedCount = frameBlobValueCount(bounds, channels);
  if (values.size() != expectedCount) {
    throw std::runtime_error("frame blob data size is invalid");
  }

  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("frame blob could not be opened");
  }
  abortPoller.throwIfRequested("frame blob write");
  output.write("CKFB", 4);
  writeU32(output, 1);
  writeI32(output, bounds.x1);
  writeI32(output, bounds.y1);
  writeI32(output, bounds.x2);
  writeI32(output, bounds.y2);
  writeU32(output, static_cast<std::uint32_t>(channels));
  for (const float value : values) {
    abortPoller.throwIfRequested("frame blob write");
    writeF32(output, value);
  }
  if (!output) {
    throw std::runtime_error("frame blob could not be written");
  }
}

FrameBlob readFrameBlob(const std::filesystem::path& path,
                        const Core::RectI& expectedBounds,
                        int expectedChannels,
                        const char* label,
                        const AbortPoller& abortPoller) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("frame blob could not be opened");
  }
  char magic[4] = {};
  input.read(magic, sizeof(magic));
  if (!input || std::string(magic, sizeof(magic)) != "CKFB") {
    throw std::runtime_error("frame blob header is invalid");
  }
  const std::uint32_t version = readU32(input);
  FrameBlob blob;
  blob.bounds = Core::RectI{readI32(input), readI32(input), readI32(input),
                            readI32(input)};
  blob.channels = static_cast<int>(readU32(input));
  if (version != 1 || Core::isEmpty(blob.bounds) ||
      !validFrameBlobChannels(blob.channels)) {
    throw std::runtime_error("frame blob shape is invalid");
  }
  if (!expectedFrameBlobShape(blob.bounds, blob.channels, expectedBounds,
                              expectedChannels)) {
    throw std::runtime_error(std::string{label} + " frame blob contract mismatch");
  }
  blob.values.resize(frameBlobValueCount(blob.bounds, blob.channels));
  for (float& value : blob.values) {
    abortPoller.throwIfRequested("output blob read");
    value = readF32(input);
  }
  char trailing = '\0';
  if (input.read(&trailing, 1)) {
    throw std::runtime_error("frame blob has trailing data");
  }
  if (!input.eof()) {
    throw std::runtime_error("frame blob could not be read");
  }
  return blob;
}

std::filesystem::path debugFrameBlobDir(const std::string& jobId,
                                        const std::string& frameId,
                                        int outputModeChoice) {
  const char* root = std::getenv("CORRIDORKEY_DEBUG_FRAME_BLOB_DIR");
  if (root == nullptr || root[0] == '\0') {
    return {};
  }
  std::ostringstream mode;
  mode.imbue(std::locale::classic());
  mode << "mode-" << outputModeChoice;
  return std::filesystem::path{root} / jobId / frameId / mode.str();
}

void copyDebugFrameBlob(const std::filesystem::path& source,
                        const std::filesystem::path& debugDir,
                        const char* name) {
  if (debugDir.empty() || name == nullptr || name[0] == '\0') {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(debugDir, ec);
  if (ec) {
    return;
  }
  std::filesystem::copy_file(source, debugDir / name,
                             std::filesystem::copy_options::overwrite_existing, ec);
}

bool isResponseFrameBlobPathInJob(const std::filesystem::path& path,
                                  const std::filesystem::path& jobRoot) {
  const std::filesystem::path normalizedPath = path.lexically_normal();
  const std::filesystem::path normalizedRoot = jobRoot.lexically_normal();
  if (!normalizedPath.is_absolute() || normalizedPath.extension() != ".ckfb") {
    return false;
  }

  auto rootIt = normalizedRoot.begin();
  auto pathIt = normalizedPath.begin();
  for (; rootIt != normalizedRoot.end(); ++rootIt, ++pathIt) {
    if (pathIt == normalizedPath.end() || *pathIt != *rootIt) {
      return false;
    }
  }
  return pathIt != normalizedPath.end();
}

void validateResponseFrameBlobPath(const char* pathText,
                                   const std::filesystem::path& jobRoot,
                                   const char* label) {
  if (pathText == nullptr || pathText[0] == '\0' ||
      !isResponseFrameBlobPathInJob(std::filesystem::path{pathText}, jobRoot)) {
    throw std::runtime_error(std::string{label} +
                             " frame blob path escaped the render job directory");
  }
}

float clampAlpha(float value) {
  return std::clamp(value, 0.0F, 1.0F);
}

std::size_t blobOffset(const FrameBlob& blob, int x, int y, int channel) {
  return (static_cast<std::size_t>(y - blob.bounds.y1) *
              static_cast<std::size_t>(Core::width(blob.bounds)) +
          static_cast<std::size_t>(x - blob.bounds.x1)) *
             static_cast<std::size_t>(blob.channels) +
         static_cast<std::size_t>(channel);
}

float blobValue(const FrameBlob& blob, int x, int y, int channel) {
  if (!Core::contains(blob.bounds, x, y)) {
    return 0.0F;
  }
  if (channel >= blob.channels) {
    return channel == 3 ? 1.0F : 0.0F;
  }
  return blob.values[blobOffset(blob, x, y, channel)];
}

template <typename PixelAt>
Core::FormatStatus writeWindowPixels(const Core::PixelBufferView& output,
                                     const Core::RectI& renderWindow,
                                     PixelAt pixelAt,
                                     const AbortPoller* abortPoller = nullptr) {
  Core::FormatStatus status = Core::outputFormatStatus(output.format);
  if (!Core::isSuccess(status)) {
    return status;
  }
  const Core::RectI writeWindow = Core::clipToBounds(renderWindow, output.bounds);
  for (int y = writeWindow.y1; y < writeWindow.y2; ++y) {
    if (abortPoller != nullptr) {
      abortPoller->throwIfRequested("output writeback");
    }
    for (int x = writeWindow.x1; x < writeWindow.x2; ++x) {
      const Core::FormatStatus writeStatus = Core::writePixel(output, x, y, pixelAt(x, y));
      if (!Core::isSuccess(writeStatus)) {
        return writeStatus;
      }
      if (writeStatus == Core::FormatStatus::ByteFallback) {
        status = writeStatus;
      }
    }
  }
  return status;
}

Core::FormatStatus copyWindowWithAbort(const Core::PixelBufferView& source,
                                       const Core::PixelBufferView& output,
                                       const Core::RectI& renderWindow,
                                       const AbortPoller& abortPoller) {
  Core::FormatStatus status = Core::inputFormatStatus(source.format);
  if (!Core::isSuccess(status)) {
    return status;
  }
  const Core::FormatStatus outputStatus = Core::outputFormatStatus(output.format);
  if (!Core::isSuccess(outputStatus)) {
    return outputStatus;
  }
  if (outputStatus == Core::FormatStatus::ByteFallback) {
    status = outputStatus;
  }

  const Core::RectI writeWindow = Core::clipToBounds(renderWindow, output.bounds);
  for (int y = writeWindow.y1; y < writeWindow.y2; ++y) {
    abortPoller.throwIfRequested("output writeback");
    for (int x = writeWindow.x1; x < writeWindow.x2; ++x) {
      Core::RgbaPixel pixel{};
      if (Core::contains(source.bounds, x, y)) {
        const Core::FormatStatus readStatus = Core::readPixel(source, x, y, pixel);
        if (!Core::isSuccess(readStatus)) {
          return readStatus;
        }
        if (readStatus == Core::FormatStatus::ByteFallback) {
          status = readStatus;
        }
      }
      const Core::FormatStatus writeStatus = Core::writePixel(output, x, y, pixel);
      if (!Core::isSuccess(writeStatus)) {
        return writeStatus;
      }
      if (writeStatus == Core::FormatStatus::ByteFallback) {
        status = writeStatus;
      }
    }
  }
  return status;
}

std::vector<float> sourceBlobValues(const Core::PixelBufferView& source,
                                    const Core::RectI& renderWindow,
                                    Core::InputColorMode inputColorMode,
                                    const AbortPoller& abortPoller) {
  std::vector<float> values;
  values.reserve(static_cast<std::size_t>(Core::width(renderWindow)) *
                 static_cast<std::size_t>(Core::height(renderWindow)) * 4U);
  for (int y = renderWindow.y1; y < renderWindow.y2; ++y) {
    abortPoller.throwIfRequested("frame blob write");
    for (int x = renderWindow.x1; x < renderWindow.x2; ++x) {
      Core::RgbaPixel pixel{};
      if (Core::contains(source.bounds, x, y)) {
        const Core::FormatStatus readStatus = Core::readPixel(source, x, y, pixel);
        if (!Core::isSuccess(readStatus)) {
          throw std::runtime_error("source frame blob read failed");
        }
        pixel = Core::convertInputToLinear(pixel, inputColorMode);
      }
      values.push_back(pixel.r);
      values.push_back(pixel.g);
      values.push_back(pixel.b);
      values.push_back(pixel.a);
    }
  }
  return values;
}

std::vector<float> alphaBlobValues(const Core::AlphaHintBuffer& alphaHint,
                                   const Core::RectI& renderWindow,
                                   const AbortPoller& abortPoller) {
  std::vector<float> values;
  values.reserve(static_cast<std::size_t>(Core::width(renderWindow)) *
                 static_cast<std::size_t>(Core::height(renderWindow)));
  for (int y = renderWindow.y1; y < renderWindow.y2; ++y) {
    abortPoller.throwIfRequested("frame blob write");
    for (int x = renderWindow.x1; x < renderWindow.x2; ++x) {
      values.push_back(Core::alphaHintValueAt(alphaHint, x, y));
    }
  }
  return values;
}

Core::FormatStatus writeBlobRgba(const Core::PixelBufferView& output,
                                 const Core::RectI& renderWindow,
                                 const FrameBlob& blob,
                                 const AbortPoller& abortPoller) {
  return writeWindowPixels(output, renderWindow, [&blob](int x, int y) {
    return Core::RgbaPixel{
        blobValue(blob, x, y, 0),
        blobValue(blob, x, y, 1),
        blobValue(blob, x, y, 2),
        blobValue(blob, x, y, 3),
    };
  }, &abortPoller);
}

Core::FormatStatus writeMatte(const Core::PixelBufferView& output,
                              const Core::RectI& renderWindow,
                              const FrameBlob& alpha,
                              const AbortPoller& abortPoller) {
  return writeWindowPixels(output, renderWindow, [&alpha](int x, int y) {
    const float value = clampAlpha(blobValue(alpha, x, y, 0));
    return Core::RgbaPixel{value, value, value, value};
  }, &abortPoller);
}

Core::FormatStatus writeStraightFg(const Core::PixelBufferView& output,
                                   const Core::RectI& renderWindow,
                                   const FrameBlob& straight,
                                   const FrameBlob& alpha,
                                   const AbortPoller& abortPoller) {
  return writeWindowPixels(output, renderWindow, [&straight, &alpha](int x, int y) {
    const float matte = clampAlpha(blobValue(alpha, x, y, 0));
    return Core::RgbaPixel{
        blobValue(straight, x, y, 0),
        blobValue(straight, x, y, 1),
        blobValue(straight, x, y, 2),
        matte,
    };
  }, &abortPoller);
}

Core::FormatStatus writeAlphaHintView(const Core::PixelBufferView& output,
                                      const Core::RectI& renderWindow,
                                      const Core::AlphaHintBuffer& alphaHint,
                                      const AbortPoller& abortPoller) {
  return writeWindowPixels(output, renderWindow, [&alphaHint](int x, int y) {
    const float value = clampAlpha(Core::alphaHintValueAt(alphaHint, x, y));
    return Core::RgbaPixel{value, value, value, value};
  }, &abortPoller);
}

float checkerValue(int x, int y) {
  return ((x + y) & 1) == 0 ? 0.78F : 0.36F;
}

Core::FormatStatus writeCheckerComp(const Core::PixelBufferView& output,
                                    const Core::RectI& renderWindow,
                                    const FrameBlob& processed,
                                    const FrameBlob& alpha,
                                    const AbortPoller& abortPoller) {
  return writeWindowPixels(output, renderWindow, [&processed, &alpha](int x, int y) {
    const float matte = clampAlpha(blobValue(alpha, x, y, 0));
    const float bg = checkerValue(x, y);
    return Core::RgbaPixel{
        blobValue(processed, x, y, 0) + bg * (1.0F - matte),
        blobValue(processed, x, y, 1) + bg * (1.0F - matte),
        blobValue(processed, x, y, 2) + bg * (1.0F - matte),
        1.0F,
    };
  }, &abortPoller);
}

Core::StatusFrameContent statusContentFromSurface(
    const CorridorKeyRuntimeStatus& surface) {
  const bool ok = surface.ok;
  return Core::StatusFrameContent{
      ok ? Core::StatusFrameSeverity::Status : Core::StatusFrameSeverity::Error,
      ok ? "CorridorKey Status" : "Sidecar Error",
      ok ? "Runtime ready"
         : (surface.errorCode[0] == '\0' ? "runtime unavailable" : surface.errorCode),
      statusLinesFromSurface(surface),
  };
}

OfxStatus renderStatusOutputFrame(const Core::PixelBufferView& output,
                                  const OfxRectI& renderWindow,
                                  const CorridorKeyRuntimeStatus& surface) {
  const Core::FormatStatus status = Core::renderStatusFrame(
      output,
      Core::RectI{renderWindow.x1, renderWindow.y1, renderWindow.x2, renderWindow.y2},
      statusContentFromSurface(surface));
  return ofxStatusForFormatStatus(status);
}

void copyWireString(char* destination, std::size_t destinationSize,
                    const std::string& value) {
  if (destinationSize == 0) {
    return;
  }
  const std::size_t count = std::min(destinationSize - 1, value.size());
  std::copy_n(value.data(), count, destination);
  destination[count] = '\0';
}

CorridorKeyRuntimeStatus inferFailureSurface(const std::string& message) {
  CorridorKeyRuntimeStatus surface{};
  copyWireString(surface.state, sizeof(surface.state), "error");
  copyWireString(surface.errorCode, sizeof(surface.errorCode), "infer_failed");
  copyWireString(surface.lastError, sizeof(surface.lastError), message);
  return surface;
}

CorridorKeyRuntimeStatus inferFailureSurface(const char* code,
                                             const std::string& message) {
  CorridorKeyRuntimeStatus surface{};
  copyWireString(surface.state, sizeof(surface.state), "error");
  copyWireString(surface.errorCode, sizeof(surface.errorCode),
                 code == nullptr || code[0] == '\0' ? "infer_failed" : code);
  copyWireString(surface.lastError, sizeof(surface.lastError), message);
  return surface;
}

CorridorKeyRuntimeStatus inferFailureSurface(const CorridorKeyRuntimeInfer& response,
                                             const std::string& message) {
  CorridorKeyRuntimeStatus surface =
      inferFailureSurface(response.errorCode, message);
  copyWireString(surface.checkpoint, sizeof(surface.checkpoint), response.checkpoint);
  copyWireString(surface.modelStatus, sizeof(surface.modelStatus), response.modelStatus);
  copyWireString(surface.modelSourceStatus, sizeof(surface.modelSourceStatus),
                 response.modelSourceStatus);
  copyWireString(surface.installStatus, sizeof(surface.installStatus),
                 response.installStatus);
  copyWireString(surface.backend, sizeof(surface.backend), response.backend);
  copyWireString(surface.backendStatus, sizeof(surface.backendStatus),
                 response.backendStatus);
  copyWireString(surface.cache, sizeof(surface.cache), response.cache);
  return surface;
}

CorridorKeyRuntimeStatus cancelledSurface(const std::string& message) {
  CorridorKeyRuntimeStatus surface{};
  copyWireString(surface.state, sizeof(surface.state), "cancelled");
  copyWireString(surface.errorCode, sizeof(surface.errorCode), "cancelled");
  copyWireString(surface.lastError, sizeof(surface.lastError), message);
  return surface;
}

void updateCancelledStatusParams(OfxImageEffectHandle instance, const std::string& message) {
  updateStatusParams(instance, cancelledSurface(message));
}

bool isTrueText(const char* value) {
  return value != nullptr && (std::string{value} == "true" || std::string{value} == "1");
}

std::string inferDiagnosticMessage(const CorridorKeyRuntimeInfer& response) {
  std::ostringstream message;
  message.imbue(std::locale::classic());
  if (response.lastError[0] != '\0') {
    message << response.lastError;
  } else {
    message << "infer failed";
  }
  if (response.effectiveQuality[0] != '\0') {
    message << "; effective quality " << response.effectiveQuality;
  }
  if (response.requestedQuality[0] != '\0' &&
      std::string{response.requestedQuality} != std::string{response.effectiveQuality}) {
    message << " from requested " << response.requestedQuality;
  }
  if (response.queueTimeMs[0] != '\0') {
    message << "; queue " << response.queueTimeMs << "ms";
  }
  if (isTrueText(response.oom)) {
    message << "; OOM true";
  }
  if (isTrueText(response.downgradedQuality)) {
    message << "; downgraded true";
  }
  if (isTrueText(response.finalFailure)) {
    message << "; final failure true";
  }
  return message.str();
}

void updateInferStatusParams(OfxImageEffectHandle instance,
                             const CorridorKeyRuntimeInfer& response) {
  setStatusParam(instance, "ck_backend_status",
                 backendStatusOrBackend(response.backendStatus, response.backend));
  setStatusParam(instance, "ck_checkpoint_version", response.checkpoint);
  setStatusParam(instance, "ck_model_status", response.modelStatus);
  setStatusParam(instance, "ck_model_source_status", response.modelSourceStatus);
  setStatusParam(instance, "ck_install_status", response.installStatus);
  setStatusParam(instance, "ck_cache", response.cache);
  setStatusParam(instance, "ck_effective_quality", response.effectiveQuality);
  if (isTrueText(response.oom) || isTrueText(response.downgradedQuality) ||
      isTrueText(response.finalFailure)) {
    setStatusParam(instance, "ck_last_error", inferDiagnosticMessage(response).c_str());
  } else if (response.ok) {
    setStatusParam(instance, "ck_last_error", "");
  }
}

const char* qualityTextFromChoice(int choice) {
  switch (choice) {
    case 0:
      return "draft_512";
    case 1:
      return "high_1024";
    case 2:
      return "full_2048";
    default:
      return "unknown";
  }
}

std::string jsonEscape(std::string_view text) {
  std::string escaped;
  for (const char c : text) {
    switch (c) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped.push_back(c >= 0x20 && c < 0x7F ? c : '?');
        break;
    }
  }
  return escaped;
}

std::filesystem::path inferDiagnosticLogPath() {
  const std::string configured = envValue("CORRIDORKEY_DIAGNOSTIC_LOG");
  if (!configured.empty()) {
    return configured;
  }
  const std::string home = envValue("HOME");
  if (!home.empty()) {
    return std::filesystem::path{home} / "Library" / "Logs" /
           "CorridorKey OFX" / "render-diagnostics.jsonl";
  }
  return std::filesystem::temp_directory_path() / "corridorkey-render-diagnostics.jsonl";
}

std::filesystem::path supportBundleOutputRoot() {
  const std::string configured =
      testOverridesAllowed() ? envValue("CORRIDORKEY_SUPPORT_BUNDLE_OUTPUT") : "";
  if (!configured.empty()) {
    return configured;
  }
  const std::string home = envValue("HOME");
  if (!home.empty()) {
    return std::filesystem::path{home} / "Library" / "Logs" /
           "CorridorKey OFX" / "support-bundles";
  }
  return std::filesystem::temp_directory_path() /
         "corridorkey-ofx-support-bundles";
}

long long fileSizeOrMinusOne(const std::filesystem::path& path) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec || size > static_cast<std::uintmax_t>(std::numeric_limits<long long>::max())) {
    return -1;
  }
  return static_cast<long long>(size);
}

void appendInferFailureDiagnostic(const std::string& jobId,
                                  const Core::RectI& window,
                                  int timeoutMs,
                                  long long elapsedMs,
                                  int qualityChoice,
                                  int backendChoice,
                                  int outputModeChoice,
                                  const std::filesystem::path& sourcePath,
                                  const std::filesystem::path& alphaPath,
                                  const CorridorKeyRuntimeInfer& response) {
  try {
    const std::filesystem::path logPath = inferDiagnosticLogPath();
    std::error_code ec;
    std::filesystem::create_directories(logPath.parent_path(), ec);
    std::ofstream log(logPath, std::ios::app);
    if (!log) {
      return;
    }
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    log << "{\"event\":\"infer_failure\""
        << ",\"time_unix_ms\":" << nowMs
        << ",\"job_id\":\"" << jsonEscape(jobId) << "\""
        << ",\"render_window\":{\"x1\":" << window.x1
        << ",\"y1\":" << window.y1
        << ",\"x2\":" << window.x2
        << ",\"y2\":" << window.y2
        << ",\"width\":" << Core::width(window)
        << ",\"height\":" << Core::height(window) << "}"
        << ",\"timeout_ms\":" << timeoutMs
        << ",\"elapsed_ms\":" << elapsedMs
        << ",\"quality\":\"" << qualityTextFromChoice(qualityChoice) << "\""
        << ",\"backend_choice\":" << backendChoice
        << ",\"output_mode\":" << outputModeChoice
        << ",\"source_blob_bytes\":" << fileSizeOrMinusOne(sourcePath)
        << ",\"alpha_blob_bytes\":" << fileSizeOrMinusOne(alphaPath)
        << ",\"ok\":" << (response.ok ? "true" : "false")
        << ",\"error_code\":\"" << jsonEscape(response.errorCode) << "\""
        << ",\"last_error\":\"" << jsonEscape(response.lastError) << "\""
        << ",\"backend\":\"" << jsonEscape(response.backend) << "\""
        << ",\"backend_status\":\"" << jsonEscape(response.backendStatus) << "\""
        << ",\"model_status\":\"" << jsonEscape(response.modelStatus) << "\""
        << ",\"model_source_status\":\"" << jsonEscape(response.modelSourceStatus) << "\""
        << ",\"install_status\":\"" << jsonEscape(response.installStatus) << "\""
        << ",\"requested_quality\":\"" << jsonEscape(response.requestedQuality) << "\""
        << ",\"effective_quality\":\"" << jsonEscape(response.effectiveQuality) << "\""
        << ",\"queue_time_ms\":\"" << jsonEscape(response.queueTimeMs) << "\""
        << "}\n";
  } catch (...) {
  }
}

OfxStatus renderInferOutputMode(OfxImageEffectHandle instance,
                                const Core::PixelBufferView& source,
                                const Core::PixelBufferView& output,
                                const Core::AlphaHintBuffer& alphaHint,
                                OfxTime time,
                                const OfxRectI& renderWindow,
                                int alphaHintChoice,
                                int outputModeChoice) {
  const std::string bundleRoot = statusProbeBundleRoot();
  if (bundleRoot.empty()) {
    return renderFailureStatusFrame(
        output, renderWindow,
        inferFailureSurface("runtime bundle root could not be resolved"));
  }

  const std::string frameId = inferFrameId(time);
  const std::string jobId = jobIdForFrame(frameId);
  const AbortPoller abortPoller(instance);
  AbortCallbackContext abortContext{instance};
  const Core::RectI window{renderWindow.x1, renderWindow.y1, renderWindow.x2,
                           renderWindow.y2};
  const TempDir tempDir(renderTempRoot(frameId));
  const std::filesystem::path debugDir =
      debugFrameBlobDir(jobId, frameId, outputModeChoice);
  const std::filesystem::path sourcePath = tempDir.path() / "source.ckfb";
  const std::filesystem::path alphaPath = tempDir.path() / "alpha_hint.ckfb";

  try {
    abortPoller.throwIfRequested("frame blob write");
    std::filesystem::create_directories(tempDir.path());
    const int inputColorSpaceChoice =
        choiceParamAtTime(instance, "ck_input_color_space", time, 0);
    writeFrameBlob(sourcePath, window, 4,
                   sourceBlobValues(source, window,
                                    inputColorModeFromChoice(inputColorSpaceChoice),
                                    abortPoller),
                   abortPoller);
    writeFrameBlob(alphaPath, window, 1,
                   alphaBlobValues(alphaHint, window, abortPoller), abortPoller);
    copyDebugFrameBlob(sourcePath, debugDir, "source.ckfb");
    copyDebugFrameBlob(alphaPath, debugDir, "alpha_hint.ckfb");
    abortPoller.throwIfRequested("runtime wait");

    CorridorKeyRuntimeInfer response{};
    const std::string runtimePath = statusProbeRuntimePath();
    const int qualityChoice = choiceParamAtTime(instance, "ck_quality", time, 1);
    const int backendChoice = choiceParamAtTime(instance, "ck_backend", time, 0);
    const int timeoutMs = runtimeInferTimeoutMs(window);
    const auto inferStart = std::chrono::steady_clock::now();
    const int inferOk = CorridorKeyRunStubInfer(
        bundleRoot.c_str(), runtimePath.c_str(), timeoutMs, jobId.c_str(),
        frameId.c_str(), renderWindow.x1, renderWindow.y1, renderWindow.x2,
        renderWindow.y2, sourcePath.string().c_str(), alphaPath.string().c_str(),
        alphaHintChoice, choiceParamAtTime(instance, "ck_screen_color", time, 0),
        qualityChoice, inputColorSpaceChoice,
        doubleParamAtTime(instance, "ck_despill_strength", time, 5.0),
        backendChoice, outputModeChoice, cancelRequestedFromHost, &abortContext,
        &response, sizeof(response));
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - inferStart)
                               .count();
    if (response.requestedQuality[0] == '\0') {
      copyWireString(response.requestedQuality, sizeof(response.requestedQuality),
                     qualityTextFromChoice(qualityChoice));
    }
    if (response.effectiveQuality[0] == '\0') {
      copyWireString(response.effectiveQuality, sizeof(response.effectiveQuality),
                     qualityTextFromChoice(qualityChoice));
    }
    if (inferOk != 1 || !response.ok) {
      appendInferFailureDiagnostic(jobId, window, timeoutMs, elapsedMs, qualityChoice,
                                   backendChoice, outputModeChoice, sourcePath, alphaPath,
                                   response);
      if (std::string{response.errorCode} == "cancelled") {
        return updateAndRenderFailureStatusFrame(instance, output, renderWindow,
                                                 cancelledSurface(response.lastError));
      }
      updateInferStatusParams(instance, response);
      return updateAndRenderFailureStatusFrame(instance, output, renderWindow,
                                               inferFailureSurface(
                                                   response,
                                                   inferDiagnosticMessage(response)));
    }
    updateInferStatusParams(instance, response);
    abortPoller.throwIfRequested("output writeback");
    FrameBlob processed;
    FrameBlob straight;
    FrameBlob alpha;
    if (outputModeNeedsProcessed(outputModeChoice)) {
      validateResponseFrameBlobPath(response.processedPath, tempDir.path(),
                                    "processed RGBA");
      copyDebugFrameBlob(response.processedPath, debugDir, "processed_rgba.ckfb");
      processed = readFrameBlob(response.processedPath, window, 4,
                                "processed RGBA", abortPoller);
    }
    if (outputModeNeedsStraight(outputModeChoice)) {
      validateResponseFrameBlobPath(response.straightPath, tempDir.path(),
                                    "straight FG");
      copyDebugFrameBlob(response.straightPath, debugDir, "straight_fg.ckfb");
      straight = readFrameBlob(response.straightPath, window, 3, "straight FG",
                               abortPoller);
    }
    if (outputModeNeedsAlpha(outputModeChoice)) {
      validateResponseFrameBlobPath(response.alphaPath, tempDir.path(), "alpha");
      copyDebugFrameBlob(response.alphaPath, debugDir, "alpha.ckfb");
      alpha = readFrameBlob(response.alphaPath, window, 1, "alpha",
                            abortPoller);
    }

    Core::FormatStatus writeStatus = Core::FormatStatus::Ok;
    switch (outputModeChoice) {
      case 0:
        writeStatus = writeBlobRgba(output, window, processed, abortPoller);
        break;
      case 1:
        writeStatus = writeMatte(output, window, alpha, abortPoller);
        break;
      case 2:
        writeStatus = writeStraightFg(output, window, straight, alpha, abortPoller);
        break;
      case 4:
        writeStatus = writeCheckerComp(output, window, processed, alpha, abortPoller);
        break;
      default:
        writeStatus = Core::FormatStatus::UnsupportedComponents;
        break;
    }
    return ofxStatusForFormatStatus(writeStatus);
  } catch (const RenderCancelled& exc) {
    return updateAndRenderFailureStatusFrame(instance, output, renderWindow,
                                             cancelledSurface(exc.what()));
  } catch (const std::exception& exc) {
    return updateAndRenderFailureStatusFrame(instance, output, renderWindow,
                                             inferFailureSurface(exc.what()));
  }
}

OfxStatus runWarmup(OfxImageEffectHandle instance, const char* scope) {
  if (instance == nullptr) {
    return kOfxStatErrBadHandle;
  }
  if (hostAbortRequested(instance)) {
    updateCancelledStatusParams(instance, "cancelled before warmup");
    return kOfxStatOK;
  }

  setStatusParam(instance, "ck_warmup", "warming");
  setStatusParam(instance, "ck_last_error", "");

  const std::string bundleRoot = statusProbeBundleRoot();
  if (bundleRoot.empty()) {
    setStatusParam(instance, "ck_warmup", "unavailable");
    setStatusParam(instance, "ck_last_error", "runtime bundle root could not be resolved");
    return kOfxStatOK;
  }

  const int backendChoice = choiceParamAtTime(instance, "ck_backend", 0.0, 0);
  const int qualityChoice = choiceParamAtTime(instance, "ck_quality", 0.0, 1);
  const std::string runtimePath = statusProbeRuntimePath();
  const std::string jobId = warmupJobId(scope);
  AbortCallbackContext abortContext{instance};
  CorridorKeyRuntimeWarmup response{};
  const int warmupOk = CorridorKeyRunWarmup(
      bundleRoot.c_str(), runtimePath.c_str(), kRuntimeWarmupTimeoutMs, jobId.c_str(),
      backendChoice, qualityChoice, cancelRequestedFromHost, &abortContext,
      &response, sizeof(response));
  if (hostAbortRequested(instance)) {
    updateCancelledStatusParams(instance, "cancelled during warmup");
    return kOfxStatOK;
  }

  if (warmupOk == 1 && response.ok) {
    setStatusParam(instance, "ck_runtime_state", "ready");
    setStatusParam(instance, "ck_backend_status",
                   backendStatusOrBackend(response.backendStatus, response.backend));
    setStatusParam(instance, "ck_model_status", response.modelStatus);
    setStatusParam(instance, "ck_model_source_status", response.modelSourceStatus);
    setStatusParam(instance, "ck_install_status", response.installStatus);
    setStatusParam(instance, "ck_warmup", response.warmup);
    setStatusParam(instance, "ck_last_error", "");
    return kOfxStatOK;
  }

  const std::string code = response.errorCode[0] == '\0' ? "warmup_failed" : response.errorCode;
  const std::string message =
      response.lastError[0] == '\0' ? "warmup failed" : response.lastError;
  setStatusParam(instance, "ck_runtime_state",
                 code == "cancelled" ? "cancelled" : "error");
  setStatusParam(instance, "ck_backend_status",
                 backendStatusOrBackend(response.backendStatus, response.backend));
  setStatusParam(instance, "ck_model_status", response.modelStatus);
  setStatusParam(instance, "ck_model_source_status", response.modelSourceStatus);
  setStatusParam(instance, "ck_install_status", response.installStatus);
  setStatusParam(instance, "ck_warmup", code == "cancelled" ? "cancelled" : "failed");
  setStatusParam(instance, "ck_last_error", message.c_str());
  return kOfxStatOK;
}

OfxStatus runDownloadMissingModels(OfxImageEffectHandle instance) {
  if (instance == nullptr) {
    return kOfxStatErrBadHandle;
  }

  setStatusParam(instance, "ck_runtime_state", "blocked");
  setStatusParam(instance, "ck_model_status", "missing");
  setStatusParam(instance, "ck_model_source_status", "ready");
  setStatusParam(instance, "ck_install_status", "not_installed");
  setStatusParam(instance, "ck_download_status", "not_configured");
  setStatusParam(instance, "ck_download_progress", "0/0");
  setStatusParam(
      instance, "ck_last_error",
      "configure a local model package or manifest URL before running model download");
  return kOfxStatOK;
}

std::filesystem::path sourceRepoRoot() {
#if defined(CORRIDORKEY_SOURCE_DIR)
  return std::filesystem::path{CORRIDORKEY_SOURCE_DIR};
#else
  return {};
#endif
}

std::filesystem::path doctorScriptPath() {
  const std::string configured = envValue("CORRIDORKEY_DOCTOR_SCRIPT");
  if (!configured.empty()) {
    return std::filesystem::path{configured};
  }

  const std::filesystem::path sourceRoot = sourceRepoRoot();
  if (!sourceRoot.empty()) {
    return sourceRoot / "scripts" / "doctor_dev_env.py";
  }

  const std::string bundleRoot = statusProbeBundleRoot();
  if (!bundleRoot.empty()) {
    return std::filesystem::path{bundleRoot} / "Contents" / "Resources" /
           "scripts" / "doctor_dev_env.py";
  }
  return {};
}

std::string shellQuote(const std::filesystem::path& path) {
  std::string value = path.string();
#if defined(_WIN32)
  std::string quoted = "\"";
  for (const char ch : value) {
    if (ch == '"') {
      quoted += "\\\"";
    } else {
      quoted += ch;
    }
  }
  quoted += "\"";
  return quoted;
#else
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
#endif
}

std::filesystem::path supportBundlePythonPath(
    const std::filesystem::path& bundleRoot) {
  const std::filesystem::path bundledPython = bundledPythonLauncherPath(bundleRoot);
  std::error_code ec;
  if (std::filesystem::is_regular_file(bundledPython, ec)) {
    return bundledPython;
  }
  return std::filesystem::path{statusProbeRuntimePath()};
}

std::string supportBundleCommand(const std::filesystem::path& resources,
                                 const std::filesystem::path& python,
                                 const std::filesystem::path& outputRoot,
                                 const std::filesystem::path& diagnosticLog,
                                 const std::filesystem::path& reportPath) {
#if defined(_WIN32)
  return "cd /d " + shellQuote(resources) +
         " && set \"PYTHONHOME=\""
         " && set \"PYTHONUSERBASE=\""
         " && set \"VIRTUAL_ENV=\""
         " && set \"CONDA_PREFIX=\""
         " && set \"CONDA_DEFAULT_ENV=\""
         " && set \"PYTHONPATH=" + resources.string() + "\""
         " && set \"PYTHONNOUSERSITE=1\""
         " && set \"PYTHONDONTWRITEBYTECODE=1\""
         " && " + shellQuote(python) +
         " -m sidecar.corridorkey_sidecar.support_bundle --output " +
         shellQuote(outputRoot) + " --log " + shellQuote(diagnosticLog) +
         " > " + shellQuote(reportPath) + " 2>&1";
#else
  return "cd " + shellQuote(resources) +
         " && /usr/bin/env"
         " -u PYTHONHOME"
         " -u PYTHONUSERBASE"
         " -u VIRTUAL_ENV"
         " -u CONDA_PREFIX"
         " -u CONDA_DEFAULT_ENV"
         " PYTHONPATH=" + shellQuote(resources) +
         " PYTHONNOUSERSITE=1"
         " PYTHONDONTWRITEBYTECODE=1 " +
         shellQuote(python) +
         " -m sidecar.corridorkey_sidecar.support_bundle --output " +
         shellQuote(outputRoot) + " --log " + shellQuote(diagnosticLog) +
         " > " + shellQuote(reportPath) + " 2>&1";
#endif
}

OfxStatus runDoctor(OfxImageEffectHandle instance) {
  if (instance == nullptr) {
    return kOfxStatErrBadHandle;
  }

  setStatusParam(instance, "ck_doctor_status", "running");
  setStatusParam(instance, "ck_last_error", "");

  const std::filesystem::path script = doctorScriptPath();
  std::error_code ec;
  if (script.empty() || !std::filesystem::is_regular_file(script, ec)) {
    setStatusParam(instance, "ck_doctor_status", "unavailable");
    setStatusParam(
        instance, "ck_last_error",
        "Run Doctor script is unavailable; see docs/diagnostics.md for manual fallback");
    return kOfxStatOK;
  }

  const std::filesystem::path reportRoot =
      std::filesystem::temp_directory_path() / "corridorkey-ofx-diagnostics";
  std::filesystem::create_directories(reportRoot, ec);
  if (ec) {
    setStatusParam(instance, "ck_doctor_status", "failed");
    setStatusParam(instance, "ck_last_error",
                   "doctor report directory could not be created");
    return kOfxStatOK;
  }

  const std::filesystem::path reportPath = reportRoot / "doctor-last.txt";
  const std::filesystem::path runtimePath = statusProbeRuntimePath();
  const std::string command = shellQuote(runtimePath) + " " + shellQuote(script) +
                              " > " + shellQuote(reportPath) + " 2>&1";
  const int exitCode = std::system(command.c_str());
  if (exitCode == 0) {
    setStatusParam(instance, "ck_doctor_status", "completed");
    setStatusParam(instance, "ck_last_error", "");
  } else {
    setStatusParam(instance, "ck_doctor_status", "failed");
    setStatusParam(instance, "ck_last_error",
                   "doctor failed; see local redacted doctor report");
  }
  return kOfxStatOK;
}

OfxStatus openLogFolderFallback(OfxImageEffectHandle instance) {
  if (instance == nullptr) {
    return kOfxStatErrBadHandle;
  }

  setStatusParam(instance, "ck_log_folder_status", "manual fallback");
  setStatusParam(instance, "ck_last_error",
                 "Open Log Folder is host-disabled; see docs/diagnostics.md");
  return kOfxStatOK;
}

OfxStatus copySupportBundle(OfxImageEffectHandle instance) {
  if (instance == nullptr) {
    return kOfxStatErrBadHandle;
  }

  setStatusParam(instance, "ck_support_bundle_status", "running");
  setStatusParam(instance, "ck_last_error", "");

  const std::string bundleRootText = statusProbeBundleRoot();
  if (bundleRootText.empty()) {
    setStatusParam(instance, "ck_support_bundle_status", "unavailable");
    setStatusParam(instance, "ck_last_error",
                   "support bundle runtime could not be resolved");
    return kOfxStatOK;
  }

  const std::filesystem::path bundleRoot{bundleRootText};
  const std::filesystem::path resources = bundleResourcesRoot(bundleRoot);
  std::error_code ec;
  if (!std::filesystem::is_directory(resources, ec)) {
    setStatusParam(instance, "ck_support_bundle_status", "unavailable");
    setStatusParam(instance, "ck_last_error",
                   "support bundle resources could not be resolved");
    return kOfxStatOK;
  }

  const std::filesystem::path outputRoot = supportBundleOutputRoot();
  std::filesystem::create_directories(outputRoot, ec);
  if (ec) {
    setStatusParam(instance, "ck_support_bundle_status", "failed");
    setStatusParam(instance, "ck_last_error",
                   "support bundle directory could not be created");
    return kOfxStatOK;
  }

  const std::filesystem::path reportPath = outputRoot / "support-bundle-last.txt";
  const std::filesystem::path python = supportBundlePythonPath(bundleRoot);
  const std::filesystem::path diagnosticLog = inferDiagnosticLogPath();
  const std::string command =
      supportBundleCommand(resources, python, outputRoot, diagnosticLog, reportPath);
  const int exitCode = std::system(command.c_str());
  if (exitCode == 0) {
    setStatusParam(instance, "ck_support_bundle_status", "completed");
    setStatusParam(instance, "ck_last_error", "");
  } else {
    setStatusParam(instance, "ck_support_bundle_status", "failed");
    setStatusParam(instance, "ck_last_error",
                   "support bundle failed; see local redacted support bundle log");
  }
  return kOfxStatOK;
}

}  // namespace

OfxStatus getClipPreferences(OfxImageEffectHandle instance,
                             OfxPropertySetHandle inArgs,
                             OfxPropertySetHandle outArgs) {
  if (propertySuite() == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }
  if (outArgs == nullptr) {
    return kOfxStatErrBadHandle;
  }
  OfxTime time = 0.0;
  if (inArgs != nullptr && propertySuite()->propGetDouble != nullptr) {
    (void)propertySuite()->propGetDouble(inArgs, kOfxPropTime, 0, &time);
  }
  const int outputModeChoice = choiceParamAtTime(instance, "ck_output_mode", time, 0);
  propertySuite()->propSetString(outArgs, kOfxImageEffectPropPreMultiplication, 0,
                                 premultiplicationForOutputMode(outputModeChoice));
  return kOfxStatOK;
}

OfxStatus render(OfxImageEffectHandle instance,
                 OfxPropertySetHandle inArgs,
                 OfxPropertySetHandle /*outArgs*/) {
  if (effectSuite() == nullptr || propertySuite() == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }

  OfxTime time = 0.0;
  OfxRectI renderWindow{};
  OfxStatus status = propertySuite()->propGetDouble(inArgs, kOfxPropTime, 0, &time);
  if (status != kOfxStatOK) {
    return status;
  }
  status = propertySuite()->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4,
                                        &renderWindow.x1);
  if (status != kOfxStatOK) {
    return status;
  }
  char* fieldToRender = nullptr;
  if (getStringProperty(inArgs, kOfxImageEffectPropFieldToRender, fieldToRender)) {
    const Core::FormatStatus fieldStatus =
        Core::renderFieldingStatus(Core::parseFielding(fieldToRender));
    if (!Core::isSuccess(fieldStatus)) {
      return ofxStatusForFormatStatus(fieldStatus);
    }
  }
  if (Core::isEmpty(Core::RectI{renderWindow.x1, renderWindow.y1, renderWindow.x2,
                                renderWindow.y2})) {
    return kOfxStatOK;
  }

  OfxImageClipHandle outputClip = nullptr;
  status = effectSuite()->clipGetHandle(instance, kOfxImageEffectOutputClipName, &outputClip,
                                        nullptr);
  if (status != kOfxStatOK) {
    return status;
  }
  OfxImageClipHandle sourceClip = nullptr;
  status = effectSuite()->clipGetHandle(instance, kOfxImageEffectSimpleSourceClipName,
                                        &sourceClip, nullptr);
  if (status != kOfxStatOK) {
    return status;
  }

  OfxPropertySetHandle outputImage = nullptr;
  OfxPropertySetHandle sourceImage = nullptr;
  status = effectSuite()->clipGetImage(outputClip, time, nullptr, &outputImage);
  if (status != kOfxStatOK) {
    return status;
  }
  ScopedImage outputImageGuard(outputImage);
  const OfxStatus sourceStatus =
      effectSuite()->clipGetImage(sourceClip, time, nullptr, &sourceImage);
  ScopedImage sourceImageGuard(sourceStatus == kOfxStatOK ? sourceImage : nullptr);
  Core::PixelBufferView output{};
  status = imageView(outputImage, true, output);
  if (status != kOfxStatOK) {
    return status;
  }

  const bool hasOutputMode = choiceParamDefined(instance, "ck_output_mode");
  const int outputModeChoice =
      hasOutputMode ? choiceParamAtTime(instance, "ck_output_mode", time, 0) : 0;

  if (hasOutputMode && outputModeChoice == 5) {
    const std::string statusBundleRoot = statusProbeBundleRoot();
    if (statusBundleRoot.empty()) {
      return renderFailureStatusFrame(
          output, renderWindow,
          inferFailureSurface("runtime bundle root could not be resolved"));
    }
    CorridorKeyRuntimeStatus surface{};
    const std::string runtimePath = statusProbeRuntimePath();
    (void)CorridorKeyProbeRuntimeStatus(statusBundleRoot.c_str(),
                                        runtimePath.c_str(), kRuntimeStatusProbeTimeoutMs,
                                        &surface, sizeof(surface));
    updateStatusParams(instance, surface);
    return renderStatusOutputFrame(output, renderWindow, surface);
  }

  const std::string bundleRoot = hasOutputMode ? std::string{} : statusProbeBundleRoot();
  if (!bundleRoot.empty()) {
    const std::string runtimePath = statusProbeRuntimePath();
    CorridorKeyRuntimeStatus surface{};
    (void)CorridorKeyProbeRuntimeStatus(bundleRoot.c_str(), runtimePath.c_str(),
                                        kRuntimeStatusProbeTimeoutMs, &surface,
                                        sizeof(surface));
    if (!surface.ok) {
      return renderFailureStatusFrame(output, renderWindow, surface);
    }
  }

  if (sourceStatus == kOfxStatFailed) {
    const Core::FormatStatus fillStatus = Core::fillWindowTransparent(
        output, Core::RectI{renderWindow.x1, renderWindow.y1, renderWindow.x2,
                            renderWindow.y2});
    return ofxStatusForFormatStatus(fillStatus);
  }
  if (sourceStatus != kOfxStatOK) {
    return sourceStatus;
  }

  Core::PixelBufferView source{};
  if (status == kOfxStatOK) {
    status = imageView(sourceImage, false, source);
  }
  if (status == kOfxStatOK) {
    OfxImageClipHandle alphaHintClip = nullptr;
    OfxPropertySetHandle alphaHintImage = nullptr;
    ScopedImage alphaImageGuard;
    Core::PixelBufferView alphaHint{};
    Core::PixelBufferView invalidAlphaHint{};
    const Core::PixelBufferView* alphaHintView = nullptr;
    const OfxStatus alphaClipStatus =
        effectSuite()->clipGetHandle(instance, kAlphaHintClipName, &alphaHintClip, nullptr);
    if (alphaClipStatus == kOfxStatOK) {
      const OfxStatus alphaImageStatus =
          effectSuite()->clipGetImage(alphaHintClip, time, nullptr, &alphaHintImage);
      if (alphaImageStatus == kOfxStatOK) {
        alphaImageGuard.reset(alphaHintImage);
        const OfxStatus alphaViewStatus =
            imageView(alphaHintImage, false, alphaHint, true);
        alphaHintView = alphaViewStatus == kOfxStatOK ? &alphaHint : &invalidAlphaHint;
      }
    }

    const int alphaHintChoice = alphaHintChoiceAtTime(instance, time);
    const Core::AlphaHintResult alphaHintResult = Core::resolveAlphaHint(
        alphaHintSourceFromChoice(alphaHintChoice), source, alphaHintView,
        Core::RectI{renderWindow.x1, renderWindow.y1, renderWindow.x2, renderWindow.y2});
    setStatusParam(instance, "ck_guide_source_status",
                   Core::alphaHintStatusLabel(alphaHintResult.status));
    const Core::RectI window{renderWindow.x1, renderWindow.y1, renderWindow.x2,
                             renderWindow.y2};
    try {
      const AbortPoller abortPoller(instance);
      if (!hasOutputMode) {
        const Core::FormatStatus copyStatus =
            copyWindowWithAbort(source, output, window, abortPoller);
        status = ofxStatusForFormatStatus(copyStatus);
      } else if (outputModeChoice == 3) {
        status = ofxStatusForFormatStatus(
            writeAlphaHintView(output, window, alphaHintResult.hint, abortPoller));
      } else {
        status = renderInferOutputMode(instance, source, output, alphaHintResult.hint,
                                       time, renderWindow, alphaHintChoice,
                                       outputModeChoice);
      }
    } catch (const RenderCancelled& exc) {
      status = renderFailureStatusFrame(output, renderWindow, cancelledSurface(exc.what()));
    }
  }

  return status;
}

OfxStatus beginSequenceRender(OfxImageEffectHandle instance,
                              OfxPropertySetHandle /*inArgs*/,
                              OfxPropertySetHandle /*outArgs*/) {
  if (effectSuite() == nullptr || propertySuite() == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }
  return instance == nullptr ? kOfxStatErrBadHandle : kOfxStatOK;
}

OfxStatus endSequenceRender(OfxImageEffectHandle instance,
                            OfxPropertySetHandle /*inArgs*/,
                            OfxPropertySetHandle /*outArgs*/) {
  if (instance == nullptr) {
    return kOfxStatErrBadHandle;
  }
  return kOfxStatOK;
}

OfxStatus instanceChanged(OfxImageEffectHandle instance,
                          OfxPropertySetHandle inArgs,
                          OfxPropertySetHandle /*outArgs*/) {
  if (effectSuite() == nullptr || propertySuite() == nullptr) {
    return kOfxStatErrMissingHostFeature;
  }
  char* changedName = nullptr;
  if (inArgs == nullptr ||
      !getStringProperty(inArgs, kOfxPropName, changedName) ||
      changedName == nullptr) {
    return kOfxStatReplyDefault;
  }
  const std::string name{changedName};
  if (name == "ck_prepare_warmup") {
    return runWarmup(instance, "button");
  }
  if (name == "ck_download_missing_models") {
    return runDownloadMissingModels(instance);
  }
  if (name == "ck_run_doctor") {
    return runDoctor(instance);
  }
  if (name == "ck_open_log_folder") {
    return openLogFolderFallback(instance);
  }
  if (name == "ck_copy_support_bundle") {
    return copySupportBundle(instance);
  }
  return kOfxStatReplyDefault;
}

}  // namespace CorridorKey::OFX
