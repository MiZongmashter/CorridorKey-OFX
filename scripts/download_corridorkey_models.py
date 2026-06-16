#!/usr/bin/env python3
"""Install CorridorKey green/blue model packages."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import urllib.request


MODELS = {
    "green": {
        "model_id": "corridorkey-backend-green",
        "version": "1.0",
        "filename": "green-model.safetensors",
        "url_env": "CORRIDORKEY_GREEN_MODEL_URL",
        "sha256_env": "CORRIDORKEY_GREEN_MODEL_SHA256",
    },
    "blue": {
        "model_id": "corridorkey-backend-blue",
        "version": "1.0",
        "filename": "blue-model.safetensors",
        "url_env": "CORRIDORKEY_BLUE_MODEL_URL",
        "sha256_env": "CORRIDORKEY_BLUE_MODEL_SHA256",
    },
}


MLX_MODELS = {
    "green": {
        "model_id": "corridorkey-backend-green-mlx",
        "version": "1.0-mlx",
        "filename": "green-mlx-model.safetensors",
        "screen_color": "green",
        "source_filename": "green-mlx-model.safetensors",
        "requires_conversion": False,
    },
    "blue": {
        "model_id": "corridorkey-backend-blue-mlx",
        "version": "1.0-mlx",
        "filename": "blue-mlx-model.safetensors",
        "screen_color": "blue",
        "source_filename": "blue-model.safetensors",
        "requires_conversion": True,
    },
}


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model-dir",
        default=str(Path(".local") / "models" / "corridorkey"),
        help="Destination model root. Defaults to .local/models/corridorkey.",
    )
    parser.add_argument(
        "--color",
        choices=("green", "blue", "all"),
        default="all",
        help="Model color to install. Defaults to all.",
    )
    parser.add_argument(
        "--backend",
        choices=("torch_cpu", "mlx", "all"),
        default="torch_cpu",
        help="Backend model family to install.",
    )
    parser.add_argument(
        "--mlx-checkpoint",
        type=Path,
        help="Local green MLX model path used when --backend is mlx or all.",
    )
    parser.add_argument(
        "--mlx-blue-checkpoint",
        type=Path,
        help="Local blue model path to convert for MLX.",
    )
    args = parser.parse_args(argv)

    model_root = Path(args.model_dir).expanduser()
    if args.backend in ("torch_cpu", "all"):
        colors = ("green", "blue") if args.color == "all" else (args.color,)
        for color in colors:
            install_model(model_root, color, MODELS[color])
    if args.backend in ("mlx", "all"):
        colors = ("green", "blue") if args.color == "all" else (args.color,)
        for color in colors:
            configured = args.mlx_checkpoint if color == "green" else args.mlx_blue_checkpoint
            install_mlx_model(
                model_root,
                find_mlx_checkpoint(color, configured, model_root),
                color=color,
            )
    return 0


def install_model(model_root, color, spec):
    package_dir = model_root / spec["model_id"] / spec["version"]
    package_dir.mkdir(parents=True, exist_ok=True)
    model_path = package_dir / spec["filename"]
    expected_sha256 = os.environ.get(spec["sha256_env"], "")
    if not expected_sha256:
        raise SystemExit(
            f"{spec['sha256_env']} must be set to the approved SHA-256 for {spec['filename']}"
        )
    digest = sha256_file(model_path) if model_path.is_file() else ""
    if digest != expected_sha256:
        url = os.environ.get(spec["url_env"], "")
        if not url:
            raise SystemExit(
                f"{spec['url_env']} must be set to an approved model URL before downloading {spec['filename']}"
            )
        download_file(url, model_path)
        digest = sha256_file(model_path)
        if digest != expected_sha256:
            raise SystemExit(
                f"checksum mismatch for {model_path}: expected {expected_sha256}, got {digest}"
            )
    manifest = {
        "backend_compatibility": ["torch_cpu"],
        "expected_files": [{"path": spec["filename"], "sha256": expected_sha256}],
        "local_path": spec["filename"],
        "model_id": spec["model_id"],
        "screen_color": color,
        "version": spec["version"],
    }
    (package_dir / "model-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"ready: {package_dir}")


def install_mlx_model(model_root, checkpoint_path, *, color="green"):
    source = Path(checkpoint_path).expanduser()
    if not source.is_file():
        raise SystemExit(f"MLX checkpoint not found: {source}")
    spec = MLX_MODELS[color]
    package_dir = model_root / spec["model_id"] / spec["version"]
    package_dir.mkdir(parents=True, exist_ok=True)
    model_path = package_dir / spec["filename"]
    if spec["requires_conversion"]:
        if source.resolve() != model_path.resolve():
            convert_mlx_checkpoint(source, model_path)
    else:
        source_digest = sha256_file(source)
        if not model_path.is_file() or sha256_file(model_path) != source_digest:
            if source.resolve() != model_path.resolve():
                shutil.copy2(source, model_path)
    if not model_path.is_file():
        if source.resolve() != model_path.resolve():
            raise SystemExit(f"MLX checkpoint install failed: {model_path}")
    digest = sha256_file(model_path)
    manifest = {
        "backend_compatibility": ["mlx"],
        "expected_files": [{"path": spec["filename"], "sha256": digest}],
        "local_path": spec["filename"],
        "model_id": spec["model_id"],
        "screen_color": spec["screen_color"],
        "version": spec["version"],
    }
    (package_dir / "model-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"ready: {package_dir}")


def find_mlx_checkpoint(color, configured, model_root=None):
    spec = MLX_MODELS[color]
    candidates = []
    if configured is not None:
        candidates.append(configured)
    env_name = "CORRIDORKEY_MLX_CHECKPOINT" if color == "green" else "CORRIDORKEY_MLX_BLUE_CHECKPOINT"
    env_checkpoint = os.environ.get(env_name)
    if env_checkpoint:
        candidates.append(Path(env_checkpoint))
    if color == "blue" and model_root is not None:
        torch_spec = MODELS["blue"]
        candidates.append(model_root / torch_spec["model_id"] / torch_spec["version"] / torch_spec["filename"])
    for env_name in ("CORRIDORKEY_SOURCE_DIR", "CORRIDORKEY_REPO"):
        configured_root = os.environ.get(env_name)
        if configured_root:
            candidates.extend(
                _mlx_checkpoint_candidates(Path(configured_root).expanduser(), color)
            )
    candidates.extend(
        (
            Path("external")
            / "CorridorKey"
            / "CorridorKey-main"
            / "CorridorKeyModule"
            / "checkpoints"
            / spec["source_filename"],
            Path("external")
            / "CorridorKey-main"
            / "CorridorKeyModule"
            / "checkpoints"
            / spec["source_filename"],
            Path.home()
            / "CorridorKey"
            / "CorridorKey-main"
            / "CorridorKeyModule"
            / "checkpoints"
            / spec["source_filename"],
            Path.home()
            / "Desktop"
            / "CorridorKey-main"
            / "CorridorKeyModule"
            / "checkpoints"
            / spec["source_filename"],
        )
    )
    for candidate in candidates:
        path = Path(candidate).expanduser()
        if path.is_file():
            return path
    raise SystemExit(
        f"MLX {color} checkpoint not found. Pass "
        + (
            "--mlx-checkpoint /path/to/green-mlx-model.safetensors"
            if color == "green"
            else "--mlx-blue-checkpoint /path/to/blue-model.safetensors"
        )
    )


def _mlx_checkpoint_candidates(root, color):
    filename = MLX_MODELS[color]["source_filename"]
    return (
        root / "CorridorKeyModule" / "checkpoints" / filename,
        root
        / "CorridorKey-main"
        / "CorridorKeyModule"
        / "checkpoints"
        / filename,
    )


def convert_mlx_checkpoint(source, destination):
    try:
        from corridorkey_mlx import CorridorKeyMLXEngine
        from corridorkey_mlx.convert.converter import convert_state_dict
        from safetensors.numpy import load_file, save_file
    except ImportError as exc:
        raise SystemExit(
            "MLX blue conversion requires corridorkey_mlx and safetensors in this Python environment"
        ) from exc
    converted, _diagnostics = convert_state_dict(load_file(str(source)))
    destination.parent.mkdir(parents=True, exist_ok=True)
    save_file(converted, str(destination))
    try:
        CorridorKeyMLXEngine(destination, img_size=64, tile_size=None, overlap=0)
    except Exception as exc:
        destination.unlink(missing_ok=True)
        raise SystemExit(f"converted MLX checkpoint failed load validation: {exc}") from exc


def download_file(url, destination):
    part_path = destination.with_suffix(destination.suffix + ".part")
    print(f"downloading: {url}", file=sys.stderr)
    try:
        with urllib.request.urlopen(url) as response, part_path.open("wb") as output:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)
    except Exception:
        part_path.unlink(missing_ok=True)
        raise
    part_path.replace(destination)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


if __name__ == "__main__":
    raise SystemExit(main())
