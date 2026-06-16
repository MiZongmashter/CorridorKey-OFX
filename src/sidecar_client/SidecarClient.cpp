#include "sidecar_client/SidecarClient.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <regex>
#include <sstream>

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

struct RuntimeWarmupWire {
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

namespace CorridorKey::Sidecar {
namespace {

constexpr std::chrono::milliseconds kAbortPollInterval{50};
constexpr std::chrono::milliseconds kCancelGracePeriod{250};

bool testStubInferForced() {
  const char* faultsAllowed = std::getenv("CORRIDORKEY_TEST_FAULTS_ALLOWED");
  const char* forceStub = std::getenv("CORRIDORKEY_TEST_FORCE_STUB_INFER");
  return faultsAllowed != nullptr && std::string{faultsAllowed} == "1" &&
         forceStub != nullptr && std::string{forceStub} == "1";
}

struct SidecarStatusSurface {
  bool ok = false;
  bool restarted = false;
  std::string state = "error";
  std::string backend;
  std::string backendStatus;
  std::string modelVersion;
  std::string modelStatus;
  std::string modelSourceStatus;
  std::string installStatus;
  std::string downloadStatus;
  std::string downloadProgress;
  std::string gpu;
  std::string vram;
  std::string cache;
  std::string warmup;
  std::string lastError;
  std::string errorCode;
};

bool containsText(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

std::string toLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

std::string redactForStatus(std::string text) {
  text = std::regex_replace(text, std::regex(R"([A-Za-z]:\\[^"'\r\n]+)"),
                            "<redacted-path>");
  text = std::regex_replace(text, std::regex(R"([A-Za-z]:/[^"'\r\n]+)"),
                            "<redacted-path>");
  text = std::regex_replace(text, std::regex(R"(/[^"'\r\n]+)"), "<redacted-path>");
  text = std::regex_replace(
      text,
      std::regex(R"((^|[^\w.-])[\w .-]+\.(ari|braw|cin|dng|dpx|exr|mov|mp4|mxf|npy|png|r3d|tif|tiff)([^\w.-]|$))",
                 std::regex_constants::icase),
      "$1<redacted-file>$3");
  return text;
}

std::string classifyLaunchFailure(const std::string& message) {
  const std::string lower = toLower(message);
  if (containsText(lower, "timed out")) {
    return "health_timeout";
  }
  if (containsText(lower, "not running") || containsText(lower, "stdout closed") ||
      containsText(lower, "stdin is closed") ||
      containsText(lower, "closed before a response") ||
      containsText(lower, "non-zero status")) {
    return "process_stopped";
  }
  return "request_failed";
}

std::string classifyInferException(const std::string& message) {
  const std::string lower = toLower(message);
  if (containsText(lower, "timed out")) {
    return "infer_timeout";
  }
  return "infer_failed";
}

SidecarStatusSurface failureSurface(const std::string& code,
                                    const std::string& message) {
  SidecarStatusSurface result;
  result.ok = false;
  result.state = "error";
  result.errorCode = code;
  result.lastError = redactForStatus(message);
  return result;
}

SidecarStatusSurface successSurface(const SidecarResponse& response) {
  SidecarStatusSurface result;
  result.ok = true;
  result.state = response.payloadValue("state");
  if (result.state.empty()) {
    result.state = "ready";
  }
  result.backend = response.payloadValue("backend");
  result.backendStatus = response.payloadValue("backend_status");
  if (result.backendStatus.empty()) {
    result.backendStatus = result.backend;
  }
  result.modelVersion = response.payloadValue("model_version");
  if (result.modelVersion.empty()) {
    result.modelVersion = response.payloadValue("model");
  }
  result.modelStatus = response.payloadValue("model_status");
  result.modelSourceStatus = response.payloadValue("model_source_status");
  result.installStatus = response.payloadValue("install_status");
  result.downloadStatus = response.payloadValue("download_status");
  result.downloadProgress = response.payloadValue("download_progress");
  result.gpu = response.payloadValue("gpu");
  result.vram = response.payloadValue("vram");
  result.cache = response.payloadValue("cache");
  result.warmup = response.payloadValue("warmup");
  result.lastError = redactForStatus(response.payloadValue("last_error"));
  return result;
}

void throwIfCommandFailed(const SidecarResponse& response);

SidecarStatusSurface probeStatusOnce(const SidecarLaunchOptions& options) {
  std::unique_ptr<SidecarClient> client;
  try {
    client = std::make_unique<SidecarClient>(SidecarClient::launch(options));
  } catch (const LaunchError& exc) {
    return failureSurface("launch_failed", exc.what());
  }

  try {
    const SidecarResponse health = client->health();
    throwIfCommandFailed(health);
    const SidecarResponse status = client->status();
    throwIfCommandFailed(status);
    SidecarStatusSurface surface = successSurface(status);
    try {
      client->shutdown();
    } catch (...) {
    }
    return surface;
  } catch (const ProtocolError& exc) {
    return failureSurface("protocol_error", std::string("protocol error: ") + exc.what());
  } catch (const LaunchError& exc) {
    return failureSurface(classifyLaunchFailure(exc.what()), exc.what());
  }
}

SidecarStatusSurface probeStatusWithRecovery(const SidecarLaunchOptions& options) {
  SidecarStatusSurface first = probeStatusOnce(options);
  if (first.ok) {
    return first;
  }

  if (first.errorCode != "process_stopped") {
    return first;
  }

  SidecarStatusSurface second = probeStatusOnce(options);
  second.restarted = true;
  if (!second.ok && second.lastError.empty()) {
    second.lastError = first.lastError;
  }
  return second;
}

SidecarResponse cancelledResponse(const std::string& requestId, const std::string& jobId) {
  SidecarResponse response;
  response.requestId = requestId;
  response.ok = false;
  response.payload = {{"job_id", jobId}, {"cancelled", "true"}};
  response.error = SidecarError{"cancelled", "Sidecar job was cancelled"};
  return response;
}

void throwIfCommandFailed(const SidecarResponse& response) {
  if (response.ok) {
    return;
  }
  const std::string code = response.error ? response.error->code : "unknown";
  throw ProtocolError("sidecar command failed: " + code);
}

}  // namespace

SidecarClient SidecarClient::launch(const SidecarLaunchOptions& options) {
  auto process = std::make_unique<SidecarProcess>();
  process->start(options);
  return SidecarClient(std::move(process), options.ioTimeout);
}

SidecarClient::SidecarClient(std::unique_ptr<SidecarProcess> process,
                             std::chrono::milliseconds timeout)
    : process_(std::move(process)), timeout_(timeout) {}

SidecarClient::~SidecarClient() {
  if (!shutdown_) {
    try {
      shutdown();
    } catch (...) {
      if (process_) {
        process_->terminate();
      }
    }
  }
}

SidecarResponse SidecarClient::health() {
  return request("health");
}

SidecarResponse SidecarClient::status() {
  return request("status");
}

SidecarResponse SidecarClient::infer(const InferRequestContract& contract) {
  return request(makeInferRequest(nextRequestId(), contract));
}

SidecarResponse SidecarClient::infer(const InferRequestContract& contract,
                                     const CancelCallback& shouldCancel) {
  if (!shouldCancel) {
    return infer(contract);
  }
  return requestCancellable(makeInferRequest(nextRequestId(), contract), contract.jobId,
                            shouldCancel);
}

SidecarResponse SidecarClient::warmup(const WarmupRequestContract& contract,
                                      const CancelCallback& shouldCancel) {
  if (!shouldCancel) {
    return request(makeWarmupRequest(nextRequestId(), contract));
  }
  return requestCancellable(makeWarmupRequest(nextRequestId(), contract), contract.jobId,
                            shouldCancel);
}

void SidecarClient::shutdown() {
  if (shutdown_ || !process_) {
    return;
  }
  try {
    (void)request("shutdown");
    const int exitCode = process_->waitForExit(timeout_);
    if (exitCode == -1) {
      process_->terminate();
      shutdown_ = true;
      throw LaunchError("sidecar did not exit after shutdown");
    }
    if (exitCode != 0) {
      shutdown_ = true;
      throw LaunchError("sidecar exited with a non-zero status");
    }
  } catch (...) {
    process_->terminate();
    shutdown_ = true;
    throw;
  }
  shutdown_ = true;
}

std::string SidecarClient::readStderrAvailable() {
  return process_ ? process_->readStderrAvailable() : std::string{};
}

SidecarResponse SidecarClient::request(const std::string& command) {
  return request(SidecarRequest{nextRequestId(), command, {}});
}

SidecarResponse SidecarClient::request(const SidecarRequest& request) {
  if (!process_) {
    throw LaunchError("sidecar client has no process");
  }
  if (!process_->isRunning()) {
    throw LaunchError("sidecar process is not running");
  }
  process_->writeLine(encodeRequest(request));
  SidecarResponse response = decodeResponse(process_->readLine(timeout_));
  if (response.requestId != request.requestId) {
    throw ProtocolError("sidecar response request_id did not match request");
  }
  return response;
}

SidecarResponse SidecarClient::requestCancellable(const SidecarRequest& request,
                                                  const std::string& jobId,
                                                  const CancelCallback& shouldCancel) {
  if (!process_) {
    throw LaunchError("sidecar client has no process");
  }
  if (!process_->isRunning()) {
    throw LaunchError("sidecar process is not running");
  }
  if (shouldCancel && shouldCancel()) {
    return cancelledResponse(request.requestId, jobId);
  }

  process_->writeLine(encodeRequest(request));
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  bool cancelSent = false;
  bool cancelAcked = false;
  std::string cancelRequestId;
  std::optional<std::chrono::steady_clock::time_point> cancelDeadline;

  while (std::chrono::steady_clock::now() < deadline) {
    if (!cancelSent && shouldCancel && shouldCancel()) {
      cancelRequestId = nextRequestId();
      process_->writeLine(encodeRequest(makeCancelRequest(cancelRequestId, jobId)));
      cancelSent = true;
      cancelDeadline = std::chrono::steady_clock::now() + kCancelGracePeriod;
    }

    const auto now = std::chrono::steady_clock::now();
    if (cancelDeadline && !cancelAcked && now >= *cancelDeadline) {
      process_->terminate();
      shutdown_ = true;
      return cancelledResponse(request.requestId, jobId);
    }
    if (now >= deadline) {
      break;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    auto slice = std::min(kAbortPollInterval, std::max(std::chrono::milliseconds{1},
                                                       remaining));
    if (cancelDeadline && !cancelAcked) {
      const auto cancelRemaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(*cancelDeadline - now);
      slice = std::min(slice, std::max(std::chrono::milliseconds{1}, cancelRemaining));
    }
    try {
      SidecarResponse response = decodeResponse(process_->readLine(slice));
      if (response.requestId == request.requestId) {
        if (cancelSent) {
          const auto ackDeadline =
              std::chrono::steady_clock::now() + kCancelGracePeriod;
          while (!cancelAcked && std::chrono::steady_clock::now() < ackDeadline) {
            const auto ackNow = std::chrono::steady_clock::now();
            const auto ackRemaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(ackDeadline - ackNow);
            try {
              const SidecarResponse ack =
                  decodeResponse(process_->readLine(std::max(std::chrono::milliseconds{1},
                                                             ackRemaining)));
              if (ack.requestId == cancelRequestId && ack.ok &&
                  ack.payloadValue("cancel") == "accepted" &&
                  ack.payloadValue("job_id") == jobId) {
                cancelAcked = true;
                break;
              }
              throw ProtocolError("sidecar response request_id did not match request");
            } catch (const IoTimeout&) {
              continue;
            }
          }
          if (!cancelAcked) {
            process_->terminate();
            shutdown_ = true;
          }
          if (response.ok) {
            return cancelledResponse(request.requestId, jobId);
          }
        }
        return response;
      }
      if (cancelSent && response.requestId == cancelRequestId && response.ok &&
          response.payloadValue("cancel") == "accepted" &&
          response.payloadValue("job_id") == jobId) {
        cancelAcked = true;
        continue;
      }
      throw ProtocolError("sidecar response request_id did not match request");
    } catch (const IoTimeout&) {
      continue;
    }
  }

  process_->terminate();
  shutdown_ = true;
  throw LaunchError("timed out reading sidecar stdout");
}

std::string SidecarClient::nextRequestId() {
  std::ostringstream id;
  id << "req-cpp-" << ++requestCounter_;
  return id.str();
}

}  // namespace CorridorKey::Sidecar

namespace {

void copyRuntimeString(char* destination, std::size_t destinationSize,
                       const std::string& value) {
  if (destinationSize == 0) {
    return;
  }
  const std::size_t count = std::min(destinationSize - 1, value.size());
  std::copy_n(value.data(), count, destination);
  destination[count] = '\0';
}

std::string escapeLogString(const std::string& value) {
  std::string escaped;
  for (const unsigned char c : value) {
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
        if (c >= 0x20 && c < 0x7F) {
          escaped.push_back(static_cast<char>(c));
        } else {
          escaped.push_back('?');
        }
        break;
    }
  }
  return escaped;
}

}  // namespace

namespace CorridorKey::Sidecar {
namespace {

void logRuntimeFailure(const SidecarStatusSurface& surface) {
  std::cerr << "{\"code\":\"" << escapeLogString(surface.errorCode)
            << "\",\"event\":\"runtime_failure\",\"level\":\"error\",\"message\":\""
            << escapeLogString(surface.lastError) << "\"}\n";
}

std::string stringOrEmpty(const char* value) {
  return value == nullptr ? std::string{} : std::string{value};
}

bool testFaultsAllowed() {
  const char* value = std::getenv("CORRIDORKEY_TEST_FAULTS_ALLOWED");
  return value != nullptr && std::string{value} == "1";
}

CorridorKey::Sidecar::SidecarLaunchOptions launchOptionsFromWire(
    const char* bundleRoot,
    const char* runtimePath,
    int timeoutMilliseconds) {
  CorridorKey::Sidecar::SidecarLaunchOptions options;
  options.bundleRoot = stringOrEmpty(bundleRoot);
  options.pythonExecutable = stringOrEmpty(runtimePath);
  options.context = CorridorKey::Sidecar::LaunchContext::Render;
  options.ioTimeout = std::chrono::milliseconds(std::max(1, timeoutMilliseconds));
  options.allowTestFaultEnvironment = testFaultsAllowed();
  return options;
}

}  // namespace
}  // namespace CorridorKey::Sidecar

extern "C" int CorridorKeyProbeRuntimeStatus(const char* bundleRoot,
                                             const char* runtimePath,
                                             int timeoutMilliseconds,
                                             void* outputBuffer,
                                             std::size_t outputSize) {
  RuntimeStatusWire result;
  try {
    const CorridorKey::Sidecar::SidecarStatusSurface surface =
        CorridorKey::Sidecar::probeStatusWithRecovery(
        CorridorKey::Sidecar::launchOptionsFromWire(bundleRoot, runtimePath,
                                                    timeoutMilliseconds));
    result.ok = surface.ok;
    result.restarted = surface.restarted;
    copyRuntimeString(result.state, sizeof(result.state), surface.state);
    copyRuntimeString(result.checkpoint, sizeof(result.checkpoint), surface.modelVersion);
    copyRuntimeString(result.modelStatus, sizeof(result.modelStatus), surface.modelStatus);
    copyRuntimeString(result.modelSourceStatus, sizeof(result.modelSourceStatus),
                      surface.modelSourceStatus);
    copyRuntimeString(result.installStatus, sizeof(result.installStatus),
                      surface.installStatus);
    copyRuntimeString(result.downloadStatus, sizeof(result.downloadStatus),
                      surface.downloadStatus);
    copyRuntimeString(result.downloadProgress, sizeof(result.downloadProgress),
                      surface.downloadProgress);
    copyRuntimeString(result.compute, sizeof(result.compute), surface.gpu);
    copyRuntimeString(result.backend, sizeof(result.backend), surface.backend);
    copyRuntimeString(result.backendStatus, sizeof(result.backendStatus),
                      surface.backendStatus);
    copyRuntimeString(result.warmup, sizeof(result.warmup), surface.warmup);
    copyRuntimeString(result.cache, sizeof(result.cache), surface.cache);
    copyRuntimeString(result.memory, sizeof(result.memory), surface.vram);
    copyRuntimeString(result.errorCode, sizeof(result.errorCode), surface.errorCode);
    copyRuntimeString(result.lastError, sizeof(result.lastError), surface.lastError);
    if (!surface.ok) {
      logRuntimeFailure(surface);
    }
  } catch (...) {
    result.ok = false;
    copyRuntimeString(result.state, sizeof(result.state), "error");
    copyRuntimeString(result.errorCode, sizeof(result.errorCode), "request_failed");
    copyRuntimeString(result.lastError, sizeof(result.lastError),
                      "status probe failed");
  }
  if (outputBuffer != nullptr && outputSize > 0) {
    std::memset(outputBuffer, 0, outputSize);
    std::memcpy(outputBuffer, &result, std::min(outputSize, sizeof(result)));
  }
  return result.ok ? 1 : 0;
}

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
                                       std::size_t outputSize) {
  RuntimeInferWire result;

  try {
    const CorridorKey::Sidecar::SidecarLaunchOptions options =
        CorridorKey::Sidecar::launchOptionsFromWire(bundleRoot, runtimePath,
                                                    timeoutMilliseconds);

    CorridorKey::Sidecar::SidecarClient client =
        CorridorKey::Sidecar::SidecarClient::launch(options);
    const std::string backend =
        backendChoice == 0
            ? (CorridorKey::Sidecar::testStubInferForced() ? "stub" : "auto")
            : CorridorKey::Sidecar::backendToken(backendChoice);
    const CorridorKey::Sidecar::InferRequestContract contract{
        CorridorKey::Sidecar::stringOrEmpty(jobId),
        CorridorKey::Sidecar::stringOrEmpty(frameId),
        renderWindowX1,
        renderWindowY1,
        renderWindowX2,
        renderWindowY2,
        CorridorKey::Sidecar::stringOrEmpty(sourceFrameBlobPath),
        CorridorKey::Sidecar::stringOrEmpty(alphaHintFrameBlobPath),
        CorridorKey::Sidecar::alphaHintSourceToken(alphaHintChoice),
        CorridorKey::Sidecar::screenColorToken(screenColorChoice),
        CorridorKey::Sidecar::qualityToken(qualityChoice),
        CorridorKey::Sidecar::inputColorSpaceToken(inputColorSpaceChoice),
        despillStrength,
        backend,
        CorridorKey::Sidecar::outputModeToken(outputModeChoice),
    };
    const CorridorKey::Sidecar::CancelCallback cancelCallback =
        cancelRequested == nullptr
            ? CorridorKey::Sidecar::CancelCallback{}
            : CorridorKey::Sidecar::CancelCallback{
                  [cancelRequested, cancelContext]() {
                    return cancelRequested(cancelContext) != 0;
                  }};
    const CorridorKey::Sidecar::SidecarResponse response =
        client.infer(contract, cancelCallback);
    if (!response.ok) {
      const std::string code = response.error ? response.error->code : "unknown";
      const std::string message =
          response.error ? response.error->message : "sidecar infer failed";
      copyRuntimeString(result.jobId, sizeof(result.jobId),
                        response.payloadValue("job_id"));
      copyRuntimeString(result.backend, sizeof(result.backend),
                        response.payloadValue("backend").empty()
                            ? contract.backend
                            : response.payloadValue("backend"));
      copyRuntimeString(result.backendStatus, sizeof(result.backendStatus),
                        response.payloadValue("backend_status"));
      copyRuntimeString(result.checkpoint, sizeof(result.checkpoint),
                        response.payloadValue("model"));
      copyRuntimeString(result.modelStatus, sizeof(result.modelStatus),
                        response.payloadValue("model_status"));
      copyRuntimeString(result.modelSourceStatus, sizeof(result.modelSourceStatus),
                        response.payloadValue("model_source_status"));
      copyRuntimeString(result.installStatus, sizeof(result.installStatus),
                        response.payloadValue("install_status"));
      copyRuntimeString(result.cache, sizeof(result.cache), response.payloadValue("cache"));
      copyRuntimeString(result.requestedQuality, sizeof(result.requestedQuality),
                        response.payloadValue("requested_quality"));
      copyRuntimeString(result.effectiveQuality, sizeof(result.effectiveQuality),
                        response.payloadValue("effective_quality"));
      copyRuntimeString(result.queueTimeMs, sizeof(result.queueTimeMs),
                        response.payloadValue("queue_time_ms"));
      copyRuntimeString(result.oom, sizeof(result.oom), response.payloadValue("oom"));
      copyRuntimeString(result.downgradedQuality, sizeof(result.downgradedQuality),
                        response.payloadValue("downgraded_quality"));
      copyRuntimeString(result.finalFailure, sizeof(result.finalFailure),
                        response.payloadValue("final_failure"));
      copyRuntimeString(result.errorCode, sizeof(result.errorCode), code);
      copyRuntimeString(result.lastError, sizeof(result.lastError),
                        CorridorKey::Sidecar::redactForStatus(message));
    } else {
      result.ok = true;
      copyRuntimeString(result.jobId, sizeof(result.jobId),
                        response.payloadValue("job_id"));
      copyRuntimeString(result.processedPath, sizeof(result.processedPath),
                        response.payloadValue("processed_rgba_frame_blob_path"));
      copyRuntimeString(result.straightPath, sizeof(result.straightPath),
                        response.payloadValue("straight_fg_frame_blob_path"));
      copyRuntimeString(result.alphaPath, sizeof(result.alphaPath),
                        response.payloadValue("alpha_frame_blob_path"));
      copyRuntimeString(result.backend, sizeof(result.backend),
                        response.payloadValue("backend"));
      copyRuntimeString(result.backendStatus, sizeof(result.backendStatus),
                        response.payloadValue("backend_status"));
      copyRuntimeString(result.checkpoint, sizeof(result.checkpoint),
                        response.payloadValue("model"));
      copyRuntimeString(result.modelStatus, sizeof(result.modelStatus),
                        response.payloadValue("model_status"));
      copyRuntimeString(result.modelSourceStatus, sizeof(result.modelSourceStatus),
                        response.payloadValue("model_source_status"));
      copyRuntimeString(result.installStatus, sizeof(result.installStatus),
                        response.payloadValue("install_status"));
      copyRuntimeString(result.cache, sizeof(result.cache), response.payloadValue("cache"));
      copyRuntimeString(result.requestedQuality, sizeof(result.requestedQuality),
                        response.payloadValue("requested_quality"));
      copyRuntimeString(result.effectiveQuality, sizeof(result.effectiveQuality),
                        response.payloadValue("effective_quality"));
      copyRuntimeString(result.queueTimeMs, sizeof(result.queueTimeMs),
                        response.payloadValue("queue_time_ms"));
      copyRuntimeString(result.oom, sizeof(result.oom), response.payloadValue("oom"));
      copyRuntimeString(result.downgradedQuality, sizeof(result.downgradedQuality),
                        response.payloadValue("downgraded_quality"));
      copyRuntimeString(result.finalFailure, sizeof(result.finalFailure),
                        response.payloadValue("final_failure"));
    }
    try {
      if (result.ok) {
        client.shutdown();
      }
    } catch (...) {
    }
  } catch (const std::exception& exc) {
    result.ok = false;
    copyRuntimeString(result.errorCode, sizeof(result.errorCode),
                      CorridorKey::Sidecar::classifyInferException(exc.what()));
    copyRuntimeString(result.lastError, sizeof(result.lastError),
                      CorridorKey::Sidecar::redactForStatus(exc.what()));
  }

  if (outputBuffer != nullptr && outputSize > 0) {
    std::memset(outputBuffer, 0, outputSize);
    std::memcpy(outputBuffer, &result, std::min(outputSize, sizeof(result)));
  }
  return result.ok ? 1 : 0;
}

extern "C" int CorridorKeyRunWarmup(const char* bundleRoot,
                                    const char* runtimePath,
                                    int timeoutMilliseconds,
                                    const char* jobId,
                                    int backendChoice,
                                    int qualityChoice,
                                    int (*cancelRequested)(void*),
                                    void* cancelContext,
                                    void* outputBuffer,
                                    std::size_t outputSize) {
  RuntimeWarmupWire result;

  try {
    const CorridorKey::Sidecar::SidecarLaunchOptions options =
        CorridorKey::Sidecar::launchOptionsFromWire(bundleRoot, runtimePath,
                                                    timeoutMilliseconds);
    CorridorKey::Sidecar::SidecarClient client =
        CorridorKey::Sidecar::SidecarClient::launch(options);
    const std::string backend =
        backendChoice == 0 ? "auto" : CorridorKey::Sidecar::backendToken(backendChoice);
    const CorridorKey::Sidecar::WarmupRequestContract contract{
        CorridorKey::Sidecar::stringOrEmpty(jobId),
        backend,
        CorridorKey::Sidecar::qualityToken(qualityChoice),
    };
    const CorridorKey::Sidecar::CancelCallback cancelCallback =
        cancelRequested == nullptr
            ? CorridorKey::Sidecar::CancelCallback{}
            : CorridorKey::Sidecar::CancelCallback{
                  [cancelRequested, cancelContext]() {
                    return cancelRequested(cancelContext) != 0;
                  }};
    const CorridorKey::Sidecar::SidecarResponse response =
        client.warmup(contract, cancelCallback);
    if (response.ok) {
      result.ok = true;
      copyRuntimeString(result.jobId, sizeof(result.jobId), response.payloadValue("job_id"));
      copyRuntimeString(result.backend, sizeof(result.backend),
                        response.payloadValue("backend"));
      copyRuntimeString(result.backendStatus, sizeof(result.backendStatus),
                        response.payloadValue("backend_status"));
      copyRuntimeString(result.modelStatus, sizeof(result.modelStatus),
                        response.payloadValue("model_status"));
      copyRuntimeString(result.modelSourceStatus, sizeof(result.modelSourceStatus),
                        response.payloadValue("model_source_status"));
      copyRuntimeString(result.installStatus, sizeof(result.installStatus),
                        response.payloadValue("install_status"));
      copyRuntimeString(result.warmup, sizeof(result.warmup),
                        response.payloadValue("warmup"));
    } else {
      const std::string code = response.error ? response.error->code : "unknown";
      const std::string message =
          response.error ? response.error->message : "sidecar warmup failed";
      copyRuntimeString(result.jobId, sizeof(result.jobId),
                        response.payloadValue("job_id"));
      copyRuntimeString(result.backend, sizeof(result.backend),
                        response.payloadValue("backend").empty()
                            ? contract.backend
                            : response.payloadValue("backend"));
      copyRuntimeString(result.backendStatus, sizeof(result.backendStatus),
                        response.payloadValue("backend_status"));
      copyRuntimeString(result.modelStatus, sizeof(result.modelStatus),
                        response.payloadValue("model_status"));
      copyRuntimeString(result.modelSourceStatus, sizeof(result.modelSourceStatus),
                        response.payloadValue("model_source_status"));
      copyRuntimeString(result.installStatus, sizeof(result.installStatus),
                        response.payloadValue("install_status"));
      copyRuntimeString(result.errorCode, sizeof(result.errorCode), code);
      copyRuntimeString(result.lastError, sizeof(result.lastError),
                        CorridorKey::Sidecar::redactForStatus(message));
    }
    try {
      if (result.ok) {
        client.shutdown();
      }
    } catch (...) {
    }
  } catch (const std::exception& exc) {
    result.ok = false;
    copyRuntimeString(result.errorCode, sizeof(result.errorCode), "warmup_failed");
    copyRuntimeString(result.lastError, sizeof(result.lastError),
                      CorridorKey::Sidecar::redactForStatus(exc.what()));
  }

  if (outputBuffer != nullptr && outputSize > 0) {
    std::memset(outputBuffer, 0, outputSize);
    std::memcpy(outputBuffer, &result, std::min(outputSize, sizeof(result)));
  }
  return result.ok ? 1 : 0;
}
