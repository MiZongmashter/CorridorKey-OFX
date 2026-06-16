#include "sidecar_client/SidecarClient.h"
#include "sidecar_client/SidecarProtocol.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
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

constexpr int kTimeoutMs = 2000;

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

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "oom status test failed: " << message << '\n';
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
  writeI32(output, 2);
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

std::filesystem::path makeJobRoot(const std::string& name) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("corridorkey-render-" + name);
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root);
  writeBlob(root / "source.ckfb", 4,
            {0.2F, 0.7F, 0.1F, 1.0F, 0.8F, 0.3F, 0.2F, 0.5F});
  writeBlob(root / "alpha.ckfb", 1, {0.25F, 0.75F});
  return root;
}

RuntimeInferWire runInfer(const std::filesystem::path& bundleRoot,
                          const std::filesystem::path& jobRoot,
                          const char* jobId,
                          int qualityChoice) {
  RuntimeInferWire result{};
  const std::string bundleRootText = bundleRoot.string();
  const std::string sourcePath = (jobRoot / "source.ckfb").string();
  const std::string alphaPath = (jobRoot / "alpha.ckfb").string();
  (void)CorridorKeyRunStubInfer(
      bundleRootText.c_str(), CORRIDORKEY_PYTHON_EXECUTABLE, kTimeoutMs, jobId,
      "frame-oom-status", 0, 0, 2, 1, sourcePath.c_str(),
      alphaPath.c_str(), 0, 1, qualityChoice, 0, 5.0, 0, 0, nullptr,
      nullptr, &result, sizeof(result));
  return result;
}

}  // namespace

int main() {
  bool ok = true;
  const std::filesystem::path bundleRoot = makeDevBundle("oom-status-test");
  setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
  setEnv("CORRIDORKEY_TEST_FORCE_STUB_INFER", "1");

  setEnv("CORRIDORKEY_TEST_SYNTHETIC_OOM_QUALITIES", "full_2048");
  {
    const std::filesystem::path jobRoot = makeJobRoot("oom-status-downgrade");
    RuntimeInferWire infer = runInfer(bundleRoot, jobRoot, "job-oom-status-downgrade", 2);
    ok &= expect(infer.ok, "full quality synthetic OOM should downgrade and succeed");
    ok &= expect(std::string(infer.requestedQuality) == "full_2048",
                 "requested quality should cross the C wrapper");
    ok &= expect(std::string(infer.effectiveQuality) == "high_1024",
                 "effective quality should cross the C wrapper");
    ok &= expect(std::string(infer.oom) == "true", "OOM flag should cross the C wrapper");
    ok &= expect(std::string(infer.downgradedQuality) == "true",
                 "downgrade flag should cross the C wrapper");
    ok &= expect(std::string(infer.finalFailure) == "false",
                 "final failure flag should cross the C wrapper");
    ok &= expect(std::string(infer.queueTimeMs).empty() == false,
                 "queue time should cross the C wrapper");
    std::error_code ec;
    std::filesystem::remove_all(jobRoot, ec);
  }

  setEnv("CORRIDORKEY_TEST_SYNTHETIC_OOM_QUALITIES", "draft_512");
  {
    const std::filesystem::path jobRoot = makeJobRoot("oom-status-failure");
    RuntimeInferWire infer = runInfer(bundleRoot, jobRoot, "job-oom-status-failure", 0);
    ok &= expect(!infer.ok, "draft synthetic OOM should fail without crashing");
    ok &= expect(std::string(infer.errorCode) == "gpu_oom",
                 "failed OOM should expose gpu_oom");
    ok &= expect(std::string(infer.requestedQuality) == "draft_512",
                 "failed requested quality should cross the C wrapper");
    ok &= expect(std::string(infer.effectiveQuality) == "draft_512",
                 "failed effective quality should cross the C wrapper");
    ok &= expect(std::string(infer.oom) == "true",
                 "failed OOM flag should cross the C wrapper");
    ok &= expect(std::string(infer.finalFailure) == "true",
                 "failed final failure flag should cross the C wrapper");
    std::error_code ec;
    std::filesystem::remove_all(jobRoot, ec);
  }

  std::error_code ec;
  std::filesystem::remove_all(bundleRoot.parent_path(), ec);
  unsetEnv("CORRIDORKEY_TEST_SYNTHETIC_OOM_QUALITIES");
  unsetEnv("CORRIDORKEY_TEST_FORCE_STUB_INFER");
  unsetEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED");
  return ok ? 0 : 1;
}
