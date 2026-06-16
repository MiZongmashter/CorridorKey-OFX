#pragma once

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace CorridorKey::Sidecar {

class LaunchError : public std::runtime_error {
 public:
  explicit LaunchError(const std::string& message) : std::runtime_error(message) {}
};

class IoTimeout : public LaunchError {
 public:
  explicit IoTimeout(const std::string& message) : LaunchError(message) {}
};

enum class LaunchContext {
  Scan,
  Describe,
  Instance,
  Render,
};

struct SidecarLaunchOptions {
  std::filesystem::path bundleRoot;
  std::filesystem::path pythonExecutable;
  LaunchContext context = LaunchContext::Instance;
  std::string moduleName = "sidecar.corridorkey_sidecar.server";
  std::chrono::milliseconds ioTimeout{5000};
  bool allowTestFaultEnvironment = false;
};

class SidecarProcess {
 public:
  SidecarProcess() = default;
  ~SidecarProcess();

  SidecarProcess(const SidecarProcess&) = delete;
  SidecarProcess& operator=(const SidecarProcess&) = delete;
  SidecarProcess(SidecarProcess&& other) noexcept;
  SidecarProcess& operator=(SidecarProcess&& other) noexcept;

  void start(const SidecarLaunchOptions& options);
  bool isRunning();
  void writeLine(const std::string& line);
  std::string readLine(std::chrono::milliseconds timeout);
  std::string readStderrAvailable();
  int waitForExit(std::chrono::milliseconds timeout);
  void terminate();

  static std::filesystem::path sidecarWorkingDirectory(const std::filesystem::path& bundleRoot);
  static std::filesystem::path resolvePythonExecutable(const SidecarLaunchOptions& options);
  static std::vector<std::string> buildLaunchEnvironmentForTesting(
      const SidecarLaunchOptions& options);

 private:
  void closeDescriptors();
  void moveFrom(SidecarProcess& other) noexcept;

#if defined(_WIN32)
  void* childProcess_ = nullptr;
  void* stdinWrite_ = nullptr;
  void* stdoutRead_ = nullptr;
  void* stderrRead_ = nullptr;
#else
  int childPid_ = -1;
  int stdinFd_ = -1;
  int stdoutFd_ = -1;
  int stderrFd_ = -1;
#endif
  std::string stdoutLineBuffer_;
  std::string stderrBuffer_;
};

bool isLaunchAllowed(LaunchContext context);

}  // namespace CorridorKey::Sidecar
