#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef CORRIDORKEY_BUNDLE_DIRECTORY
#define CORRIDORKEY_BUNDLE_DIRECTORY "CorridorKey.ofx.bundle"
#endif

#ifndef CORRIDORKEY_BUNDLE_BINARY
#define CORRIDORKEY_BUNDLE_BINARY "CorridorKey.ofx"
#endif

#ifndef CORRIDORKEY_BUILD_BUNDLE_SCRIPT
#define CORRIDORKEY_BUILD_BUNDLE_SCRIPT "packaging/build_bundle.py"
#endif

#ifndef CORRIDORKEY_PLUGIN_BINARY
#define CORRIDORKEY_PLUGIN_BINARY "build/CorridorKey.ofx"
#endif

#ifndef CORRIDORKEY_PYTHON_EXECUTABLE
#define CORRIDORKEY_PYTHON_EXECUTABLE "python3"
#endif

namespace {

bool expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "bundle layout test failed: " << message << '\n';
    return false;
  }
  return true;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool isSameFile(const std::filesystem::path& left,
                const std::filesystem::path& right) {
  std::ifstream leftInput(left, std::ios::binary);
  std::ifstream rightInput(right, std::ios::binary);
  std::istreambuf_iterator<char> leftIt(leftInput);
  std::istreambuf_iterator<char> rightIt(rightInput);
  const std::istreambuf_iterator<char> end;
  while (leftIt != end && rightIt != end) {
    if (*leftIt != *rightIt) {
      return false;
    }
    ++leftIt;
    ++rightIt;
  }
  return leftIt == end && rightIt == end;
}

std::string quotePath(const std::filesystem::path& path) {
  std::string quoted = "'";
  for (const char ch : path.string()) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

std::filesystem::path currentPlatformBinaryDirectory() {
  return "MacOS";
}

void writeTextFile(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << text;
}

void addFilteredSidecarSentinels(const std::filesystem::path& sidecarSource) {
  writeTextFile(sidecarSource / "__pycache__" / "protocol.pyc", "cache");
  writeTextFile(sidecarSource / "corridorkey_sidecar" / "tests" / "test_protocol.py", "test");
  writeTextFile(sidecarSource / "corridorkey_sidecar" / "cached.pyc", "bytecode");
  for (const char* directory : {"torch", "mlx", "models", "runtime"}) {
    writeTextFile(sidecarSource / "corridorkey_sidecar" / directory / "sentinel.txt",
                  "must not be bundled");
  }
}

}  // namespace

int main() {
  const std::filesystem::path bundle_root{CORRIDORKEY_BUNDLE_DIRECTORY};
  const std::filesystem::path binary_name{CORRIDORKEY_BUNDLE_BINARY};
  const std::filesystem::path build_script{CORRIDORKEY_BUILD_BUNDLE_SCRIPT};
  const std::filesystem::path plugin_binary{CORRIDORKEY_PLUGIN_BINARY};
  const std::filesystem::path real_sidecar_source =
      build_script.parent_path().parent_path() / "sidecar";
  const std::vector<std::filesystem::path> expected_paths = {
      bundle_root / "Contents" / "Info.plist",
      bundle_root / "Contents" / "Resources",
      bundle_root / "Contents" / "Resources" / "sidecar",
      bundle_root / "Contents" / "Resources" / "sidecar" / "corridorkey_sidecar" /
          "__init__.py",
      bundle_root / "Contents" / "Resources" / "sidecar" / "corridorkey_sidecar" /
          "protocol.py",
      bundle_root / "Contents" / "Resources" / "sidecar" / "corridorkey_sidecar" /
          "redaction.py",
      bundle_root / "Contents" / "Resources" / "sidecar" / "corridorkey_sidecar" /
          "server.py",
  };

  bool ok = true;
  ok &= expect(bundle_root.filename() == "CorridorKey.ofx.bundle",
               "bundle directory must be CorridorKey.ofx.bundle");
  ok &= expect(bundle_root.extension() == ".bundle",
               "bundle directory must use .bundle suffix");
  ok &= expect(binary_name.filename() == "CorridorKey.ofx",
               "plugin binary must be named CorridorKey.ofx");
  ok &= expect(binary_name.extension() == ".ofx",
               "plugin binary must use .ofx suffix in the OpenFX bundle");

  ok &= expect(std::filesystem::is_regular_file(build_script),
               "packaging/build_bundle.py must exist for bundle convention validation");
  const std::string script = read_file(build_script);
  ok &= expect(script.find("BUNDLE_DIRECTORY = \"CorridorKey.ofx.bundle\"") != std::string::npos,
               "build_bundle.py must use CorridorKey.ofx.bundle as the bundle directory");
  ok &= expect(script.find("PLUGIN_BINARY_NAME = \"CorridorKey.ofx\"") != std::string::npos,
               "build_bundle.py must use CorridorKey.ofx as the plugin binary name");
  ok &= expect(script.find("current_platform_binary_directory()") != std::string::npos,
               "build_bundle.py must make dry-run match the current-platform dev bundle");
  ok &= expect(script.find("SIDECAR_PACKAGE_FILES") != std::string::npos,
               "build_bundle.py must include sidecar resources in the layout contract");
  ok &= expect(script.find("PLATFORM_BINARY_DIRECTORIES") == std::string::npos,
               "dry-run must not advertise platform binary directories the dev build cannot produce");

  for (const auto& expected_path : expected_paths) {
    ok &= expect(!expected_path.is_absolute(),
                 "expected bundle paths must be relative to the output root");
    ok &= expect(expected_path.string().find('\\') == std::string::npos,
                 "expected bundle paths must use portable separators in docs/scripts");
  }

  if (!ok) {
    return 1;
  }

  const std::filesystem::path output_root =
      std::filesystem::temp_directory_path() / "corridorkey-bundle-layout-test";
  const std::filesystem::path dry_run_output = output_root / "dry-run.txt";
  const std::filesystem::path test_sidecar_source = output_root / "sidecar-source";
  std::error_code ec;
  std::filesystem::remove_all(output_root, ec);
  std::filesystem::create_directories(output_root);

  const std::string dry_run_command = quotePath(CORRIDORKEY_PYTHON_EXECUTABLE) + " " +
      quotePath(build_script) + " --dry-run --output " + quotePath(output_root) +
      " > " + quotePath(dry_run_output);
  ok &= expect(std::system(dry_run_command.c_str()) == 0,
               "build_bundle.py dry-run should succeed");
  const std::string dry_run = read_file(dry_run_output);
  ok &= expect(dry_run.find((bundle_root / "Contents" /
                             currentPlatformBinaryDirectory() / binary_name)
                                .string()) != std::string::npos,
               "dry-run should advertise the current-platform plugin binary");
  std::filesystem::copy(real_sidecar_source, test_sidecar_source,
                        std::filesystem::copy_options::recursive);
  addFilteredSidecarSentinels(test_sidecar_source);
  const std::string command = quotePath(CORRIDORKEY_PYTHON_EXECUTABLE) + " " +
      quotePath(build_script) + " --output " + quotePath(output_root) +
      " --plugin-binary " + quotePath(plugin_binary) +
      " --sidecar-source " + quotePath(test_sidecar_source);
  ok &= expect(std::system(command.c_str()) == 0,
               "build_bundle.py should build a real dev bundle for layout verification");

  const std::filesystem::path builtBundle = output_root / bundle_root;
  const std::filesystem::path builtBinary =
      builtBundle / "Contents" / currentPlatformBinaryDirectory() / binary_name;
  const std::filesystem::path builtSidecar =
      builtBundle / "Contents" / "Resources" / "sidecar" / "corridorkey_sidecar";
  ok &= expect(std::filesystem::is_regular_file(builtBinary),
               "dev bundle should contain the current-platform plugin binary");
  ok &= expect(isSameFile(plugin_binary, builtBinary),
               "dev bundle binary should match the built plugin binary");
  for (const char* sidecar_file : {"__init__.py", "protocol.py", "redaction.py", "server.py"}) {
    ok &= expect(std::filesystem::is_regular_file(builtSidecar / sidecar_file),
                 "dev bundle should contain only the required sidecar package files");
  }
  if (std::filesystem::exists(builtBundle)) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(builtBundle)) {
      const std::filesystem::path path = entry.path();
      ok &= expect(path.filename() != "__pycache__",
                   "dev bundle must not include Python bytecode cache directories");
      ok &= expect(path.filename() != "tests",
                   "dev bundle must not include sidecar test directories");
      ok &= expect(path.extension() != ".pyc",
                   "dev bundle must not include Python bytecode files");
      ok &= expect(path.filename() != "torch" && path.filename() != "mlx" &&
                       path.filename() != "models" && path.filename() != "runtime",
                   "dev bundle must not include ML/runtime/model directories");
    }
  }

  std::filesystem::remove_all(output_root, ec);

  return ok ? 0 : 1;
}
