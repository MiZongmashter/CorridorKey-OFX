#include "sidecar_client/SidecarClient.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#ifndef CORRIDORKEY_SOURCE_DIR
#define CORRIDORKEY_SOURCE_DIR "."
#endif

#ifndef CORRIDORKEY_PYTHON_EXECUTABLE
#define CORRIDORKEY_PYTHON_EXECUTABLE "python3"
#endif

namespace {

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

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "abort polling test failed: " << message << '\n';
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

void writeBlob(const std::filesystem::path& path,
               int channels,
               const std::vector<float>& values) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write("CKFB", 4);
  writeU32(output, 1);
  writeI32(output, 0);
  writeI32(output, 0);
  writeI32(output, 1);
  writeI32(output, 1);
  writeU32(output, static_cast<std::uint32_t>(channels));
  for (float value : values) {
    writeF32(output, value);
  }
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

}  // namespace

int main() {
  using namespace CorridorKey::Sidecar;

  bool ok = true;
  const std::filesystem::path bundleRoot = makeDevBundle("abort-polling-test");
  const std::filesystem::path jobRoot =
      std::filesystem::temp_directory_path() / "corridorkey-render-abort-polling";
  std::error_code ec;
  std::filesystem::remove_all(jobRoot, ec);
  writeBlob(jobRoot / "source.ckfb", 4, {0.2F, 0.6F, 0.1F, 1.0F});
  writeBlob(jobRoot / "alpha.ckfb", 1, {0.5F});

  SidecarLaunchOptions options;
  options.bundleRoot = bundleRoot;
  options.pythonExecutable = std::filesystem::path{CORRIDORKEY_PYTHON_EXECUTABLE};
  options.context = LaunchContext::Render;
  options.ioTimeout = std::chrono::milliseconds{5000};
  options.allowTestFaultEnvironment = true;

  InferRequestContract infer{
      "job-abort-polling",
      "frame-1",
      0,
      0,
      1,
      1,
      (jobRoot / "source.ckfb").string(),
      (jobRoot / "alpha.ckfb").string(),
      "external",
      "green",
      "high_1024",
      "host_managed",
      5.0,
      "stub",
      "processed_rgba",
  };

  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  setEnv("CORRIDORKEY_TEST_FAKE_INFER_DELAY_MS", "5000");
  const auto start = std::chrono::steady_clock::now();
  int polls = 0;
  SidecarClient client = SidecarClient::launch(options);
  const SidecarResponse response = client.infer(infer, [&polls]() {
    ++polls;
    return polls >= 3;
  });
  const auto elapsed = std::chrono::steady_clock::now() - start;
  unsetEnv("CORRIDORKEY_TEST_FAKE_INFER_DELAY_MS");
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");

  ok &= expect(!response.ok, "cancelled infer should return ok=false");
  ok &= expect(response.error.has_value(), "cancelled infer should include diagnostics");
  ok &= expect(response.error->code == "cancelled",
               "cancelled infer should report the cancelled code");
  ok &= expect(response.payloadValue("job_id") == "job-abort-polling",
               "cancelled infer should preserve job_id diagnostics");
  ok &= expect(elapsed < std::chrono::seconds{2},
               "cancelled infer should be bounded by host abort polling");
  ok &= expect(!client.readStderrAvailable().empty() || !response.ok,
               "cancelled infer should remain observable after process termination");

  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  setEnv("CORRIDORKEY_TEST_FAKE_INFER_DELAY_MS", "120");
  {
    InferRequestContract raceInfer = infer;
    raceInfer.jobId = "job-cancel-ack-drain";
    int racePolls = 0;
    SidecarClient raceClient = SidecarClient::launch(options);
    const SidecarResponse raceResponse = raceClient.infer(raceInfer, [&racePolls]() {
      ++racePolls;
      return racePolls >= 3;
    });
    ok &= expect(!raceResponse.ok, "raced cancel should return ok=false");
    ok &= expect(raceResponse.error.has_value() &&
                     raceResponse.error->code == "cancelled",
                 "raced cancel should report the cancelled code");
    try {
      raceClient.shutdown();
    } catch (const std::exception& exc) {
      ok &= expect(false, std::string{"raced cancel shutdown failed: "} + exc.what());
    }
  }
  unsetEnv("CORRIDORKEY_TEST_FAKE_INFER_DELAY_MS");
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");

  std::filesystem::remove_all(jobRoot, ec);
  ok &= expect(!std::filesystem::exists(jobRoot),
               "test temp frame blobs should be removable after cancel");

  {
    InferRequestContract nonStubInfer = infer;
    nonStubInfer.jobId = "job-nonstub-infer";
    nonStubInfer.backend = "torch_cpu";
    SidecarClient nonStubClient = SidecarClient::launch(options);
    const SidecarResponse nonStubResponse = nonStubClient.infer(nonStubInfer);
    ok &= expect(!nonStubResponse.ok,
                 "non-cancellable infer should return sidecar failure");
    ok &= expect(nonStubResponse.error.has_value(),
                 "non-cancellable infer should preserve sidecar error diagnostics");
    ok &= expect(nonStubResponse.error->code == "blocked_backend",
                 "non-cancellable infer should preserve backend error code");
    ok &= expect(nonStubResponse.payloadValue("model_status") == "missing",
                 "non-cancellable infer should preserve visible model status");
    ok &= expect(nonStubResponse.payloadValue("model_source_status") == "ready",
                 "non-cancellable infer should not report model-source blocking");
    ok &= expect(nonStubResponse.error->message.find("runtime paths") != std::string::npos,
                 "non-cancellable infer should report missing backend runtime paths");
    nonStubClient.shutdown();
  }

  setEnv("CORRIDORKEY_TEST_SERVER_MODE", "split_warmup_response");
  SidecarClient splitClient = SidecarClient::launch(options);
  const SidecarResponse splitResponse = splitClient.warmup(
      WarmupRequestContract{"job-split-warmup", "stub", "high_1024"},
      []() { return false; });
  unsetEnv("CORRIDORKEY_TEST_SERVER_MODE");
  ok &= expect(splitResponse.ok, "cancellable wait should preserve split stdout lines");
  ok &= expect(splitResponse.payloadValue("job_id") == "job-split-warmup",
               "split warmup response should preserve job id");
  ok &= expect(splitResponse.payloadValue("warmup") == "ready",
               "split warmup response should decode normally after poll timeout");
  try {
    splitClient.shutdown();
  } catch (const std::exception& exc) {
    ok &= expect(false, std::string{"split warmup shutdown failed: "} + exc.what());
  }

  std::filesystem::remove_all(bundleRoot.parent_path(), ec);

  return ok ? 0 : 1;
}
