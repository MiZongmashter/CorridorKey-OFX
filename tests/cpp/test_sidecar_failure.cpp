#include "core/PixelBuffer.h"
#include "core/StatusFrame.h"
#include "sidecar_client/SidecarProcess.h"

#include "ofxCore.h"
#include "ofxImageEffect.h"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifndef CORRIDORKEY_SOURCE_DIR
#define CORRIDORKEY_SOURCE_DIR "."
#endif

#ifndef CORRIDORKEY_PYTHON_EXECUTABLE
#define CORRIDORKEY_PYTHON_EXECUTABLE "python3"
#endif

namespace {

constexpr int kProbeTimeoutMs = 1000;

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

extern "C" int CorridorKeyProbeRuntimeStatus(const char* bundleRoot,
                                             const char* runtimePath,
                                             int timeoutMilliseconds,
                                             void* outputBuffer,
                                             std::size_t outputSize);
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

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "sidecar failure test failed: " << message << '\n';
    return false;
  }
  return true;
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

std::filesystem::path makeDevBundle(const std::string& name) {
  const std::filesystem::path sourceRoot{CORRIDORKEY_SOURCE_DIR};
  const std::filesystem::path tempRoot =
      std::filesystem::temp_directory_path() / ("corridorkey-" + name);
  std::error_code ec;
  std::filesystem::remove_all(tempRoot, ec);

  const std::filesystem::path resources =
      tempRoot / "CorridorKey.ofx.bundle" / "Contents" / "Resources";
  std::filesystem::create_directories(resources);
  std::filesystem::copy(sourceRoot / "sidecar",
                        resources / "sidecar",
                        std::filesystem::copy_options::recursive);
  return tempRoot / "CorridorKey.ofx.bundle";
}

std::string pathText(const std::filesystem::path& path) {
  return path.string();
}

CorridorKey::Sidecar::SidecarLaunchOptions launchOptions(
    const std::filesystem::path& bundleRoot) {
  CorridorKey::Sidecar::SidecarLaunchOptions options;
  options.bundleRoot = bundleRoot;
  options.pythonExecutable = CORRIDORKEY_PYTHON_EXECUTABLE;
  options.context = CorridorKey::Sidecar::LaunchContext::Render;
  options.ioTimeout = std::chrono::milliseconds(kProbeTimeoutMs);
  options.allowTestFaultEnvironment = true;
  return options;
}

}  // namespace

int main() {
  using namespace CorridorKey::Sidecar;

  bool ok = true;

  const std::filesystem::path bundleRoot = makeDevBundle("sidecar-failure-test");
  const std::string bundleRootText = pathText(bundleRoot);

  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  unsetEnv("CORRIDORKEY_TEST_SERVER_MODE");
  unsetEnv("CORRIDORKEY_TEST_MARKER_PATH");
  {
    RuntimeStatusWire status{};
    const int statusOk = CorridorKeyProbeRuntimeStatus(
        bundleRootText.c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        &status, sizeof(status));
    ok &= expect(statusOk == 1 && status.ok, "healthy sidecar status probe should succeed");
    ok &= expect(!status.restarted, "healthy sidecar should not require restart");
    ok &= expect(std::string(status.state) == "ready",
                 "healthy sidecar state should be ready");
    ok &= expect(std::string(status.backend) == "stub", "status should include backend");
    ok &= expect(std::string(status.backendStatus) == "stub",
                 "status should include backend status fallback");
    ok &= expect(std::string(status.checkpoint) == "not_loaded",
                 "status should include model version");
    ok &= expect(std::string(status.modelStatus) == "missing",
                 "status should include model presence");
    ok &= expect(std::string(status.modelSourceStatus) == "ready",
                 "status should include model source state");
    ok &= expect(std::string(status.installStatus) == "not_installed",
                 "status should include offline install state");
    ok &= expect(std::string(status.downloadStatus) == "not_started",
                 "status should include model download state");
    ok &= expect(std::string(status.downloadProgress) == "0/0",
                 "status should include model download progress");
    ok &= expect(std::string(status.compute) == "unknown",
                 "status should include GPU status");
    ok &= expect(std::string(status.memory) == "unknown",
                 "status should include VRAM status");
    ok &= expect(std::string(status.cache) == "enabled",
                 "status should include cache status");
    ok &= expect(std::string(status.warmup) == "cold",
                 "status should include warmup status");
  }

  {
    RuntimeWarmupWire warmup{};
    const int warmupOk = CorridorKeyRunWarmup(
        bundleRootText.c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        "job-warmup-auto", 0, 1, nullptr, nullptr, &warmup, sizeof(warmup));
    ok &= expect(warmupOk == 0 && !warmup.ok,
                 "default Auto warmup should route to the real backend path");
    ok &= expect(std::string(warmup.backend) == "auto",
                 "default Auto warmup should preserve auto backend");
    ok &= expect(std::string(warmup.backendStatus) == "blocked",
                 "default Auto warmup should expose blocked backend status when runtime paths are absent");
  }

  {
    setEnv("CORRIDORKEY_REPO", pathText(bundleRoot.parent_path() / "missing-corridorkey-repo"));
    setEnv("CORRIDORKEY_MODEL_DIR", pathText(bundleRoot.parent_path() / "missing-model-dir"));
    RuntimeInferWire infer{};
    const std::filesystem::path frameBlobRoot =
        std::filesystem::temp_directory_path() / "corridorkey-render-auto-infer-test";
    std::filesystem::create_directories(frameBlobRoot);
    const int inferOk = CorridorKeyRunStubInfer(
        bundleRootText.c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        "job-infer-auto", "frame-infer-auto", 0, 0, 1, 1,
        pathText(frameBlobRoot / "source.ckfb").c_str(),
        pathText(frameBlobRoot / "alpha.ckfb").c_str(), 0, 0, 1, 0, 5.0, 0, 0, nullptr,
        nullptr, &infer, sizeof(infer));
    ok &= expect(inferOk == 0 && !infer.ok,
                 "default Auto infer should route to the real backend path");
    ok &= expect(std::string(infer.errorCode) == "blocked_backend",
                 "default Auto infer should expose backend blocker code");
    ok &= expect(std::string(infer.backend) == "auto",
                 "default Auto infer should preserve auto backend");
    ok &= expect(std::string(infer.backendStatus) == "blocked",
                 "default Auto infer should expose blocked backend status");
    unsetEnv("CORRIDORKEY_REPO");
    unsetEnv("CORRIDORKEY_MODEL_DIR");
    std::error_code ec;
    std::filesystem::remove_all(frameBlobRoot, ec);
  }

  {
    const std::filesystem::path marker =
        bundleRoot.parent_path() / "sidecar-crash-once.marker";
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    setEnv("CORRIDORKEY_TEST_SERVER_MODE", "exit_once_on_status");
    setEnv("CORRIDORKEY_TEST_MARKER_PATH", marker.string());
    RuntimeStatusWire status{};
    const int statusOk = CorridorKeyProbeRuntimeStatus(
        bundleRootText.c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        &status, sizeof(status));
    ok &= expect(statusOk == 1 && status.ok,
                 "one sidecar crash during status should recover");
    ok &= expect(status.restarted, "crash recovery should report a restart attempt");
    ok &= expect(std::string(status.lastError).empty(),
                 "recovered status should not keep stale error text");
    unsetEnv("CORRIDORKEY_TEST_SERVER_MODE");
    unsetEnv("CORRIDORKEY_TEST_MARKER_PATH");
    std::filesystem::remove(marker, ec);
  }

  {
    setEnv("CORRIDORKEY_TEST_SERVER_MODE", "invalid_json_on_health");
    RuntimeStatusWire status{};
    const int statusOk = CorridorKeyProbeRuntimeStatus(
        bundleRootText.c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        &status, sizeof(status));
    ok &= expect(statusOk == 0 && !status.ok, "protocol failure should be non-fatal");
    ok &= expect(std::string(status.state) == "error",
                 "protocol failure should surface error state");
    ok &= expect(std::string(status.errorCode) == "protocol_error",
                 "protocol failure should use protocol_error code");
    ok &= expect(std::string(status.lastError).find("protocol") != std::string::npos,
                 "protocol failure should preserve a readable last error");
    unsetEnv("CORRIDORKEY_TEST_SERVER_MODE");
  }

  {
    setEnv("CORRIDORKEY_TEST_SERVER_MODE", "error_on_status");
    RuntimeStatusWire status{};
    const int statusOk = CorridorKeyProbeRuntimeStatus(
        bundleRootText.c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        &status, sizeof(status));
    ok &= expect(statusOk == 0 && !status.ok, "status ok=false should be non-fatal");
    ok &= expect(std::string(status.errorCode) == "protocol_error",
                 "status ok=false should surface protocol_error code");
    unsetEnv("CORRIDORKEY_TEST_SERVER_MODE");
  }

  {
    setEnv("CORRIDORKEY_TEST_SERVER_MODE", "hang_on_health");
    RuntimeStatusWire status{};
    const int statusOk = CorridorKeyProbeRuntimeStatus(
        bundleRootText.c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        &status, sizeof(status));
    ok &= expect(statusOk == 0 && !status.ok, "health timeout should be non-fatal");
    ok &= expect(std::string(status.errorCode) == "health_timeout",
                 "health timeout should use health_timeout code");
    unsetEnv("CORRIDORKEY_TEST_SERVER_MODE");
  }

  {
    const std::filesystem::path secretRoot =
        std::filesystem::path{"/Users/alice/SecretShow/shot010/plate.exr"};
    RuntimeStatusWire status{};
    const int statusOk = CorridorKeyProbeRuntimeStatus(
        pathText(secretRoot).c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        &status, sizeof(status));
    ok &= expect(statusOk == 0 && !status.ok,
                 "launch failure should be returned as status");
    ok &= expect(std::string(status.errorCode) == "launch_failed",
                 "launch failure should use launch_failed code");
    ok &= expect(std::string(status.lastError).find("SecretShow") == std::string::npos,
                 "launch failure must not expose project names");
    ok &= expect(std::string(status.lastError).find("plate.exr") == std::string::npos,
                 "launch failure must not expose raw media paths");
  }

  {
    const std::filesystem::path secretRoot =
        std::filesystem::path{"/Users/Alice Smith/Secret Show/shot010/plate.exr"};
    RuntimeStatusWire status{};
    const int statusOk = CorridorKeyProbeRuntimeStatus(
        pathText(secretRoot).c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        &status, sizeof(status));
    const std::string lastError = status.lastError;
    ok &= expect(statusOk == 0 && !status.ok,
                 "launch failure with spaces should be returned as status");
    ok &= expect(lastError.find("Alice Smith") == std::string::npos,
                 "launch failure must redact owner names containing spaces");
    ok &= expect(lastError.find("Secret Show") == std::string::npos,
                 "launch failure must redact project names containing spaces");
    ok &= expect(lastError.find("plate.exr") == std::string::npos,
                 "launch failure must redact media filenames after spaced paths");
  }

  {
    const std::filesystem::path secretRoot =
        std::filesystem::path{"C:/Users/Alice/SecretShow/shot010/plate.mov"};
    RuntimeStatusWire status{};
    const int statusOk = CorridorKeyProbeRuntimeStatus(
        pathText(secretRoot).c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        &status, sizeof(status));
    const std::string lastError = status.lastError;
    ok &= expect(statusOk == 0 && !status.ok,
                 "forward-slash Windows launch failure should be returned as status");
    ok &= expect(lastError.find("C:") == std::string::npos,
                 "forward-slash Windows paths must redact drive prefixes");
    ok &= expect(lastError.find("Alice") == std::string::npos,
                 "forward-slash Windows paths must redact user names");
    ok &= expect(lastError.find("plate.mov") == std::string::npos,
                 "forward-slash Windows paths must redact media filenames");
  }

  {
    setEnv("CORRIDORKEY_TEST_SERVER_MODE", "exit_after_start");
    SidecarProcess process;
    process.start(launchOptions(bundleRoot));
    ok &= expect(process.waitForExit(std::chrono::milliseconds(1000)) == 44,
                 "test sidecar should exit before stdin write");
    bool writeFailedWithoutSignalTermination = false;
    try {
      process.writeLine("{\"command\":\"health\",\"payload\":{},\"request_id\":\"req-pipe\"}");
    } catch (const LaunchError&) {
      writeFailedWithoutSignalTermination = true;
    }
    ok &= expect(writeFailedWithoutSignalTermination,
                 "broken stdin pipe should throw instead of terminating host process");
    unsetEnv("CORRIDORKEY_TEST_SERVER_MODE");
  }

  {
    setEnv("CORRIDORKEY_TEST_SERVER_MODE", "path_error_on_infer");
    RuntimeInferWire infer{};
    const int inferOk = CorridorKeyRunStubInfer(
        bundleRootText.c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        "job-path-error", "frame-path-error", 0, 0, 1, 1,
        "/tmp/source.ckfb", "/tmp/alpha.ckfb", 0, 0, 1, 0, 5.0, 0, 0, nullptr,
        nullptr, &infer, sizeof(infer));
    const std::string lastError = infer.lastError;
    ok &= expect(inferOk == 0 && !infer.ok,
                 "infer error should be returned as a non-fatal failure");
    ok &= expect(lastError.find("SecretShow") == std::string::npos,
                 "infer error must redact project names");
    ok &= expect(lastError.find("plate.exr") == std::string::npos,
                 "infer error must redact media filenames");
    unsetEnv("CORRIDORKEY_TEST_SERVER_MODE");
  }

  {
    setEnv("CORRIDORKEY_TEST_SERVER_MODE", "path_error_on_warmup");
    RuntimeWarmupWire warmup{};
    const int warmupOk = CorridorKeyRunWarmup(
        bundleRootText.c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        "job-warmup-path-error", 0, 1, nullptr, nullptr, &warmup, sizeof(warmup));
    const std::string lastError = warmup.lastError;
    ok &= expect(warmupOk == 0 && !warmup.ok,
                 "warmup error should be returned as a non-fatal failure");
    ok &= expect(lastError.find("SecretShow") == std::string::npos,
                 "warmup error must redact project names");
    ok &= expect(lastError.find("plate.exr") == std::string::npos,
                 "warmup error must redact media filenames");
    unsetEnv("CORRIDORKEY_TEST_SERVER_MODE");
  }

  {
    RuntimeWarmupWire warmup{};
    const int warmupOk = CorridorKeyRunWarmup(
        bundleRootText.c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kProbeTimeoutMs,
        "job-warmup-nonstub-model-source", 2, 1, nullptr, nullptr, &warmup,
        sizeof(warmup));
    ok &= expect(warmupOk == 0 && !warmup.ok,
                 "non-stub warmup should report missing backend runtime when not configured");
    ok &= expect(std::string(warmup.errorCode) == "blocked_backend",
                 "non-stub warmup should expose backend error code");
    ok &= expect(std::string(warmup.backend) == "torch_cpu",
                 "non-stub warmup should preserve selected backend");
    ok &= expect(std::string(warmup.backendStatus) == "blocked",
                 "non-stub warmup should expose blocked backend status");
    ok &= expect(std::string(warmup.modelStatus) == "missing",
                 "non-stub warmup should expose visible model status");
    ok &= expect(std::string(warmup.modelSourceStatus) == "ready",
                 "non-stub warmup should expose model source status");
    ok &= expect(std::string(warmup.installStatus) == "not_installed",
                 "non-stub warmup should expose install status");
  }

  {
    std::vector<float> output(80 * 40 * 4, -1.0F);
    CorridorKey::Core::PixelBufferView view{
        output.data(), CorridorKey::Core::RectI{0, 0, 80, 40},
        80 * 4 * static_cast<int>(sizeof(float)),
        CorridorKey::Core::imageFormatFromOfx(kOfxImageComponentRGBA,
                                              kOfxBitDepthFloat)};
    const CorridorKey::Core::StatusFrameContent content{
        CorridorKey::Core::StatusFrameSeverity::Error,
        "Sidecar Error",
        "Health timeout",
        {{"State", "error"}, {"Last Error", "health_timeout"}},
    };
    ok &= expect(CorridorKey::Core::renderStatusFrame(
                     view, CorridorKey::Core::RectI{0, 0, 80, 40}, content) ==
                     CorridorKey::Core::FormatStatus::Ok,
                 "sidecar failure should be renderable as an error frame");
  }

  std::error_code ec;
  std::filesystem::remove_all(bundleRoot.parent_path(), ec);
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");
  return ok ? 0 : 1;
}
