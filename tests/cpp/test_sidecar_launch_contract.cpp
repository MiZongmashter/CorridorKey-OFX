#include "sidecar_client/SidecarClient.h"
#include "sidecar_client/SidecarProcess.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef CORRIDORKEY_SOURCE_DIR
#define CORRIDORKEY_SOURCE_DIR "."
#endif

#ifndef CORRIDORKEY_PYTHON_EXECUTABLE
#define CORRIDORKEY_PYTHON_EXECUTABLE "python3"
#endif

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "sidecar launch contract test failed: " << message << '\n';
    return false;
  }
  return true;
}

std::map<std::string, std::string> envMap(const std::vector<std::string>& env) {
  std::map<std::string, std::string> result;
  for (const std::string& item : env) {
    const std::size_t equal = item.find('=');
    if (equal != std::string::npos) {
      result[item.substr(0, equal)] = item.substr(equal + 1);
    }
  }
  return result;
}

void setEnv(const char* key, const char* value) {
#if defined(_WIN32)
  _putenv_s(key, value);
#else
  setenv(key, value, 1);
#endif
}

std::filesystem::path makeDevBundle() {
  const std::filesystem::path sourceRoot{CORRIDORKEY_SOURCE_DIR};
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() /
      ("corridorkey-sidecar-launch-test-" + std::to_string(nonce));
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

}  // namespace

int main() {
  using namespace CorridorKey::Sidecar;

  bool ok = true;

  setEnv("PYTHONPATH", "/Users/alice/SecretShow/plate.exr");
  setEnv("PYTHONHOME", "/Users/alice/miniconda");
  setEnv("PYTHONUSERBASE", "/Users/alice/.local");
  setEnv("VIRTUAL_ENV", "/Users/alice/project/.venv");
  setEnv("CONDA_PREFIX", "/Users/alice/miniconda/envs/secret");
  setEnv("CONDA_DEFAULT_ENV", "secret");
  setEnv("CORRIDORKEY_REPO", "/Users/alice/dev/CorridorKey");
  setEnv("CORRIDORKEY_SOURCE_DIR", "/Users/alice/dev/CorridorKey-main");
  setEnv("CORRIDORKEY_MODEL_DIR", "/Users/alice/models/corridorkey");
  setEnv("CORRIDORKEY_MODEL_ROOT", "/Users/alice/legacy-models");
  setEnv("CORRIDORKEY_BACKEND_FIXTURE_DIR", "/Users/alice/private-fixtures");
  setEnv("CORRIDORKEY_DEVICE", "mps");
  setEnv("DYLD_LIBRARY_PATH", "/Applications/DaVinci Resolve/libs");
  setEnv("DYLD_FRAMEWORK_PATH", "/Applications/Autodesk/Flame/frameworks");
  setEnv("LD_LIBRARY_PATH", "/opt/resolve/libs");
  setEnv("PATH", "/Applications/DaVinci Resolve/bin:/Applications/Autodesk/Flame/bin:/usr/bin");
  setEnv("CORRIDORKEY_TEST_SERVER_MODE", "hang_on_health");

  const std::filesystem::path bundleRoot = makeDevBundle();
  SidecarLaunchOptions options;
  options.bundleRoot = bundleRoot;
  options.pythonExecutable = CORRIDORKEY_PYTHON_EXECUTABLE;
  options.context = LaunchContext::Instance;

  const std::filesystem::path bundledPython =
      bundleRoot / "Contents" / "Resources" / "python" / "bin" / "python3";
  std::filesystem::create_directories(bundledPython.parent_path());
  {
    std::ofstream launcher(bundledPython);
    launcher << "#!/bin/sh\nexec " << CORRIDORKEY_PYTHON_EXECUTABLE << " \"$@\"\n";
  }
  std::filesystem::permissions(
      bundledPython,
      std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read |
          std::filesystem::perms::owner_write,
      std::filesystem::perm_options::add);
  ok &= expect(SidecarProcess::resolvePythonExecutable(options) == bundledPython,
               "bundled python launcher should override the host fallback path");

  const auto sanitized = envMap(SidecarProcess::buildLaunchEnvironmentForTesting(options));
  ok &= expect(sanitized.count("PYTHONPATH") == 0, "inherited PYTHONPATH must be cleared");
  ok &= expect(sanitized.count("PYTHONHOME") == 0, "inherited PYTHONHOME must be cleared");
  ok &= expect(sanitized.count("PYTHONUSERBASE") == 0, "inherited PYTHONUSERBASE must be cleared");
  ok &= expect(sanitized.count("VIRTUAL_ENV") == 0, "inherited virtualenv must be cleared");
  ok &= expect(sanitized.count("CONDA_PREFIX") == 0, "inherited conda prefix must be cleared");
  ok &= expect(sanitized.count("CONDA_DEFAULT_ENV") == 0, "inherited conda env must be cleared");
  ok &= expect(sanitized.count("CORRIDORKEY_REPO") == 0,
               "inherited CorridorKey repo override must be cleared");
  ok &= expect(sanitized.count("CORRIDORKEY_SOURCE_DIR") == 0,
               "inherited CorridorKey source override must be cleared");
  ok &= expect(sanitized.count("CORRIDORKEY_MODEL_DIR") == 0,
               "inherited CorridorKey model override must be cleared");
  ok &= expect(sanitized.count("CORRIDORKEY_MODEL_ROOT") == 0,
               "legacy inherited CorridorKey model override must be cleared");
  ok &= expect(sanitized.count("CORRIDORKEY_BACKEND_FIXTURE_DIR") == 0,
               "inherited CorridorKey fixture override must be cleared");
  ok &= expect(sanitized.count("CORRIDORKEY_DEVICE") == 0,
               "inherited CorridorKey device override must be cleared");
  ok &= expect(sanitized.count("DYLD_LIBRARY_PATH") == 0, "DYLD_LIBRARY_PATH must be cleared");
  ok &= expect(sanitized.count("DYLD_FRAMEWORK_PATH") == 0, "DYLD_FRAMEWORK_PATH must be cleared");
  ok &= expect(sanitized.count("LD_LIBRARY_PATH") == 0, "LD_LIBRARY_PATH must be cleared");
  ok &= expect(sanitized.at("PYTHONNOUSERSITE") == "1", "PYTHONNOUSERSITE must be forced on");
  ok &= expect(sanitized.at("PATH").find("DaVinci") == std::string::npos,
               "host-appended Resolve PATH entries must not leak");
  ok &= expect(sanitized.at("PATH").find("Flame") == std::string::npos,
               "host-appended Flame PATH entries must not leak");
  ok &= expect(sanitized.count("CORRIDORKEY_TEST_SERVER_MODE") == 0,
               "test-only sidecar fault injection must be cleared by default");

  SidecarLaunchOptions faultOptions = options;
  faultOptions.allowTestFaultEnvironment = true;
  const auto faultEnv =
      envMap(SidecarProcess::buildLaunchEnvironmentForTesting(faultOptions));
  ok &= expect(faultEnv.at("CORRIDORKEY_TEST_SERVER_MODE") == "hang_on_health",
               "tests may explicitly opt in to sidecar fault injection");

  SidecarLaunchOptions describeOptions = options;
  describeOptions.context = LaunchContext::Describe;
  bool blockedDescribeLaunch = false;
  try {
    SidecarProcess process;
    process.start(describeOptions);
  } catch (const LaunchError&) {
    blockedDescribeLaunch = true;
  }
  ok &= expect(blockedDescribeLaunch, "describe launch attempts must be rejected");

  SidecarLaunchOptions scanOptions = options;
  scanOptions.context = LaunchContext::Scan;
  bool blockedScanLaunch = false;
  try {
    SidecarProcess process;
    process.start(scanOptions);
  } catch (const LaunchError&) {
    blockedScanLaunch = true;
  }
  ok &= expect(blockedScanLaunch, "scan launch attempts must be rejected");

  try {
    SidecarClient client = SidecarClient::launch(options);
    const SidecarResponse health = client.health();
    const SidecarResponse status = client.status();
    ok &= expect(health.requestId != status.requestId,
                 "health and status should use distinct request ids");
    ok &= expect(health.payloadValue("backend") == "stub", "health should reach Python sidecar");
    ok &= expect(status.payloadValue("model") == "not_loaded", "status should reach Python sidecar");
    ok &= expect(status.payloadValue("warmup") == "cold", "status should preserve payload");

    client.shutdown();
    const std::string stderrText = client.readStderrAvailable();
    ok &= expect(stderrText.find("SecretShow") == std::string::npos,
                 "stderr must not leak project-like paths from inherited env");
    ok &= expect(stderrText.find("\"event\"") != std::string::npos,
                 "stderr should contain structured sidecar logs");
    ok &= expect(stderrText.find("\"request_id\"") == std::string::npos,
                 "stderr logs should not include request ids");
  } catch (const std::exception& exc) {
    ok &= expect(false, std::string("sidecar launch/client query failed: ") + exc.what());
  }

#if !defined(_WIN32)
  {
    setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
    setEnv("CORRIDORKEY_TEST_SERVER_MODE", "report_cwd");
    SidecarLaunchOptions cwdOptions = options;
    cwdOptions.allowTestFaultEnvironment = true;
    try {
      SidecarClient cwdClient = SidecarClient::launch(cwdOptions);
      const SidecarResponse health = cwdClient.health();
      ok &= expect(health.ok, "sidecar cwd check should succeed");
      const std::filesystem::path actualCwd =
          std::filesystem::weakly_canonical(health.payloadValue("cwd"));
      const std::filesystem::path expectedCwd =
          std::filesystem::weakly_canonical(
              SidecarProcess::sidecarWorkingDirectory(bundleRoot));
      ok &= expect(actualCwd == expectedCwd,
                   "sidecar must launch with Resources as its working directory");
      cwdClient.shutdown();
    } catch (const std::exception& exc) {
      ok &= expect(false, std::string("cwd check failed: ") + exc.what());
    }
  }

  {
    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0) {
      setEnv("CORRIDORKEY_TEST_FAULTS_ALLOWED", "1");
      setEnv("CORRIDORKEY_TEST_SERVER_MODE", "check_inherited_fd");
      for (int socketFd : sockets) {
        setEnv("CORRIDORKEY_TEST_EXPECT_CLOSED_FD", std::to_string(socketFd).c_str());
        SidecarLaunchOptions fdOptions = options;
        fdOptions.allowTestFaultEnvironment = true;
        try {
          SidecarClient fdClient = SidecarClient::launch(fdOptions);
          const SidecarResponse health = fdClient.health();
          ok &= expect(health.ok, "sidecar must not inherit unrelated host file descriptors");
          ok &= expect(health.payloadValue("fd_inherited") == "false",
                       "test sidecar should report unrelated fd as closed");
          fdClient.shutdown();
        } catch (const std::exception& exc) {
          ok &= expect(false, std::string("fd inheritance check failed: ") + exc.what());
        }
      }
      close(sockets[0]);
      close(sockets[1]);
    }
  }
#endif

  std::error_code ec;
  std::filesystem::remove_all(bundleRoot.parent_path(), ec);
  return ok ? 0 : 1;
}
