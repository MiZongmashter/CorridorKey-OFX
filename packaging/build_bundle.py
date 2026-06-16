#!/usr/bin/env python3
"""Build or dry-run the deterministic CorridorKey OpenFX distribution layout."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


BUNDLE_DIRECTORY = "CorridorKey.ofx.bundle"
PLUGIN_BINARY_NAME = "CorridorKey.ofx"
CONTENTS_DIRECTORY = "Contents"
SIDECAR_SOURCE_DIRECTORY = "sidecar"
SIDECAR_RESOURCE_DIRECTORY = "sidecar"
SIDECAR_PACKAGE_FILES = (
    "__init__.py",
    "cache.py",
    "logging_config.py",
    "model_manager.py",
    "model_source_gate.py",
    "protocol.py",
    "redaction.py",
    "runtime_config.py",
    "server.py",
    "support_bundle.py",
    "backends/base.py",
    "backends/torch_backend.py",
)
PACKAGE_ROOT_FILES = (
    "macos.md",
    "uninstall.md",
    "uninstall_corridorkey_ofx.py",
    "THIRD_PARTY_NOTICES.md",
    "DEPENDENCIES.md",
    "CHANGELOG.md",
    "KNOWN_ISSUES.md",
    "offline-model-package-manifest.example.json",
)
DISTRIBUTION_MANIFEST_NAME = "CorridorKey-distribution-manifest.json"
MODEL_MANIFEST_DIRECTORY = "model-manifests"
OFFLINE_MODEL_DIRECTORY = "offline-models"
PACKAGED_RUNTIME_REPO_DIRECTORY = Path("external") / "CorridorKey"
PACKAGED_RUNTIME_ADAPTER_NAME = "corridorkey_ofx_adapter.py"
PACKAGED_RUNTIME_MODEL_DIRECTORY = Path(".local") / "models" / "corridorkey"
PACKAGED_RUNTIME_PYTHON = Path("python") / "bin" / "python3"
PACKAGED_RUNTIME_DIRECTORY = Path("python-runtime")
PACKAGED_RUNTIME_BASE_DIRECTORY = PACKAGED_RUNTIME_DIRECTORY / "base"
PACKAGED_RUNTIME_VENV_DIRECTORY = PACKAGED_RUNTIME_DIRECTORY / "venv"
RUNTIME_ENVIRONMENT_OVERRIDES = (
    "CORRIDORKEY_REPO",
    "CORRIDORKEY_SOURCE_DIR",
    "CORRIDORKEY_MODEL_DIR",
    "CORRIDORKEY_MODEL_ROOT",
    "CORRIDORKEY_BACKEND_FIXTURE_DIR",
    "CORRIDORKEY_DEVICE",
)
VERSION = "0.1.0"


def current_platform_binary_directory() -> str:
    if platform.system() != "Darwin":
        raise RuntimeError("CorridorKey Apple 1.0 packaging requires macOS")
    return "MacOS"


def expected_bundle_paths(output_root: Path) -> list[Path]:
    bundle_root = output_root / BUNDLE_DIRECTORY
    resources_root = bundle_root / CONTENTS_DIRECTORY / "Resources"
    sidecar_package_root = resources_root / SIDECAR_RESOURCE_DIRECTORY / "corridorkey_sidecar"
    packaged_manifest_root = output_root / MODEL_MANIFEST_DIRECTORY
    bundle_manifest_root = resources_root / MODEL_MANIFEST_DIRECTORY
    paths = [
        bundle_root,
        bundle_root / CONTENTS_DIRECTORY,
        bundle_root / CONTENTS_DIRECTORY / "Info.plist",
        resources_root,
        resources_root / SIDECAR_RESOURCE_DIRECTORY,
        bundle_manifest_root,
        bundle_manifest_root / "offline-model-package-manifest.example.json",
        packaged_manifest_root,
        output_root / DISTRIBUTION_MANIFEST_NAME,
    ]
    for sidecar_file in SIDECAR_PACKAGE_FILES:
        paths.append(sidecar_package_root / sidecar_file)
    for package_file in PACKAGE_ROOT_FILES:
        paths.append(output_root / package_file)
    paths.append(output_root / OFFLINE_MODEL_DIRECTORY)
    platform_dir = current_platform_binary_directory()
    paths.append(bundle_root / CONTENTS_DIRECTORY / platform_dir)
    paths.append(bundle_root / CONTENTS_DIRECTORY / platform_dir / PLUGIN_BINARY_NAME)
    return paths


def validate_output_root(output_root: Path) -> None:
    if output_root.name.endswith(".ofx.bundle"):
        raise ValueError("--output must be a distribution directory, not the bundle directory itself")


def print_dry_run(output_root: Path) -> None:
    print("dry-run: no filesystem changes")
    print(f"output_root: {output_root}")
    print(f"bundle_root: {output_root / BUNDLE_DIRECTORY}")
    print(f"distribution_manifest: {output_root / DISTRIBUTION_MANIFEST_NAME}")
    print("expected_layout:")
    for path in expected_bundle_paths(output_root):
        print(f"  {path}")


def _packaging_root() -> Path:
    return Path(__file__).resolve().parent


def _owned_output_paths(output_root: Path) -> list[Path]:
    paths = [
        output_root / BUNDLE_DIRECTORY,
        output_root / MODEL_MANIFEST_DIRECTORY,
        output_root / OFFLINE_MODEL_DIRECTORY,
    ]
    paths.extend(output_root / name for name in PACKAGE_ROOT_FILES)
    paths.append(output_root / DISTRIBUTION_MANIFEST_NAME)
    return paths


def _remove_owned_outputs(output_root: Path) -> None:
    for path in _owned_output_paths(output_root):
        if path.is_dir():
            shutil.rmtree(path)
        elif path.exists():
            path.unlink()


def _copy_package_root_files(output_root: Path) -> None:
    packaging_root = _packaging_root()
    for name in PACKAGE_ROOT_FILES:
        source = packaging_root / name
        if not source.is_file():
            raise FileNotFoundError(f"packaging artifact is missing: {source}")
        target = output_root / name
        shutil.copy2(source, target)


def _copy_sidecar_package(sidecar_source: Path, resources_root: Path) -> None:
    package_source = sidecar_source / "corridorkey_sidecar"
    if not package_source.is_dir():
        raise FileNotFoundError(f"sidecar package directory not found: {package_source}")
    package_target = resources_root / SIDECAR_RESOURCE_DIRECTORY / "corridorkey_sidecar"
    for relative_name in SIDECAR_PACKAGE_FILES:
        source = package_source / relative_name
        if not source.is_file():
            raise FileNotFoundError(f"sidecar package file missing: {source}")
        target = package_target / relative_name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def _copy_offline_manifest_fixture(output_root: Path, resources_root: Path) -> None:
    source = _packaging_root() / "offline-model-package-manifest.example.json"
    target_root = output_root / MODEL_MANIFEST_DIRECTORY
    bundle_target_root = resources_root / MODEL_MANIFEST_DIRECTORY
    target_root.mkdir(parents=True, exist_ok=True)
    bundle_target_root.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target_root / source.name)
    shutil.copy2(source, bundle_target_root / source.name)


def _read_json(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def _safe_segment(value: object, fallback: str) -> str:
    text = str(value or fallback)
    path = Path(text)
    if (
        not text
        or path.is_absolute()
        or len(path.parts) != 1
        or text in (".", "..")
        or "/" in text
        or "\\" in text
    ):
        raise ValueError(f"model manifest value must be a safe path segment: {text!r}")
    return text


def _copy_model_manifests(model_root: Path, output_root: Path, resources_root: Path) -> list[dict]:
    copied: list[dict] = []
    if not model_root.is_dir():
        return copied

    seen_targets: set[str] = set()
    for manifest in sorted(model_root.glob("*/*/model-manifest.json")):
        data = _read_json(manifest)
        model_id = _safe_segment(data.get("model_id"), manifest.parent.parent.name)
        version = _safe_segment(data.get("version"), manifest.parent.name)
        relative = Path("installed") / model_id / version / "model-manifest.json"
        relative_text = relative.as_posix()
        if relative_text in seen_targets:
            raise ValueError(f"duplicate packaged model manifest target: {relative_text}")
        seen_targets.add(relative_text)
        for root in (output_root / MODEL_MANIFEST_DIRECTORY, resources_root / MODEL_MANIFEST_DIRECTORY):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(manifest, target)
        copied.append(
            {
                "model_id": model_id,
                "version": version,
                "packaged_manifest": (Path(MODEL_MANIFEST_DIRECTORY) / relative).as_posix(),
                "sha256": _sha256(manifest),
            }
        )
    return copied


def _copy_offline_model_archive(archive: Path | None, output_root: Path) -> dict:
    if archive is None:
        return {
            "status": "missing",
            "path": "",
            "size": 0,
            "sha256": "",
            "blocker": "offline model package archive was not supplied to the packaging build",
        }
    if not archive.is_file():
        raise FileNotFoundError(f"offline model archive not found: {archive}")

    target_root = output_root / OFFLINE_MODEL_DIRECTORY
    target_root.mkdir(parents=True, exist_ok=True)
    target = target_root / archive.name
    shutil.copy2(archive, target)
    return {
        "status": "ready",
        "path": target.relative_to(output_root).as_posix(),
        "size": target.stat().st_size,
        "sha256": _sha256(target),
        "blocker": "",
    }


def _runtime_ignore(_directory: str, names: list[str]) -> set[str]:
    ignored = {
        ".DS_Store",
        ".git",
        ".github",
        ".idea",
        ".mypy_cache",
        ".pytest_cache",
        ".ruff_cache",
        ".vscode",
        ".venv",
        "__pycache__",
        "checkpoints",
        "ClipsForInference",
        "docs",
        "examples",
        "IgnoredCheckpoints",
        "IgnoredClips",
        "notebooks",
        "Output",
        "samples",
        "test",
        "tests",
    }
    ignored_names = {name for name in names if name in ignored or name.endswith(".pyc")}
    if Path(_directory).name == "gvm_core" and "weights" in names:
        ignored_names.add("weights")
    return ignored_names


def _copy_corridorkey_runtime(
    runtime_root: Path,
    adapter: Path,
    resources_root: Path,
) -> dict:
    if not runtime_root.is_dir():
        raise FileNotFoundError(f"CorridorKey runtime root not found: {runtime_root}")
    if not adapter.is_file():
        raise FileNotFoundError(f"CorridorKey OFX adapter not found: {adapter}")

    target_root = resources_root / PACKAGED_RUNTIME_REPO_DIRECTORY
    target_source = target_root / "CorridorKey-main"
    target_source.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(
        runtime_root,
        target_source,
        dirs_exist_ok=True,
        ignore=_runtime_ignore,
    )
    shutil.copy2(adapter, target_root / adapter.name)
    return {
        "status": "ready",
        "runtime_path": PACKAGED_RUNTIME_REPO_DIRECTORY.as_posix(),
        "source_path": (PACKAGED_RUNTIME_REPO_DIRECTORY / "CorridorKey-main").as_posix(),
        "adapter": (PACKAGED_RUNTIME_REPO_DIRECTORY / adapter.name).as_posix(),
    }


def _validate_runtime_model_file(model_root: Path, relative_path: str) -> Path:
    path = Path(relative_path)
    if (
        not relative_path
        or path.is_absolute()
        or ".." in path.parts
        or urllib_parse_has_scheme(relative_path)
    ):
        raise ValueError(f"model expected file path is unsafe: {relative_path!r}")
    source = model_root / path
    if not source.is_file():
        raise FileNotFoundError(f"model expected file missing: {source}")
    return source


def urllib_parse_has_scheme(value: str) -> bool:
    return ":" in Path(value).parts[0] if Path(value).parts else False


def _copy_runtime_model_assets(model_root: Path, resources_root: Path) -> list[dict]:
    if not model_root.is_dir():
        raise FileNotFoundError(f"runtime model root not found: {model_root}")

    copied: list[dict] = []
    target_root = resources_root / PACKAGED_RUNTIME_MODEL_DIRECTORY
    for manifest in sorted(model_root.glob("*/*/model-manifest.json")):
        data = _read_json(manifest)
        model_id = _safe_segment(data.get("model_id"), manifest.parent.parent.name)
        version = _safe_segment(data.get("version"), manifest.parent.name)
        relative_dir = Path(model_id) / version
        target_dir = target_root / relative_dir
        target_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(manifest, target_dir / "model-manifest.json")
        for item in data.get("expected_files", []):
            rel_path = item.get("path")
            if not isinstance(rel_path, str):
                raise ValueError("model expected file path must be a string")
            source = _validate_runtime_model_file(manifest.parent, rel_path)
            target = target_dir / rel_path
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        copied.append(
            {
                "model_id": model_id,
                "version": version,
                "runtime_manifest": (
                    PACKAGED_RUNTIME_MODEL_DIRECTORY / relative_dir / "model-manifest.json"
                ).as_posix(),
            }
        )
    return copied


def _packaged_runtime_model_assets(packaged_resources: Path) -> list[dict]:
    model_root = packaged_resources / PACKAGED_RUNTIME_MODEL_DIRECTORY
    if not model_root.is_dir():
        raise FileNotFoundError(f"packaged runtime model root not found: {model_root}")

    packaged: list[dict] = []
    for manifest in sorted(model_root.glob("*/*/model-manifest.json")):
        data = _read_json(manifest)
        model_id = _safe_segment(data.get("model_id"), manifest.parent.parent.name)
        version = _safe_segment(data.get("version"), manifest.parent.name)
        for item in data.get("expected_files", []):
            rel_path = item.get("path")
            if not isinstance(rel_path, str):
                raise ValueError("model expected file path must be a string")
            _validate_runtime_model_file(manifest.parent, rel_path)
        packaged.append(
            {
                "model_id": model_id,
                "version": version,
                "runtime_manifest": (
                    PACKAGED_RUNTIME_MODEL_DIRECTORY
                    / model_id
                    / version
                    / "model-manifest.json"
                ).as_posix(),
            }
        )
    if not packaged:
        raise FileNotFoundError(f"packaged runtime model manifests not found under: {model_root}")
    return packaged


def _packaged_runtime_python_status(packaged_resources: Path) -> dict:
    launcher = packaged_resources / PACKAGED_RUNTIME_PYTHON
    if not launcher.is_file():
        raise FileNotFoundError(f"packaged runtime Python launcher not found: {launcher}")
    base_root = packaged_resources / PACKAGED_RUNTIME_BASE_DIRECTORY
    if not base_root.is_dir():
        raise FileNotFoundError(f"packaged runtime Python base not found: {base_root}")
    interpreters = sorted((base_root / "bin").glob("python*"))
    interpreter = next((path for path in interpreters if path.is_file()), None)
    if interpreter is None:
        raise FileNotFoundError(f"packaged runtime Python interpreter not found under: {base_root / 'bin'}")
    site_packages = sorted(
        (packaged_resources / PACKAGED_RUNTIME_VENV_DIRECTORY).glob(
            "lib/python*/site-packages"
        )
    )
    site_packages = [path for path in site_packages if path.is_dir()]
    if not site_packages:
        raise FileNotFoundError(
            "packaged runtime Python site-packages not found under: "
            f"{packaged_resources / PACKAGED_RUNTIME_VENV_DIRECTORY}"
        )
    return {
        "status": "standalone_ready",
        "path": PACKAGED_RUNTIME_PYTHON.as_posix(),
        "home": PACKAGED_RUNTIME_BASE_DIRECTORY.as_posix(),
        "interpreter": interpreter.relative_to(packaged_resources).as_posix(),
        "site_packages": [
            path.relative_to(packaged_resources).as_posix() for path in site_packages
        ],
        "source": "packaged_runtime_resources",
    }


def _copy_packaged_runtime_resources(packaged_resources: Path, resources_root: Path) -> dict:
    if not packaged_resources.is_dir():
        raise FileNotFoundError(f"packaged runtime resources root not found: {packaged_resources}")

    runtime_repo_root = packaged_resources / PACKAGED_RUNTIME_REPO_DIRECTORY
    runtime_source = runtime_repo_root / "CorridorKey-main"
    adapter = runtime_repo_root / PACKAGED_RUNTIME_ADAPTER_NAME
    if not runtime_source.is_dir():
        raise FileNotFoundError(f"packaged CorridorKey runtime source not found: {runtime_source}")
    if not adapter.is_file():
        raise FileNotFoundError(f"packaged CorridorKey adapter not found: {adapter}")

    runtime_models = _packaged_runtime_model_assets(packaged_resources)
    runtime_python_status = _packaged_runtime_python_status(packaged_resources)
    for relative in (
        PACKAGED_RUNTIME_REPO_DIRECTORY.parts[0],
        PACKAGED_RUNTIME_MODEL_DIRECTORY.parts[0],
        PACKAGED_RUNTIME_DIRECTORY,
        Path("python"),
    ):
        source = packaged_resources / relative
        if not source.exists():
            raise FileNotFoundError(f"packaged runtime resource not found: {source}")
        ignore = (
            _runtime_ignore
            if str(relative) == PACKAGED_RUNTIME_REPO_DIRECTORY.parts[0]
            else _runtime_dependency_ignore
        )
        _copy_runtime_tree(source, resources_root / relative, ignore)

    return {
        "status": "ready",
        "repo_status": "ready",
        "model_status": "ready",
        "python_status": runtime_python_status["status"],
        "repo": {
            "status": "ready",
            "runtime_path": PACKAGED_RUNTIME_REPO_DIRECTORY.as_posix(),
            "source_path": (PACKAGED_RUNTIME_REPO_DIRECTORY / "CorridorKey-main").as_posix(),
            "adapter": (PACKAGED_RUNTIME_REPO_DIRECTORY / PACKAGED_RUNTIME_ADAPTER_NAME).as_posix(),
            "source": "packaged_runtime_resources",
        },
        "models": runtime_models,
        "python": runtime_python_status,
    }


def _runtime_dependency_ignore(_directory: str, names: list[str]) -> set[str]:
    ignored = {".DS_Store", "__pycache__"}
    return {name for name in names if name in ignored or name.endswith(".pyc")}


def _read_pyvenv_cfg(venv_root: Path) -> dict[str, str]:
    config_path = venv_root / "pyvenv.cfg"
    if not config_path.is_file():
        raise ValueError(f"runtime python must be inside a virtualenv with pyvenv.cfg: {venv_root}")
    config: dict[str, str] = {}
    for line in config_path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        config[key.strip()] = value.strip()
    return config


def _runtime_python_layout(runtime_python: Path) -> dict[str, object]:
    if not runtime_python.is_file():
        raise FileNotFoundError(f"runtime python executable not found: {runtime_python}")

    venv_root = runtime_python.parent.parent
    config = _read_pyvenv_cfg(venv_root)
    home_text = config.get("home")
    if not home_text:
        raise ValueError(f"runtime python pyvenv.cfg is missing home: {venv_root / 'pyvenv.cfg'}")
    home = Path(home_text).expanduser()
    if not home.is_absolute():
        home = (venv_root / home).resolve()
    if not home.is_dir():
        raise FileNotFoundError(f"runtime python home directory not found: {home}")

    base_root = home.parent if home.name in ("bin", "Scripts") else home
    if not base_root.is_dir():
        raise FileNotFoundError(f"runtime python base directory not found: {base_root}")

    version_info = config.get("version_info", "")
    version = ".".join(version_info.split(".")[:2]) if version_info else ""
    candidates = []
    resolved_python = runtime_python.resolve()
    if resolved_python.is_file() and _is_relative_to(resolved_python, base_root):
        candidates.append(resolved_python)
    if version:
        candidates.append(home / f"python{version}")
    candidates.extend((home / "python3", home / "python", runtime_python))
    interpreter = next((path for path in candidates if path.is_file()), None)
    if interpreter is None:
        raise FileNotFoundError(f"runtime python base interpreter not found under: {home}")

    site_packages = sorted(venv_root.glob("lib/python*/site-packages"))
    site_packages = [path for path in site_packages if path.is_dir()]
    if not site_packages:
        raise FileNotFoundError(f"runtime python site-packages not found under: {venv_root}")

    return {
        "base_root": base_root,
        "interpreter": interpreter,
        "site_packages": site_packages,
        "venv_root": venv_root,
    }


def _is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
    except ValueError:
        return False
    return True


def _copy_runtime_tree(source: Path, target: Path, ignore=_runtime_dependency_ignore) -> None:
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(
        source,
        target,
        symlinks=False,
        ignore=ignore,
    )


def _remove_runtime_path(path: Path) -> bool:
    if path.is_dir():
        shutil.rmtree(path)
        return True
    if path.exists():
        path.unlink()
        return True
    return False


def _redact_runtime_text(text: str, source_paths: list[Path]) -> str:
    redacted = text
    for source_path in source_paths:
        source_text = str(source_path)
        redacted = redacted.replace(source_text, "<packaged-runtime-source>")
        redacted = redacted.replace(
            "file://" + source_text,
            "file://<packaged-runtime-source>",
        )
    redacted = re.sub(
        r"file:///(?:private/var|var/folders|Users)/[^\"'\s,;)]+",
        "file://<packaged-runtime-source>",
        redacted,
    )
    redacted = re.sub(
        r"file:///[^\"'\s,;)]+",
        "file://<packaged-runtime-source>",
        redacted,
    )
    redacted = re.sub(
        r"/(?:private/var|var/folders|Users)/[^\"'\s,;)]+",
        "<packaged-runtime-source>",
        redacted,
    )
    return redacted


def _sanitize_runtime_text_file(path: Path, source_paths: list[Path]) -> bool:
    try:
        original = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return False
    redacted = _redact_runtime_text(original, source_paths)
    if redacted == original:
        return False
    path.write_text(redacted, encoding="utf-8")
    return True


def _prune_packaged_python_base(base_root: Path, source_paths: list[Path]) -> dict:
    removed: list[str] = []
    sanitized: list[str] = []

    for relative in ("include", "share", "lib/pkgconfig"):
        path = base_root / relative
        if _remove_runtime_path(path):
            removed.append(relative)

    bin_root = base_root / "bin"
    for pattern in ("pip*", "idle*", "pydoc*", "python*-config"):
        for path in sorted(bin_root.glob(pattern)):
            if _remove_runtime_path(path):
                removed.append(path.relative_to(base_root).as_posix())

    lib_root = base_root / "lib"
    for pattern in (
        "tcl*",
        "tk*",
        "itcl*",
        "thread*",
        "libtcl*.dylib",
        "libtk*.dylib",
        "libtcl*tk*.dylib",
    ):
        for path in sorted(lib_root.glob(pattern)):
            if _remove_runtime_path(path):
                removed.append(path.relative_to(base_root).as_posix())

    for stdlib in sorted(lib_root.glob("python*")):
        if not stdlib.is_dir():
            continue
        for pattern in ("config-*",):
            for path in sorted(stdlib.glob(pattern)):
                if _remove_runtime_path(path):
                    removed.append(path.relative_to(base_root).as_posix())
        for relative in ("idlelib", "tkinter", "turtledemo", "ensurepip"):
            path = stdlib / relative
            if _remove_runtime_path(path):
                removed.append(path.relative_to(base_root).as_posix())
        site_packages = stdlib / "site-packages"
        if site_packages.is_dir():
            for pattern in ("pip", "pip-*"):
                for path in sorted(site_packages.glob(pattern)):
                    if _remove_runtime_path(path):
                        removed.append(path.relative_to(base_root).as_posix())
            removed.extend(
                f"lib/{stdlib.name}/site-packages/{path}"
                for path in _sanitize_direct_url_metadata(site_packages)
            )
        for path in sorted(stdlib.glob("_sysconfigdata*.py")):
            if _sanitize_runtime_text_file(path, source_paths):
                sanitized.append(path.relative_to(base_root).as_posix())

    return {
        "removed": sorted(set(removed)),
        "sanitized": sorted(set(sanitized)),
    }


def _rewrite_macos_python_install_names(base_root: Path) -> list[str]:
    if platform.system() != "Darwin":
        return []
    install_name_tool = shutil.which("install_name_tool")
    if install_name_tool is None:
        return []

    rewritten: list[str] = []
    for dylib in sorted((base_root / "lib").glob("libpython*.dylib")):
        subprocess.run(
            [
                install_name_tool,
                "-id",
                f"@rpath/{dylib.name}",
                str(dylib),
            ],
            check=True,
        )
        rewritten.append(dylib.relative_to(base_root).as_posix())
    return rewritten


def _unsafe_pth_line(line: str) -> bool:
    stripped = line.strip()
    if not stripped or stripped.startswith("#"):
        return False
    if stripped.startswith("import "):
        return _contains_local_absolute_path(stripped)
    if urllib_parse_has_scheme(stripped):
        return True
    return Path(stripped).is_absolute() or re.match(r"^[A-Za-z]:[\\/]", stripped) is not None


def _contains_local_absolute_path(text: str) -> bool:
    normalized = text.replace("\\", "/")
    return (
        "/Users/" in normalized
        or "/private/var/" in normalized
        or "/var/folders/" in normalized
        or "file:/" in normalized
    )


def _sanitize_packaged_pth_files(
    site_packages: Path,
    blocked_modules: set[str] | None = None,
) -> list[str]:
    blocked = blocked_modules or set()
    sanitized: list[str] = []
    for path in sorted(site_packages.glob("*.pth")):
        lines = path.read_text(encoding="utf-8").splitlines()
        kept = [
            line
            for line in lines
            if not _unsafe_pth_line(line)
            and not any(module in line for module in blocked)
        ]
        if kept == lines:
            continue
        sanitized.append(path.name)
        if kept:
            path.write_text("\n".join(kept) + "\n", encoding="utf-8")
        else:
            path.unlink()
    return sanitized


def _remove_unsafe_editable_files(site_packages: Path) -> dict:
    removed_files: list[str] = []
    removed_modules: set[str] = set()
    patterns = (
        "__editable__*.py",
        "__editable___*_finder.py",
        "*editable*finder*.py",
    )
    candidates: set[Path] = set()
    for pattern in patterns:
        candidates.update(site_packages.glob(pattern))
    for path in sorted(candidates):
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if not _contains_local_absolute_path(text):
            continue
        path.unlink()
        removed_files.append(path.name)
        removed_modules.add(path.stem)
    return {
        "files": removed_files,
        "modules": removed_modules,
    }


def _sanitize_direct_url_metadata(site_packages: Path) -> list[str]:
    removed: list[str] = []
    for path in sorted(site_packages.glob("*.dist-info/direct_url.json")):
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            continue
        if not _contains_local_absolute_path(text):
            continue
        path.unlink()
        removed.append(path.relative_to(site_packages).as_posix())
    return removed


def _sanitize_runtime_build_config_metadata(
    site_packages: Path,
    source_paths: list[Path],
) -> list[str]:
    sanitized: list[str] = []
    candidates: set[Path] = set()
    for pattern in ("**/__config__.py", "**/_build_config.py"):
        candidates.update(site_packages.glob(pattern))
    for path in sorted(candidates):
        if _sanitize_runtime_text_file(path, source_paths):
            sanitized.append(path.relative_to(site_packages).as_posix())
    return sanitized


def _write_runtime_python_launcher(runtime_python: Path, resources_root: Path) -> dict:
    layout = _runtime_python_layout(runtime_python)
    base_root = layout["base_root"]
    interpreter = layout["interpreter"]
    site_packages = layout["site_packages"]
    venv_root = layout["venv_root"]

    if not isinstance(base_root, Path) or not isinstance(interpreter, Path) or not isinstance(venv_root, Path):
        raise TypeError("runtime python layout helper returned invalid paths")

    target_base = resources_root / PACKAGED_RUNTIME_BASE_DIRECTORY
    _copy_runtime_tree(base_root, target_base)
    base_prune = _prune_packaged_python_base(
        target_base,
        [base_root, venv_root, runtime_python],
    )
    rewritten_install_names = _rewrite_macos_python_install_names(target_base)

    target_site_packages: list[Path] = []
    sanitized_pth_files: list[str] = []
    sanitized_runtime_metadata_files: list[str] = []
    removed_metadata: list[str] = []
    runtime_source_paths = [base_root, venv_root, runtime_python]
    for source_site_packages in site_packages:
        if not isinstance(source_site_packages, Path):
            raise TypeError("runtime python layout helper returned invalid site-packages")
        relative = source_site_packages.relative_to(venv_root)
        target = resources_root / PACKAGED_RUNTIME_VENV_DIRECTORY / relative
        _copy_runtime_tree(source_site_packages, target)
        removed_editable = _remove_unsafe_editable_files(target)
        sanitized_pth_files.extend(
            _sanitize_packaged_pth_files(target, removed_editable["modules"])
        )
        sanitized_runtime_metadata_files.extend(
            _sanitize_runtime_build_config_metadata(target, runtime_source_paths)
        )
        removed_metadata.extend(_sanitize_direct_url_metadata(target))
        removed_metadata.extend(removed_editable["files"])
        target_site_packages.append(target)

    interpreter_relative = PACKAGED_RUNTIME_BASE_DIRECTORY / interpreter.relative_to(base_root)
    site_package_relatives = [
        path.relative_to(resources_root).as_posix() for path in target_site_packages
    ]
    target = resources_root / PACKAGED_RUNTIME_PYTHON
    target.parent.mkdir(parents=True, exist_ok=True)
    path_entries = ":".join(f"$RESOURCES_DIR/{item}" for item in site_package_relatives)
    unset_overrides = "".join(
        f"unset {name}\n" for name in RUNTIME_ENVIRONMENT_OVERRIDES
    )
    scrub_corridorkey_env = (
        "for CK_ENV_NAME in $(env | sed -n "
        "'s/^\\(CORRIDORKEY_[A-Za-z0-9_]*\\)=.*/\\1/p'); do\n"
        "  case \"$CK_ENV_NAME\" in\n"
        "    CORRIDORKEY_TEST_*) ;;\n"
        "    CORRIDORKEY_*) unset \"$CK_ENV_NAME\" ;;\n"
        "  esac\n"
        "done\n"
    )
    target.write_text(
        "#!/bin/sh\n"
        "SCRIPT_DIR=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\n"
        "RESOURCES_DIR=$(CDPATH= cd -- \"$SCRIPT_DIR/../..\" && pwd)\n"
        f"{unset_overrides}"
        f"{scrub_corridorkey_env}"
        "cd \"$RESOURCES_DIR\"\n"
        f"export PYTHONHOME=\"$RESOURCES_DIR/{PACKAGED_RUNTIME_BASE_DIRECTORY.as_posix()}\"\n"
        "export PYTHONNOUSERSITE=1\n"
        "export PYTHONDONTWRITEBYTECODE=1\n"
        f"export PYTHONPATH=\"{path_entries}\"\n"
        f"exec \"$RESOURCES_DIR/{interpreter_relative.as_posix()}\" \"$@\"\n",
        encoding="utf-8",
    )
    target.chmod(target.stat().st_mode | 0o755)
    return {
        "status": "standalone_ready",
        "path": target.relative_to(resources_root).as_posix(),
        "home": PACKAGED_RUNTIME_BASE_DIRECTORY.as_posix(),
        "interpreter": interpreter_relative.as_posix(),
        "site_packages": site_package_relatives,
        "pruned_base_runtime_paths": base_prune["removed"],
        "sanitized_base_runtime_files": base_prune["sanitized"],
        "rewritten_macos_install_names": rewritten_install_names,
        "sanitized_pth_files": sanitized_pth_files,
        "sanitized_runtime_metadata_files": sorted(set(sanitized_runtime_metadata_files)),
        "removed_local_metadata": removed_metadata,
    }


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _tree_sha256(root: Path, exclude_relative: set[str] | None = None) -> str:
    exclude = exclude_relative or set()
    digest = hashlib.sha256()
    files = sorted(
        (path for path in root.rglob("*") if path.is_file()),
        key=lambda item: item.relative_to(root).as_posix(),
    )
    for path in files:
        rel = path.relative_to(root).as_posix()
        if rel in exclude:
            continue
        digest.update(rel.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(path.stat().st_size).encode("ascii"))
        digest.update(b"\0")
        digest.update(_sha256(path).encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def _iter_package_files(output_root: Path) -> list[Path]:
    roots = [
        output_root / BUNDLE_DIRECTORY,
        output_root / MODEL_MANIFEST_DIRECTORY,
        output_root / OFFLINE_MODEL_DIRECTORY,
    ]
    roots.extend(output_root / name for name in PACKAGE_ROOT_FILES)
    files: list[Path] = []
    for root in roots:
        if root.is_file():
            files.append(root)
            continue
        if root.is_dir():
            files.extend(path for path in root.rglob("*") if path.is_file())
    return sorted(files, key=lambda item: item.relative_to(output_root).as_posix())


def _distribution_manifest(
    output_root: Path,
    plugin_binary: Path,
    sidecar_source: Path,
    model_root: Path,
    model_manifests: list[dict],
    offline_model_package: dict,
    runtime_package: dict,
) -> dict:
    files = []
    for path in _iter_package_files(output_root):
        rel = path.relative_to(output_root).as_posix()
        files.append(
            {
                "path": rel,
                "size": path.stat().st_size,
                "sha256": _sha256(path),
            }
        )

    packaged_plugin = (
        output_root
        / BUNDLE_DIRECTORY
        / CONTENTS_DIRECTORY
        / current_platform_binary_directory()
        / PLUGIN_BINARY_NAME
    )
    plugin_binary_rel = packaged_plugin.relative_to(output_root).as_posix()
    artifact_tree_file_count = sum(
        1 for path in (output_root / BUNDLE_DIRECTORY).rglob("*") if path.is_file()
    )

    package_notes = [
        "manual install/uninstall status is recorded outside the source repository",
        "host gate summaries are published under docs/public",
        "GPU unavailable/OOM raw evidence is not included in the source repository",
    ]
    if runtime_package.get("python_status") != "standalone_ready":
        package_notes.append(
            "packaged Python/runtime dependency isolation is not evaluated by this source packager"
        )
    if offline_model_package["status"] != "ready":
        package_notes.append("offline model package archive is not linked to this distribution manifest")
    model_manifest_status = "ready" if model_manifests else "missing"
    if model_manifest_status != "ready":
        package_notes.append("no configured model manifests were available to package")
    model_source_status = (
        "ready" if model_manifests and offline_model_package["status"] == "ready" else "blocked"
    )
    if model_source_status == "ready":
        model_source_blocker = ""
    else:
        model_source_blocker = (
            "local model manifests were copied, but the offline model package archive is not linked "
            "to this distribution manifest"
            if model_manifests
            else "no configured model manifests were available to package"
        )

    runtime_ready = runtime_package.get("status") == "ready"
    full_model_runtime_ready = (
        model_source_status == "ready"
        and runtime_ready
        and runtime_package.get("repo_status") == "ready"
        and runtime_package.get("model_status") == "ready"
        and runtime_package.get("python_status") == "standalone_ready"
    )
    if not full_model_runtime_ready:
        package_notes.append("full model/runtime readiness is not evaluated by the packager")

    return {
        "schema_version": 1,
        "package_name": "CorridorKey OFX",
        "version": VERSION,
        "generated_at_utc": datetime.now(timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z"),
        "artifact_path": BUNDLE_DIRECTORY,
        "artifact_tree_sha256": _tree_sha256(output_root / BUNDLE_DIRECTORY),
        "artifact_tree_file_count": artifact_tree_file_count,
        "distribution_tree_sha256": _tree_sha256(
            output_root, {DISTRIBUTION_MANIFEST_NAME}
        ),
        "distribution_tree_file_count": len(files),
        "distribution_file_count_including_manifest": len(files) + 1,
        "tree_sha256_algorithm": (
            "SHA-256 over sorted file records path\\0size\\0file_sha256\\n; "
            "distribution tree excludes CorridorKey-distribution-manifest.json "
            "to avoid self-reference"
        ),
        "distribution_manifest": DISTRIBUTION_MANIFEST_NAME,
        "plugin_binary_path": plugin_binary_rel,
        "plugin_binary_sha256": _sha256(packaged_plugin),
        "plugin_binary_size": packaged_plugin.stat().st_size,
        "build_configuration": {
            "platform": platform.system(),
            "platform_binary_directory": current_platform_binary_directory(),
            "plugin_binary": plugin_binary_rel,
            "sidecar_source": f"{BUNDLE_DIRECTORY}/{CONTENTS_DIRECTORY}/Resources/{SIDECAR_RESOURCE_DIRECTORY}",
            "model_root": MODEL_MANIFEST_DIRECTORY,
            "model_manifest_status": model_manifest_status,
            "model_source_manifest_status": "ready" if model_manifests else "missing",
            "model_manifest_count": len(model_manifests),
            "model_source_status": model_source_status,
            "model_source_blocker": model_source_blocker,
            "offline_model_package_status": offline_model_package["status"],
            "offline_model_package_path": offline_model_package["path"],
            "offline_model_package_size": offline_model_package["size"],
            "offline_model_package_sha256": offline_model_package["sha256"],
            "runtime_readiness_status": "packaged_runtime_assets_present"
            if runtime_ready
            else "not_evaluated_by_packager",
            "packaged_runtime_status": runtime_package.get(
                "python_status", "blocked_system_python_probe_only"
            ),
            "packaged_runtime_repo_status": runtime_package.get("repo_status", "missing"),
            "packaged_runtime_model_status": runtime_package.get("model_status", "missing"),
            "packaged_runtime_python_status": runtime_package.get("python_status", "missing"),
            "signing_status": "not_required_for_current_scope",
            "release_classification": "macOS source distribution package",
        },
        "model_manifests": model_manifests,
        "offline_model_package": offline_model_package,
        "runtime_package": runtime_package,
        "package_notes": package_notes,
        "files": files,
    }


def _write_distribution_manifest(
    output_root: Path,
    plugin_binary: Path,
    sidecar_source: Path,
    model_root: Path,
    model_manifests: list[dict],
    offline_model_package: dict,
    runtime_package: dict,
) -> Path:
    manifest = _distribution_manifest(
        output_root,
        plugin_binary,
        sidecar_source,
        model_root,
        model_manifests,
        offline_model_package,
        runtime_package,
    )
    path = output_root / DISTRIBUTION_MANIFEST_NAME
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def build_bundle(
    output_root: Path,
    plugin_binary: Path,
    sidecar_source: Path,
    model_root: Path,
    offline_model_archive: Path | None = None,
    *,
    corridorkey_runtime_root: Path | None = None,
    corridorkey_adapter: Path | None = None,
    runtime_python: Path | None = None,
    include_runtime_assets: bool = False,
    packaged_runtime_resources: Path | None = None,
) -> None:
    if not plugin_binary.is_file():
        raise FileNotFoundError(f"plugin binary not found: {plugin_binary}")
    if not sidecar_source.is_dir():
        raise FileNotFoundError(f"sidecar source directory not found: {sidecar_source}")

    output_root.mkdir(parents=True, exist_ok=True)
    _remove_owned_outputs(output_root)
    bundle_root = output_root / BUNDLE_DIRECTORY
    contents_root = bundle_root / CONTENTS_DIRECTORY
    resources_root = contents_root / "Resources"
    resources_root.mkdir(parents=True, exist_ok=True)
    (contents_root / "Info.plist").write_text(
        """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>CorridorKey.ofx</string>
  <key>CFBundleIdentifier</key>
  <string>com.corridorkey.openfx</string>
  <key>CFBundleName</key>
  <string>CorridorKey OFX</string>
  <key>CFBundlePackageType</key>
  <string>BNDL</string>
  <key>CFBundleVersion</key>
  <string>{version}</string>
</dict>
</plist>
""".format(version=VERSION),
        encoding="utf-8",
    )

    platform_dir = current_platform_binary_directory()
    binary_dir = contents_root / platform_dir
    binary_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(plugin_binary, binary_dir / PLUGIN_BINARY_NAME)
    _copy_sidecar_package(sidecar_source, resources_root)
    _copy_package_root_files(output_root)
    _copy_offline_manifest_fixture(output_root, resources_root)
    model_manifests = _copy_model_manifests(model_root, output_root, resources_root)
    offline_model_package = _copy_offline_model_archive(offline_model_archive, output_root)
    runtime_package = {
        "status": "missing",
        "repo_status": "missing",
        "model_status": "missing",
        "python_status": "missing",
    }
    if include_runtime_assets and packaged_runtime_resources is not None:
        raise ValueError("--include-runtime-assets and --packaged-runtime-resources are mutually exclusive")
    if packaged_runtime_resources is not None:
        runtime_package = _copy_packaged_runtime_resources(packaged_runtime_resources, resources_root)
    elif include_runtime_assets:
        runtime_root = corridorkey_runtime_root or Path("external") / "CorridorKey-main"
        adapter = corridorkey_adapter or Path("external") / "CorridorKey" / "corridorkey_ofx_adapter.py"
        if runtime_python is None:
            raise ValueError("--runtime-python is required with --include-runtime-assets")
        runtime_repo = _copy_corridorkey_runtime(runtime_root, adapter, resources_root)
        runtime_models = _copy_runtime_model_assets(model_root, resources_root)
        runtime_python_status = _write_runtime_python_launcher(runtime_python, resources_root)
        runtime_package = {
            "status": "ready"
            if runtime_models and runtime_python_status["status"] == "standalone_ready"
            else "missing",
            "repo_status": runtime_repo["status"],
            "model_status": "ready" if runtime_models else "missing",
            "python_status": runtime_python_status["status"],
            "repo": runtime_repo,
            "models": runtime_models,
            "python": runtime_python_status,
        }
    distribution_manifest = _write_distribution_manifest(
        output_root,
        plugin_binary,
        sidecar_source,
        model_root,
        model_manifests,
        offline_model_package,
        runtime_package,
    )

    print(f"bundle_root: {bundle_root}")
    print(f"platform_binary: {binary_dir / PLUGIN_BINARY_NAME}")
    print(f"sidecar_resources: {resources_root / SIDECAR_RESOURCE_DIRECTORY}")
    print(f"distribution_manifest: {distribution_manifest}")
    print(f"model_manifest_count: {len(model_manifests)}")
    print(f"offline_model_package_status: {offline_model_package['status']}")
    print(f"runtime_package_status: {runtime_package['status']}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="Distribution output directory")
    parser.add_argument("--dry-run", action="store_true", help="Print and validate layout only")
    parser.add_argument("--plugin-binary", type=Path, help="Compiled plugin binary")
    parser.add_argument(
        "--sidecar-source",
        type=Path,
        default=Path(SIDECAR_SOURCE_DIRECTORY),
        help="Python sidecar package root copied into Contents/Resources",
    )
    parser.add_argument(
        "--model-root",
        type=Path,
        default=Path(".local") / "models" / "corridorkey",
        help="Optional local model root; model-manifest.json files are copied without weights",
    )
    parser.add_argument(
        "--offline-model-archive",
        type=Path,
        help="Offline model package archive copied into the distribution and manifest",
    )
    parser.add_argument(
        "--include-runtime-assets",
        action="store_true",
        help="Copy local runtime source, model files, and a Python launcher into the bundle",
    )
    parser.add_argument(
        "--corridorkey-runtime-root",
        type=Path,
        help="Local CorridorKey source checkout copied under Resources/external/CorridorKey/CorridorKey-main",
    )
    parser.add_argument(
        "--corridorkey-adapter",
        type=Path,
        default=Path("external") / "CorridorKey" / "corridorkey_ofx_adapter.py",
        help="OFX adapter copied to Resources/external/CorridorKey",
    )
    parser.add_argument(
        "--runtime-python",
        type=Path,
        help="Python executable used by the bundled runtime launcher",
    )
    parser.add_argument(
        "--packaged-runtime-resources",
        type=Path,
        help=(
            "Existing packaged Contents/Resources directory whose standalone runtime "
            "assets are copied into this distribution"
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    output_root = args.output
    validate_output_root(output_root)

    if args.dry_run:
        print_dry_run(output_root)
        return 0

    if args.plugin_binary is None:
        default_binary = Path("build") / PLUGIN_BINARY_NAME
        if default_binary.is_file():
            args.plugin_binary = default_binary
        else:
            raise SystemExit("--plugin-binary is required unless build/CorridorKey.ofx exists")

    build_bundle(
        output_root,
        args.plugin_binary,
        args.sidecar_source,
        args.model_root,
        args.offline_model_archive,
        corridorkey_runtime_root=args.corridorkey_runtime_root,
        corridorkey_adapter=args.corridorkey_adapter,
        runtime_python=args.runtime_python,
        include_runtime_assets=args.include_runtime_assets,
        packaged_runtime_resources=args.packaged_runtime_resources,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
