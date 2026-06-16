#!/usr/bin/env python3
"""Report local development environment facts for CorridorKey OpenFX."""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from sidecar.corridorkey_sidecar.logging_config import collect_diagnostic_status
from sidecar.corridorkey_sidecar.model_manager import ModelManager
from sidecar.corridorkey_sidecar.redaction import redact_value
from sidecar.corridorkey_sidecar.runtime_config import RuntimeConfig


DEFAULT_OPENFX_SDK_ROOT = REPO_ROOT / "third_party" / "openfx"

EXPECTED_OPENFX_HEADERS = (
    "include/ofxCore.h",
    "include/ofxImageEffect.h",
    "include/ofxParam.h",
    "include/ofxProperty.h",
    "Support/include/ofxsImageEffect.h",
)


def _display_command_name(command: str) -> str:
    return Path(command).name if os.sep in command else command


def _run_version(command: list[str]) -> str | None:
    try:
        completed = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None

    output = completed.stdout.strip().splitlines()
    return output[0] if output else None


def _find_compiler() -> str:
    preferred = []
    for env_name in ("CXX", "CC"):
        value = os.environ.get(env_name)
        if value:
            preferred.append(value)

    candidates = preferred + ["c++", "clang++", "g++", "cl"]
    seen = set()
    for candidate in candidates:
        if candidate in seen:
            continue
        seen.add(candidate)

        executable = shutil.which(candidate)
        if not executable:
            continue

        if Path(candidate).name.lower() == "cl":
            version = _run_version([candidate])
        else:
            version = _run_version([candidate, "--version"])
        display_name = _display_command_name(candidate)
        if version:
            return f"{display_name} ({version})"
        return f"{display_name} (found, version unavailable)"

    return "not found"


def _print_ofx_plugin_path() -> None:
    raw_value = os.environ.get("OFX_PLUGIN_PATH")
    if not raw_value:
        print("OFX_PLUGIN_PATH: absent (entries=0)")
        return

    entries = [entry for entry in raw_value.split(os.pathsep) if entry]
    print(f"OFX_PLUGIN_PATH: set (entries={len(entries)}, values redacted)")


def _missing_openfx_headers(root: Path) -> list[str]:
    return [
        header
        for header in EXPECTED_OPENFX_HEADERS
        if not (root / header).is_file()
    ]


def _print_openfx_sdk_root_status(label: str, root: Path) -> None:
    missing = _missing_openfx_headers(root)
    print(f"  {label}:")
    if missing:
        print("    status: missing expected headers")
        for header in missing:
            print(f"    missing: {header}")
    else:
        print("    status: found expected headers")


def _print_openfx_sdk_status() -> None:
    override = os.environ.get("OPENFX_SDK_ROOT")

    print("OpenFX SDK:")
    _print_openfx_sdk_root_status("default_root: third_party/openfx", DEFAULT_OPENFX_SDK_ROOT)
    if override:
        print("  OPENFX_SDK_ROOT env: set (value redacted)")
        _print_openfx_sdk_root_status("override_root", Path(override))
    else:
        print("  OPENFX_SDK_ROOT env: absent")


def _print_diagnostic_status() -> None:
    diagnostics = collect_diagnostic_status(
        backend=os.environ.get("CORRIDORKEY_BACKEND", "unknown"),
        model="not_loaded",
        pixel_format=os.environ.get("CORRIDORKEY_PIXEL_FORMAT", "unknown"),
        frame_size=os.environ.get("CORRIDORKEY_FRAME_SIZE", "unknown"),
    )
    print("Diagnostics:")
    print(f"  Plugin version: {diagnostics['plugin_version']}")
    print(f"  Sidecar version: {diagnostics['sidecar_version']}")
    print(
        "  Host: "
        f"{diagnostics['host']['name']} {diagnostics['host']['version']}"
    )
    print(f"  CPU summary: {diagnostics['cpu_summary']}")
    print(f"  GPU summary: {diagnostics['gpu_summary']}")
    print(f"  Backend: {diagnostics['backend']}")
    print(f"  Model: {diagnostics['model']}")
    print(f"  Pixel format: {diagnostics['pixel_format']}")
    print(f"  Frame size: {diagnostics['frame_size']}")
    print(f"  Warning codes: {','.join(diagnostics['warning_codes']) or 'none'}")
    print(f"  Error codes: {','.join(diagnostics['error_codes']) or 'none'}")


def _print_runtime_status() -> None:
    print("Runtime status:")
    config = RuntimeConfig.from_env(REPO_ROOT)
    runtime_status = redact_value(config.status_fields())
    for key in sorted(runtime_status):
        print(f"  {key}: {runtime_status[key]}")

    model_root = os.environ.get("CORRIDORKEY_MODEL_ROOT")
    manager = ModelManager(
        Path(model_root) if model_root else Path.home() / ".corridorkey-ofx" / "models"
    )
    model_status = redact_value(manager.status())
    for key in (
        "model_status",
        "model_source_status",
        "install_status",
        "download_status",
        "backend_compatibility",
        "screen_color",
        "last_error",
    ):
        value = model_status.get(key)
        if value:
            print(f"  {key}: {value}")


def main() -> int:
    print("CorridorKey OpenFX development environment")
    print(f"OS: {platform.system()} {platform.release()} ({platform.machine()})")
    print(f"Python: {platform.python_version()} ({Path(sys.executable).name})")

    cmake_version = _run_version(["cmake", "--version"])
    if cmake_version:
        print(f"CMake: {cmake_version}")
    else:
        print("CMake: not found")

    print(f"Compiler: {_find_compiler()}")
    _print_ofx_plugin_path()
    _print_openfx_sdk_status()
    _print_diagnostic_status()
    _print_runtime_status()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
