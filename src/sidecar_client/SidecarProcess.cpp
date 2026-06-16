#include "sidecar_client/SidecarProcess.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace CorridorKey::Sidecar {
namespace {

constexpr int kInvalidFd = -1;

#if defined(_WIN32)
std::wstring utf8ToWide(const std::string& text);
std::string wideToUtf8(const std::wstring& text);
#endif

bool startsWith(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

std::string upperAscii(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return text;
}

bool shouldDropInheritedEnv(const std::string& key, bool allowTestFaultEnvironment) {
  const std::string normalized = upperAscii(key);
  if (startsWith(normalized, "CORRIDORKEY_TEST_")) {
    return !allowTestFaultEnvironment;
  }
  if (startsWith(normalized, "CORRIDORKEY_")) {
    return true;
  }
  if (startsWith(normalized, "PYTHON") && normalized != "PYTHONNOUSERSITE") {
    return true;
  }
  if (normalized == "VIRTUAL_ENV" || normalized == "VIRTUAL_ENV_PROMPT") {
    return true;
  }
  if (startsWith(normalized, "CONDA") || normalized == "_CE_CONDA" ||
      normalized == "_CE_M") {
    return true;
  }
  if (startsWith(normalized, "DYLD_")) {
    return true;
  }
  if (normalized == "LD_LIBRARY_PATH" || normalized == "LD_PRELOAD") {
    return true;
  }
  if (normalized == "PATH") {
    return true;
  }
  return false;
}

std::map<std::string, std::string> inheritedEnvironmentMap(
    bool allowTestFaultEnvironment) {
  std::map<std::string, std::string> result;
#if defined(_WIN32)
  LPWCH block = ::GetEnvironmentStringsW();
  if (block != nullptr) {
    for (LPWCH entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
      const std::string item = wideToUtf8(entry);
      const std::size_t equal = item.find('=');
      if (equal == std::string::npos || equal == 0) {
        continue;
      }
      const std::string key = item.substr(0, equal);
      if (shouldDropInheritedEnv(key, allowTestFaultEnvironment)) {
        continue;
      }
      result[key] = item.substr(equal + 1);
    }
    (void)::FreeEnvironmentStringsW(block);
  }
#else
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string item{*entry};
    const std::size_t equal = item.find('=');
    if (equal == std::string::npos) {
      continue;
    }
    const std::string key = item.substr(0, equal);
    if (shouldDropInheritedEnv(key, allowTestFaultEnvironment)) {
      continue;
    }
    result[key] = item.substr(equal + 1);
  }
#endif
#if defined(_WIN32)
  result["PATH"] = "C:\\Windows\\System32;C:\\Windows";
#else
  result["PATH"] = "/usr/bin:/bin";
#endif
  result["PYTHONNOUSERSITE"] = "1";
  return result;
}

std::vector<std::string> environmentVector(bool allowTestFaultEnvironment) {
  std::vector<std::string> result;
  for (const auto& [key, value] : inheritedEnvironmentMap(allowTestFaultEnvironment)) {
    result.push_back(key + "=" + value);
  }
  return result;
}

#if defined(_WIN32)
void closeHandle(void*& handle) {
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
    (void)::CloseHandle(static_cast<HANDLE>(handle));
  }
  handle = nullptr;
}

void throwLastError(const std::string& prefix) {
  throw LaunchError(prefix + ": Windows error " + std::to_string(::GetLastError()));
}

std::wstring utf8ToWide(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) {
    return std::wstring(text.begin(), text.end());
  }
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  (void)::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              wide.data(), size);
  return wide;
}

std::string wideToUtf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
  if (size <= 0) {
    return std::string(text.begin(), text.end());
  }
  std::string utf8(static_cast<std::size_t>(size), '\0');
  (void)::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              utf8.data(), size, nullptr, nullptr);
  return utf8;
}

std::wstring quoteCommandArg(const std::wstring& arg) {
  std::wstring quoted = L"\"";
  std::size_t backslashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, L'\\');
    backslashes = 0;
    quoted.push_back(c);
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

std::vector<wchar_t> windowsEnvironmentBlock(const std::vector<std::string>& env) {
  std::vector<wchar_t> block;
  for (const std::string& item : env) {
    const std::wstring wide = utf8ToWide(item);
    block.insert(block.end(), wide.begin(), wide.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

#else
void closeFd(int& fd) {
  if (fd != kInvalidFd) {
    while (::close(fd) == -1 && errno == EINTR) {
    }
    fd = kInvalidFd;
  }
}

void setCloseOnExec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags != -1) {
    (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }
}

void throwErrno(const std::string& prefix) {
  throw LaunchError(prefix + ": " + std::strerror(errno));
}

void throwErrorCode(const std::string& prefix, int error) {
  throw LaunchError(prefix + ": " + std::strerror(error));
}

class ScopedSigpipeBlock {
 public:
  ScopedSigpipeBlock() {
    sigemptyset(&set_);
    sigaddset(&set_, SIGPIPE);
    active_ = ::pthread_sigmask(SIG_BLOCK, &set_, &previous_) == 0;
    if (active_) {
      sigset_t pending{};
      if (::sigpending(&pending) == 0) {
        alreadyPending_ = sigismember(&pending, SIGPIPE) == 1;
      }
    }
  }

  ~ScopedSigpipeBlock() {
    if (active_) {
      (void)::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }
  }

  void consumeGeneratedSignal() {
    if (!active_ || alreadyPending_) {
      return;
    }
    sigset_t pending{};
    if (::sigpending(&pending) != 0 || sigismember(&pending, SIGPIPE) != 1) {
      return;
    }
    int signal = 0;
    (void)::sigwait(&set_, &signal);
  }

 private:
  sigset_t set_{};
  sigset_t previous_{};
  bool active_ = false;
  bool alreadyPending_ = false;
};

void writeAll(int fd, const std::string& text) {
  ScopedSigpipeBlock sigpipeBlock;
  const char* data = text.data();
  std::size_t remaining = text.size();
  while (remaining > 0) {
    const ssize_t written = ::write(fd, data, remaining);
    if (written == -1) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EPIPE) {
        sigpipeBlock.consumeGeneratedSignal();
        throw LaunchError("sidecar stdin closed during write");
      }
      throwErrno("failed writing sidecar stdin");
    }
    if (written == 0) {
      throw LaunchError("sidecar stdin write made no progress");
    }
    data += written;
    remaining -= static_cast<std::size_t>(written);
  }
}

int timeoutMsUntil(const std::chrono::steady_clock::time_point& deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
  return static_cast<int>(std::max<long long>(1, remaining));
}
#endif

}  // namespace

bool isLaunchAllowed(LaunchContext context) {
  return context == LaunchContext::Instance || context == LaunchContext::Render;
}

SidecarProcess::~SidecarProcess() {
  try {
    terminate();
  } catch (...) {
  }
}

SidecarProcess::SidecarProcess(SidecarProcess&& other) noexcept {
  moveFrom(other);
}

SidecarProcess& SidecarProcess::operator=(SidecarProcess&& other) noexcept {
  if (this != &other) {
    terminate();
    moveFrom(other);
  }
  return *this;
}

void SidecarProcess::moveFrom(SidecarProcess& other) noexcept {
#if defined(_WIN32)
  childProcess_ = other.childProcess_;
  stdinWrite_ = other.stdinWrite_;
  stdoutRead_ = other.stdoutRead_;
  stderrRead_ = other.stderrRead_;
  other.childProcess_ = nullptr;
  other.stdinWrite_ = nullptr;
  other.stdoutRead_ = nullptr;
  other.stderrRead_ = nullptr;
#else
  childPid_ = other.childPid_;
  stdinFd_ = other.stdinFd_;
  stdoutFd_ = other.stdoutFd_;
  stderrFd_ = other.stderrFd_;
  other.childPid_ = -1;
  other.stdinFd_ = kInvalidFd;
  other.stdoutFd_ = kInvalidFd;
  other.stderrFd_ = kInvalidFd;
#endif
  stdoutLineBuffer_ = std::move(other.stdoutLineBuffer_);
  stderrBuffer_ = std::move(other.stderrBuffer_);
  other.stdoutLineBuffer_.clear();
  other.stderrBuffer_.clear();
}

std::filesystem::path SidecarProcess::sidecarWorkingDirectory(
    const std::filesystem::path& bundleRoot) {
  return std::filesystem::absolute(bundleRoot) / "Contents" / "Resources";
}

std::filesystem::path SidecarProcess::resolvePythonExecutable(
    const SidecarLaunchOptions& options) {
  if (!options.bundleRoot.empty()) {
    const std::filesystem::path resources =
        sidecarWorkingDirectory(options.bundleRoot);
#if defined(_WIN32)
    const std::filesystem::path bundledPython =
        resources / "python" / "python.exe";
#else
    const std::filesystem::path bundledPython =
        resources / "python" / "bin" / "python3";
#endif
    if (std::filesystem::exists(bundledPython)) {
      return bundledPython;
    }
  }
  return options.pythonExecutable;
}

std::vector<std::string> SidecarProcess::buildLaunchEnvironmentForTesting(
    const SidecarLaunchOptions& options) {
  return environmentVector(options.allowTestFaultEnvironment);
}

void SidecarProcess::start(const SidecarLaunchOptions& options) {
  if (!isLaunchAllowed(options.context)) {
    throw LaunchError("sidecar launch is only allowed from instance/render paths");
  }
#if defined(_WIN32)
  if (childProcess_ != nullptr) {
#else
  if (childPid_ != -1) {
#endif
    throw LaunchError("sidecar process is already started");
  }
  stdoutLineBuffer_.clear();
  if (options.bundleRoot.empty()) {
    throw LaunchError("sidecar bundle root is required");
  }
  const std::filesystem::path resolvedPython = resolvePythonExecutable(options);
  if (resolvedPython.empty() || !resolvedPython.is_absolute()) {
    throw LaunchError("sidecar python executable must be an absolute path");
  }
  if (options.moduleName.empty()) {
    throw LaunchError("sidecar module name is required");
  }

  const std::filesystem::path workingDirectory = sidecarWorkingDirectory(options.bundleRoot);
  if (!std::filesystem::is_directory(workingDirectory)) {
    throw LaunchError("sidecar resources directory is missing");
  }

#if defined(_WIN32)
  SECURITY_ATTRIBUTES pipeSecurity{};
  pipeSecurity.nLength = sizeof(pipeSecurity);
  pipeSecurity.bInheritHandle = TRUE;

  HANDLE stdinRead = nullptr;
  HANDLE stdinWrite = nullptr;
  HANDLE stdoutRead = nullptr;
  HANDLE stdoutWrite = nullptr;
  HANDLE stderrRead = nullptr;
  HANDLE stderrWrite = nullptr;
  if (!::CreatePipe(&stdinRead, &stdinWrite, &pipeSecurity, 0) ||
      !::CreatePipe(&stdoutRead, &stdoutWrite, &pipeSecurity, 0) ||
      !::CreatePipe(&stderrRead, &stderrWrite, &pipeSecurity, 0)) {
    void* handles[] = {stdinRead, stdinWrite, stdoutRead,
                       stdoutWrite, stderrRead, stderrWrite};
    for (void*& handle : handles) {
      closeHandle(handle);
    }
    throwLastError("failed creating sidecar pipes");
  }

  if (!::SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0) ||
      !::SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0) ||
      !::SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0)) {
    void* handles[] = {stdinRead, stdinWrite, stdoutRead,
                       stdoutWrite, stderrRead, stderrWrite};
    for (void*& handle : handles) {
      closeHandle(handle);
    }
    throwLastError("failed isolating sidecar pipe handles");
  }

  const std::vector<std::string> env =
      environmentVector(options.allowTestFaultEnvironment);
  std::vector<wchar_t> envBlock = windowsEnvironmentBlock(env);
  const std::wstring pythonPath = resolvedPython.wstring();
  std::wstring commandLine =
      quoteCommandArg(pythonPath) + L" -m " + quoteCommandArg(utf8ToWide(options.moduleName));

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = stdinRead;
  startup.StartupInfo.hStdOutput = stdoutWrite;
  startup.StartupInfo.hStdError = stderrWrite;

  SIZE_T attributeSize = 0;
  (void)::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
  std::vector<char> attributeStorage(attributeSize);
  startup.lpAttributeList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
  if (!::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                           &attributeSize)) {
    void* handles[] = {stdinRead, stdinWrite, stdoutRead,
                       stdoutWrite, stderrRead, stderrWrite};
    for (void*& handle : handles) {
      closeHandle(handle);
    }
    throwLastError("failed preparing sidecar handle allowlist");
  }

  HANDLE inheritedHandles[] = {stdinRead, stdoutWrite, stderrWrite};
  if (!::UpdateProcThreadAttribute(
          startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
          inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr)) {
    ::DeleteProcThreadAttributeList(startup.lpAttributeList);
    void* handles[] = {stdinRead, stdinWrite, stdoutRead,
                       stdoutWrite, stderrRead, stderrWrite};
    for (void*& handle : handles) {
      closeHandle(handle);
    }
    throwLastError("failed setting sidecar handle allowlist");
  }

  PROCESS_INFORMATION processInfo{};
  std::wstring workingDirectoryText = workingDirectory.wstring();
  std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back(L'\0');
  if (!::CreateProcessW(pythonPath.c_str(), mutableCommand.data(),
                        nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT,
                        envBlock.data(), workingDirectoryText.c_str(),
                        &startup.StartupInfo, &processInfo)) {
    ::DeleteProcThreadAttributeList(startup.lpAttributeList);
    void* handles[] = {stdinRead, stdinWrite, stdoutRead,
                       stdoutWrite, stderrRead, stderrWrite};
    for (void*& handle : handles) {
      closeHandle(handle);
    }
    throwLastError("failed spawning sidecar process");
  }
  ::DeleteProcThreadAttributeList(startup.lpAttributeList);

  (void)::CloseHandle(processInfo.hThread);
  childProcess_ = processInfo.hProcess;
  stdinWrite_ = stdinWrite;
  stdoutRead_ = stdoutRead;
  stderrRead_ = stderrRead;
  void* stdinReadHandle = stdinRead;
  void* stdoutWriteHandle = stdoutWrite;
  void* stderrWriteHandle = stderrWrite;
  closeHandle(stdinReadHandle);
  closeHandle(stdoutWriteHandle);
  closeHandle(stderrWriteHandle);
#else
  int stdinPipe[2] = {kInvalidFd, kInvalidFd};
  int stdoutPipe[2] = {kInvalidFd, kInvalidFd};
  int stderrPipe[2] = {kInvalidFd, kInvalidFd};
  if (::pipe(stdinPipe) == -1 || ::pipe(stdoutPipe) == -1 || ::pipe(stderrPipe) == -1) {
    closeFd(stdinPipe[0]);
    closeFd(stdinPipe[1]);
    closeFd(stdoutPipe[0]);
    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[0]);
    closeFd(stderrPipe[1]);
    throwErrno("failed creating sidecar pipes");
  }
  setCloseOnExec(stdinPipe[0]);
  setCloseOnExec(stdinPipe[1]);
  setCloseOnExec(stdoutPipe[0]);
  setCloseOnExec(stdoutPipe[1]);
  setCloseOnExec(stderrPipe[0]);
  setCloseOnExec(stderrPipe[1]);

  const std::vector<std::string> env =
      environmentVector(options.allowTestFaultEnvironment);
  std::vector<char*> envp;
  envp.reserve(env.size() + 1);
  for (const std::string& item : env) {
    envp.push_back(const_cast<char*>(item.c_str()));
  }
  envp.push_back(nullptr);

  const std::string python = resolvedPython.string();
  const std::string module = options.moduleName;
  const std::string workingDirectoryText = workingDirectory.string();
#if defined(__APPLE__)
  std::vector<char*> argv = {
      const_cast<char*>(python.c_str()),
      const_cast<char*>("-m"),
      const_cast<char*>(module.c_str()),
      nullptr,
  };
#else
  const std::string launcher =
      "import os, runpy, sys; os.chdir(sys.argv[1]); "
      "runpy.run_module(sys.argv[2], run_name='__main__')";
  std::vector<char*> argv = {
      const_cast<char*>(python.c_str()),
      const_cast<char*>("-c"),
      const_cast<char*>(launcher.c_str()),
      const_cast<char*>(workingDirectoryText.c_str()),
      const_cast<char*>(module.c_str()),
      nullptr,
  };
#endif

  posix_spawn_file_actions_t actions;
  int spawnError = ::posix_spawn_file_actions_init(&actions);
  if (spawnError != 0) {
    closeFd(stdinPipe[0]);
    closeFd(stdinPipe[1]);
    closeFd(stdoutPipe[0]);
    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[0]);
    closeFd(stderrPipe[1]);
    throwErrorCode("failed preparing sidecar file actions", spawnError);
  }

  auto failSpawnSetup = [&](const std::string& message, int error) {
    (void)::posix_spawn_file_actions_destroy(&actions);
    closeFd(stdinPipe[0]);
    closeFd(stdinPipe[1]);
    closeFd(stdoutPipe[0]);
    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[0]);
    closeFd(stderrPipe[1]);
    throwErrorCode(message, error);
  };

  spawnError = ::posix_spawn_file_actions_adddup2(&actions, stdinPipe[0], STDIN_FILENO);
  if (spawnError == 0) {
    spawnError = ::posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
  }
  if (spawnError == 0) {
    spawnError = ::posix_spawn_file_actions_adddup2(&actions, stderrPipe[1], STDERR_FILENO);
  }
  if (spawnError != 0) {
    failSpawnSetup("failed mapping sidecar stdio", spawnError);
  }

#if defined(__APPLE__)
  spawnError = ::posix_spawn_file_actions_addchdir_np(&actions,
                                                      workingDirectoryText.c_str());
  if (spawnError != 0) {
    failSpawnSetup("failed setting sidecar working directory", spawnError);
  }
#endif

#if !defined(__APPLE__) || !defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
  long openMax = ::sysconf(_SC_OPEN_MAX);
  if (openMax < 0) {
    openMax = 1024;
  }
  for (int fd = STDERR_FILENO + 1; fd < openMax; ++fd) {
    if (::fcntl(fd, F_GETFD, 0) == -1 && errno == EBADF) {
      continue;
    }
    spawnError = ::posix_spawn_file_actions_addclose(&actions, fd);
    if (spawnError != 0) {
      failSpawnSetup("failed isolating sidecar file descriptors", spawnError);
    }
  }
#endif

  posix_spawnattr_t attributes;
  spawnError = ::posix_spawnattr_init(&attributes);
  if (spawnError != 0) {
    failSpawnSetup("failed preparing sidecar spawn attributes", spawnError);
  }

  auto destroyAttributes = [&]() {
    (void)::posix_spawnattr_destroy(&attributes);
  };

#if defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
  short spawnFlags = 0;
  spawnError = ::posix_spawnattr_getflags(&attributes, &spawnFlags);
  if (spawnError == 0) {
    spawnError = ::posix_spawnattr_setflags(
        &attributes, static_cast<short>(spawnFlags | POSIX_SPAWN_CLOEXEC_DEFAULT));
  }
  if (spawnError != 0) {
    destroyAttributes();
    failSpawnSetup("failed setting sidecar close-on-exec defaults", spawnError);
  }
#endif

  pid_t pid = -1;
  spawnError = ::posix_spawn(&pid, python.c_str(), &actions, &attributes, argv.data(),
                             envp.data());
  destroyAttributes();
  (void)::posix_spawn_file_actions_destroy(&actions);
  if (spawnError != 0) {
    closeFd(stdinPipe[0]);
    closeFd(stdinPipe[1]);
    closeFd(stdoutPipe[0]);
    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[0]);
    closeFd(stderrPipe[1]);
    throwErrorCode("failed spawning sidecar process", spawnError);
  }

  if (pid == -1) {
    closeFd(stdinPipe[0]);
    closeFd(stdinPipe[1]);
    closeFd(stdoutPipe[0]);
    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[0]);
    closeFd(stderrPipe[1]);
    throw LaunchError("failed spawning sidecar process");
  }

  childPid_ = static_cast<int>(pid);
  stdinFd_ = stdinPipe[1];
  stdoutFd_ = stdoutPipe[0];
  stderrFd_ = stderrPipe[0];
  closeFd(stdinPipe[0]);
  closeFd(stdoutPipe[1]);
  closeFd(stderrPipe[1]);

  const int flags = ::fcntl(stderrFd_, F_GETFL, 0);
  if (flags != -1) {
    (void)::fcntl(stderrFd_, F_SETFL, flags | O_NONBLOCK);
  }
#endif
}

bool SidecarProcess::isRunning() {
#if defined(_WIN32)
  if (childProcess_ == nullptr) {
    return false;
  }
  const DWORD result = ::WaitForSingleObject(static_cast<HANDLE>(childProcess_), 0);
  if (result == WAIT_TIMEOUT) {
    return true;
  }
  closeHandle(childProcess_);
  closeDescriptors();
  return false;
#else
  if (childPid_ == -1) {
    return false;
  }
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(childPid_), &status, WNOHANG);
  if (result == 0) {
    return true;
  }
  if (result == static_cast<pid_t>(childPid_) || (result == -1 && errno == ECHILD)) {
    childPid_ = -1;
    closeDescriptors();
  }
  return false;
#endif
}

void SidecarProcess::writeLine(const std::string& line) {
#if defined(_WIN32)
  if (stdinWrite_ == nullptr) {
    throw LaunchError("sidecar stdin is closed");
  }
  std::string text = line + "\n";
  const char* data = text.data();
  std::size_t remaining = text.size();
  while (remaining > 0) {
    DWORD written = 0;
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(remaining, 64 * 1024));
    if (!::WriteFile(static_cast<HANDLE>(stdinWrite_), data, chunk, &written,
                     nullptr)) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_BROKEN_PIPE) {
        throw LaunchError("sidecar stdin closed during write");
      }
      throw LaunchError("failed writing sidecar stdin: Windows error " +
                        std::to_string(error));
    }
    if (written == 0) {
      throw LaunchError("sidecar stdin write made no progress");
    }
    data += written;
    remaining -= written;
  }
#else
  if (stdinFd_ == kInvalidFd) {
    throw LaunchError("sidecar stdin is closed");
  }
  writeAll(stdinFd_, line);
  writeAll(stdinFd_, "\n");
#endif
}

std::string SidecarProcess::readLine(std::chrono::milliseconds timeout) {
#if defined(_WIN32)
  if (stdoutRead_ == nullptr) {
    throw LaunchError("sidecar stdout is closed");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    stderrBuffer_ += readStderrAvailable();

    DWORD available = 0;
    if (!::PeekNamedPipe(static_cast<HANDLE>(stdoutRead_), nullptr, 0, nullptr,
                         &available, nullptr)) {
      throw LaunchError("sidecar stdout closed before a response");
    }
    if (available == 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        throw IoTimeout("timed out reading sidecar stdout");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    char c = '\0';
    DWORD bytesRead = 0;
    if (!::ReadFile(static_cast<HANDLE>(stdoutRead_), &c, 1, &bytesRead, nullptr) ||
        bytesRead == 0) {
      throw LaunchError("sidecar stdout closed before a response");
    }
    if (c == '\n') {
      std::string line = std::move(stdoutLineBuffer_);
      stdoutLineBuffer_.clear();
      return line;
    }
    stdoutLineBuffer_.push_back(c);
    if (stdoutLineBuffer_.size() > 1024 * 1024) {
      throw LaunchError("sidecar stdout line is too large");
    }
  }
#else
  if (stdoutFd_ == kInvalidFd) {
    throw LaunchError("sidecar stdout is closed");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    struct pollfd pollFds[2] = {
        {stdoutFd_, static_cast<short>(POLLIN | POLLHUP), 0},
        {stderrFd_, static_cast<short>(POLLIN | POLLHUP), 0},
    };
    const int pollResult = ::poll(pollFds, stderrFd_ == kInvalidFd ? 1 : 2,
                                  timeoutMsUntil(deadline));
    if (pollResult == 0) {
      throw IoTimeout("timed out reading sidecar stdout");
    }
    if (pollResult == -1) {
      if (errno == EINTR) {
        continue;
      }
      throwErrno("failed polling sidecar stdout");
    }
    if (stderrFd_ != kInvalidFd &&
        (pollFds[1].revents & (POLLIN | POLLHUP)) != 0) {
      stderrBuffer_ += readStderrAvailable();
    }
    if ((pollFds[0].revents & (POLLIN | POLLHUP)) == 0) {
      continue;
    }

    char c = '\0';
    const ssize_t bytesRead = ::read(stdoutFd_, &c, 1);
    if (bytesRead == 1) {
      if (c == '\n') {
        std::string line = std::move(stdoutLineBuffer_);
        stdoutLineBuffer_.clear();
        return line;
      }
      stdoutLineBuffer_.push_back(c);
      if (stdoutLineBuffer_.size() > 1024 * 1024) {
        throw LaunchError("sidecar stdout line is too large");
      }
      continue;
    }
    if (bytesRead == 0) {
      throw LaunchError("sidecar stdout closed before a response");
    }
    if (errno != EINTR) {
      throwErrno("failed reading sidecar stdout");
    }
  }
#endif
}

std::string SidecarProcess::readStderrAvailable() {
#if defined(_WIN32)
  if (stderrRead_ == nullptr) {
    return {};
  }
  std::string output;
  output.swap(stderrBuffer_);
  char buffer[4096];
  while (true) {
    DWORD available = 0;
    if (!::PeekNamedPipe(static_cast<HANDLE>(stderrRead_), nullptr, 0, nullptr,
                         &available, nullptr) ||
        available == 0) {
      break;
    }
    DWORD bytesRead = 0;
    const DWORD chunk =
        static_cast<DWORD>(std::min<DWORD>(available, sizeof(buffer)));
    if (!::ReadFile(static_cast<HANDLE>(stderrRead_), buffer, chunk, &bytesRead,
                    nullptr) ||
        bytesRead == 0) {
      break;
    }
    output.append(buffer, static_cast<std::size_t>(bytesRead));
  }
  return output;
#else
  if (stderrFd_ == kInvalidFd) {
    return {};
  }

  std::string output;
  output.swap(stderrBuffer_);
  char buffer[4096];
  while (true) {
    struct pollfd pollFd {
      stderrFd_, static_cast<short>(POLLIN | POLLHUP), 0
    };
    const int pollResult = ::poll(&pollFd, 1, 0);
    if (pollResult <= 0) {
      break;
    }
    const ssize_t bytesRead = ::read(stderrFd_, buffer, sizeof(buffer));
    if (bytesRead > 0) {
      output.append(buffer, static_cast<std::size_t>(bytesRead));
      continue;
    }
    if (bytesRead == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
      break;
    }
  }
  return output;
#endif
}

int SidecarProcess::waitForExit(std::chrono::milliseconds timeout) {
#if defined(_WIN32)
  if (childProcess_ == nullptr) {
    return 0;
  }
  const auto count = timeout.count();
  const DWORD waitMs =
      count < 0
          ? INFINITE
          : static_cast<DWORD>(std::min<long long>(count, INFINITE - 1));
  const DWORD result = ::WaitForSingleObject(static_cast<HANDLE>(childProcess_),
                                             waitMs);
  if (result == WAIT_TIMEOUT) {
    return -1;
  }
  if (result != WAIT_OBJECT_0) {
    throwLastError("failed waiting for sidecar process");
  }
  DWORD exitCode = 0;
  if (!::GetExitCodeProcess(static_cast<HANDLE>(childProcess_), &exitCode)) {
    throwLastError("failed reading sidecar exit code");
  }
  closeHandle(childProcess_);
  return static_cast<int>(exitCode);
#else
  if (childPid_ == -1) {
    return 0;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(childPid_), &status, WNOHANG);
    if (result == static_cast<pid_t>(childPid_)) {
      childPid_ = -1;
      if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
      }
      if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
      }
      return -1;
    }
    if (result == -1 && errno == ECHILD) {
      childPid_ = -1;
      return 0;
    }
    if (result == -1 && errno != EINTR) {
      throwErrno("failed waiting for sidecar process");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
#endif
}

void SidecarProcess::terminate() {
#if defined(_WIN32)
  closeHandle(stdinWrite_);
  if (childProcess_ != nullptr) {
    if (waitForExit(std::chrono::milliseconds(250)) == -1) {
      (void)::TerminateProcess(static_cast<HANDLE>(childProcess_), 1);
      (void)waitForExit(std::chrono::milliseconds(750));
    }
  }
#else
  closeFd(stdinFd_);
  if (childPid_ != -1) {
    if (waitForExit(std::chrono::milliseconds(250)) == -1) {
      (void)::kill(static_cast<pid_t>(childPid_), SIGTERM);
      if (waitForExit(std::chrono::milliseconds(750)) == -1) {
        (void)::kill(static_cast<pid_t>(childPid_), SIGKILL);
        (void)waitForExit(std::chrono::milliseconds(250));
      }
    }
  }
#endif
  closeDescriptors();
}

void SidecarProcess::closeDescriptors() {
#if defined(_WIN32)
  closeHandle(stdinWrite_);
  closeHandle(stdoutRead_);
  closeHandle(stderrRead_);
#else
  closeFd(stdinFd_);
  closeFd(stdoutFd_);
  closeFd(stderrFd_);
#endif
  stdoutLineBuffer_.clear();
  stderrBuffer_.clear();
}

}  // namespace CorridorKey::Sidecar
