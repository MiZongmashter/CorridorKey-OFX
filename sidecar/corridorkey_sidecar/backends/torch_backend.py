"""Torch CPU backend adapter for user-supplied CorridorKey runtimes."""

import importlib.util
import json
import platform
from pathlib import Path
import sys
import time

from .base import (
    BlockedBackend,
    blocked_response,
    clamp01,
    ensure_float_sequence,
    input_digest,
    read_infer_frame_blobs,
    write_backend_outputs,
)
from ..protocol import (
    COMMAND_INFER,
    COMMAND_WARMUP,
    ProtocolError,
    make_error,
    make_response,
)
from ..runtime_config import RuntimeConfig


CPU_BACKENDS = frozenset(("auto", "torch_cpu"))
GPU_BACKENDS = frozenset(("torch_cuda", "torch_mps"))
MLX_BACKEND = "mlx"
ADAPTER_NAME = "corridorkey_ofx_adapter.py"


def handle_backend_request(request, model_manager=None):
    command = request["command"]
    if command not in (COMMAND_INFER, COMMAND_WARMUP):
        return None
    payload = request["payload"]
    backend = payload.get("backend", "")
    if backend == "stub":
        return None

    backend_adapter = TorchCpuBackend(RuntimeConfig.from_env(), model_manager)
    if command == COMMAND_WARMUP:
        return backend_adapter.warmup(request["request_id"], payload)
    return backend_adapter.infer(request["request_id"], payload)


class TorchCpuBackend:
    def __init__(self, config, model_manager=None):
        self.config = config
        self.model_manager = model_manager

    def warmup(self, request_id, payload):
        try:
            blocked = self._blocked_before_runtime(payload)
            if blocked is not None:
                return blocked_response(
                    request_id, payload, blocked.message, blocked.fields, blocked.backend_status
                )
            backend = self._effective_backend(payload)
            model = self._select_model(payload, backend)
            return make_response(
                request_id,
                True,
                {
                    **self._ready_payload(payload, model, backend),
                    "warmup": "ready",
                    "quality": payload["quality"],
                    "timing_warmup_ms": "0",
                },
                None,
            )
        except BlockedBackend as exc:
            return blocked_response(
                request_id, payload, exc.message, exc.fields, exc.backend_status
            )

    def infer(self, request_id, payload):
        start = time.monotonic()
        try:
            blocked = self._blocked_before_runtime(payload)
            if blocked is not None:
                return blocked_response(
                    request_id, payload, blocked.message, blocked.fields, blocked.backend_status
                )

            backend = self._effective_backend(payload)
            model = self._select_model(payload, backend)
            adapter_function = "infer_mlx" if backend == MLX_BACKEND else "infer_cpu"
            adapter = self._load_adapter(adapter_function)
            bounds, pixel_count, source, alpha_hint = read_infer_frame_blobs(payload)
            adapter_request = {
                "bounds": bounds,
                "pixel_count": pixel_count,
                "source_rgba": list(source["values"]),
                "alpha_hint": list(alpha_hint["values"]),
                "screen_color": model["screen_color"],
                "quality": payload["quality"],
                "despill_strength": payload["despill_strength"],
                "model": model,
                "model_dir": str(self.config.model_dir),
                "corridorkey_repo": str(self.config.corridorkey_repo),
                "device": MLX_BACKEND if backend == MLX_BACKEND else "cpu",
            }
            if backend == MLX_BACKEND:
                adapter_request["input_color_space"] = payload["input_color_space"]
            try:
                adapter_result = getattr(adapter, adapter_function)(adapter_request)
            except (FileNotFoundError, ImportError, RuntimeError) as exc:
                if backend != MLX_BACKEND:
                    raise
                raise BlockedBackend(
                    "MLX backend is unavailable",
                    {
                        **self.config.status_fields(),
                        **self._model_status_for_payload(payload, backend),
                        "last_error": str(exc),
                    },
                ) from exc
            straight, alpha, processed = self._validated_adapter_outputs(
                adapter_result, pixel_count
            )
            if processed is None:
                processed = self._processed_rgba(
                    straight, alpha, model["screen_color"], payload["despill_strength"]
                )
            digest = input_digest(source, alpha_hint)
            output_key = (
                f"{payload['request_hash']}-{digest}-{model['model_id']}-"
                f"{model['model_version']}-{payload['quality']}-{payload['despill_strength']}"
            )
            output_paths = write_backend_outputs(payload, bounds, processed, straight, alpha, output_key)
            warnings = adapter_result.get("warnings", [])
            if isinstance(warnings, str):
                warnings = [warnings]
            elif not isinstance(warnings, list):
                warnings = []
            result = {
                **self._ready_payload(payload, model, backend),
                "cache": "miss",
                "timing_infer_ms": str(max(0, int(round((time.monotonic() - start) * 1000)))),
                "deterministic_key": f"{model['model_id']}:{model['model_version']}:{digest}:{payload['request_hash']}",
                "warnings": warnings,
                **output_paths,
            }
            return make_response(request_id, True, result, None)
        except BlockedBackend as exc:
            return blocked_response(
                request_id, payload, exc.message, exc.fields, exc.backend_status
            )
        except ProtocolError as exc:
            return make_response(
                request_id,
                False,
                {
                    "job_id": payload.get("job_id", ""),
                    "backend": payload.get("backend", ""),
                    "backend_status": "failed",
                    "gpu_backends_enabled": "false",
                    "final_failure": "true",
                },
                make_error(exc.code, exc.message),
            )
        except Exception:
            message = "Torch CPU backend failed before producing output"
            return make_response(
                request_id,
                False,
                {
                    "job_id": payload.get("job_id", ""),
                    "backend": payload.get("backend", ""),
                    "backend_status": "failed",
                    "gpu_backends_enabled": "false",
                    "final_failure": "true",
                    "last_error": message,
                },
                make_error("backend_runtime_error", message),
            )

    def _blocked_before_runtime(self, payload):
        backend = payload.get("backend", "")
        if backend == "torch_mps":
            return BlockedBackend(
                "torch_mps is disabled until HG-03/HG-04a validation records compatible model/runtime parity and real OOM evidence",
                self._torch_mps_blocked_fields(payload),
                backend_status="unsupported",
            )
        if backend == MLX_BACKEND:
            mlx_blocked = self._mlx_blocked_fields(payload)
            if mlx_blocked is not None:
                return mlx_blocked
        if backend in GPU_BACKENDS:
            return BlockedBackend(
                f"{backend} is disabled until HG-03/HG-04a validation is recorded",
                {
                    **self.config.status_fields(),
                    "last_error": f"{backend} is disabled until GPU validation is complete",
                },
                backend_status="unsupported",
            )
        if backend not in CPU_BACKENDS and backend != MLX_BACKEND:
            return BlockedBackend(
                f"{backend} is not a supported backend",
                {**self.config.status_fields(), "last_error": "unsupported backend"},
                backend_status="unsupported",
            )
        if backend in CPU_BACKENDS and self.config.device != "cpu":
            return BlockedBackend(
                "CORRIDORKEY_DEVICE must remain cpu for this backend",
                {
                    **self.config.status_fields(),
                    "last_error": "GPU devices are disabled for this backend",
                },
                backend_status="unsupported",
            )
        path_issues = self.config.required_path_issues()
        if path_issues:
            names = ",".join(issue.env_name for issue in path_issues)
            return BlockedBackend(
                f"CorridorKey {self._effective_backend(payload)} backend runtime paths are missing",
                {
                    **self.config.status_fields(),
                    **self._model_status_for_payload(payload, self._effective_backend(payload)),
                    "missing_runtime_paths": names,
                    "last_error": "missing required runtime paths: " + names,
                },
            )
        return None

    def _effective_backend(self, payload):
        return "torch_cpu" if payload.get("backend") == "auto" else payload.get("backend", "")

    def _mlx_blocked_fields(self, payload):
        if self.config.fixture_runtime_enabled():
            return None
        runtime_status = _mlx_runtime_status()
        if runtime_status.get("mlx_available") != "true":
            return BlockedBackend(
                "MLX backend runtime is unavailable",
                {
                    **self.config.status_fields(),
                    **runtime_status,
                    **self._model_status_for_payload(payload, MLX_BACKEND),
                    "last_error": runtime_status.get(
                        "mlx_unavailable_reason", "MLX runtime is unavailable"
                    ),
                },
                backend_status="unsupported",
            )
        return None

    def _torch_mps_blocked_fields(self, payload):
        fields = {
            **self.config.status_fields(),
            **_torch_mps_runtime_status(),
            "cpu_fallback_backend": "torch_cpu",
            "backend_gate": "HG-03/HG-04a",
            "parity_evidence": "missing",
            "real_oom_evidence": "missing",
            "last_error": "torch_mps disabled: HG-03/HG-04a compatible torch_mps model manifest, CPU/GPU parity, and real MPS OOM evidence are missing",
        }
        if self.model_manager is None:
            fields["torch_mps_model_status"] = "unknown"
            fields["torch_mps_model_last_error"] = "model manager unavailable"
            return fields
        try:
            status = self.model_manager.select_backend_model(
                payload.get("screen_color", "auto"), "torch_mps"
            )
        except Exception:
            fields["torch_mps_model_status"] = "unknown"
            fields["torch_mps_model_last_error"] = "model manager preflight failed"
            return fields

        for key, value in status.items():
            if key == "last_error":
                fields["torch_mps_model_last_error"] = value
            else:
                fields[f"torch_mps_{key}"] = value
        for key in ("model_status", "install_status", "model_source_status", "model_source_mode"):
            if key in status:
                fields[key] = status[key]
        return fields

    def _model_status_for_payload(self, payload, backend="torch_cpu"):
        if self.model_manager is None:
            return {}
        try:
            return self.model_manager.select_backend_model(
                payload.get("screen_color", "auto"), backend
            )
        except Exception:
            return {}

    def _select_model(self, payload, backend):
        screen_color = payload.get("screen_color", "auto")
        if self.config.fixture_runtime_enabled():
            return _select_fixture_model(
                self.config.model_dir,
                screen_color,
                backend,
            )
        if self.model_manager is None:
            raise BlockedBackend(
                "real CorridorKey model use requires a model manager",
                {**self.config.status_fields(), "last_error": "model manager unavailable"},
            )
        status = self.model_manager.select_backend_model(screen_color, backend)
        if status.get("model_status") != "ready":
            raise BlockedBackend(
                status.get("last_error") or f"{backend} model is not ready",
                {**self.config.status_fields(), **status},
            )
        return _model_from_status(status, self.config.model_dir)

    def _load_adapter(self, required_function):
        adapter_path = self.config.corridorkey_repo / ADAPTER_NAME
        if not adapter_path.is_file():
            raise BlockedBackend(
                f"missing CorridorKey OFX adapter: {ADAPTER_NAME}",
                {
                    **self.config.status_fields(),
                    "last_error": f"missing {ADAPTER_NAME} in CORRIDORKEY_REPO",
                },
            )
        spec = importlib.util.spec_from_file_location(
            "_corridorkey_ofx_runtime_adapter", adapter_path
        )
        if spec is None or spec.loader is None:
            raise BlockedBackend(
                "CorridorKey OFX adapter could not be loaded",
                {**self.config.status_fields(), "last_error": "adapter import failed"},
            )
        module = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(module)
        except ImportError as exc:
            raise BlockedBackend(
                "CorridorKey backend dependency is unavailable",
                {**self.config.status_fields(), "last_error": str(exc)},
            ) from exc
        if not callable(getattr(module, required_function, None)):
            raise BlockedBackend(
                f"CorridorKey OFX adapter must define callable {required_function}(request)",
                {
                    **self.config.status_fields(),
                    "last_error": f"adapter contract missing {required_function}",
                },
            )
        return module

    def _ready_payload(self, request_payload, model, backend="torch_cpu"):
        return {
            "job_id": request_payload["job_id"],
            "backend": backend,
            "requested_backend": request_payload["backend"],
            "effective_backend": backend,
            "backend_status": "ready",
            "gpu_backends_enabled": "true" if backend == MLX_BACKEND else "false",
            "model": model["model_version"],
            "model_version": model["model_version"],
            "model_id": model["model_id"],
            "model_status": "ready",
            "install_status": "ready",
            "model_source_status": "ready",
            "model_source_mode": "backend_fixture_only"
            if self.config.fixture_runtime_enabled()
            else "local_development",
            "screen_color": model["screen_color"],
            "backend_compatibility": ",".join(model["backend_compatibility"]),
            "requested_quality": request_payload["quality"],
            "effective_quality": request_payload["quality"],
            **self.config.status_fields(),
        }

    def _validated_adapter_outputs(self, adapter_result, pixel_count):
        if not isinstance(adapter_result, dict):
            raise ProtocolError("backend_contract_error", "Backend adapter result must be an object")
        straight = ensure_float_sequence(
            "straight_fg", adapter_result.get("straight_fg"), pixel_count * 3
        )
        alpha = ensure_float_sequence("alpha", adapter_result.get("alpha"), pixel_count)
        processed = None
        if "processed_rgba" in adapter_result:
            processed = ensure_float_sequence(
                "processed_rgba", adapter_result.get("processed_rgba"), pixel_count * 4
            )
        return straight, alpha, processed

    def _despill(self, straight, screen_color, despill_strength):
        despill = clamp01(float(despill_strength) / 10.0)
        if despill <= 0.0 or screen_color not in ("green", "blue"):
            return straight
        screen_index = 1 if screen_color == "green" else 2
        other_indices = [index for index in (0, 1, 2) if index != screen_index]
        adjusted = []
        for index in range(0, len(straight), 3):
            rgb = [straight[index], straight[index + 1], straight[index + 2]]
            limit = (rgb[other_indices[0]] + rgb[other_indices[1]]) / 2.0
            spill = max(rgb[screen_index] - limit, 0.0)
            despilled = list(rgb)
            despilled[screen_index] = rgb[screen_index] - spill
            despilled[other_indices[0]] = rgb[other_indices[0]] + spill * 0.5
            despilled[other_indices[1]] = rgb[other_indices[1]] + spill * 0.5
            adjusted.extend(
                clamp01(rgb[channel] * (1.0 - despill) + despilled[channel] * despill)
                for channel in (0, 1, 2)
            )
        return adjusted

    def _srgb_to_linear(self, value):
        encoded = clamp01(value)
        if encoded <= 0.04045:
            return encoded / 12.92
        return ((encoded + 0.055) / 1.055) ** 2.4

    def _processed_rgba(self, straight, alpha, screen_color, despill_strength):
        processed_straight = self._despill(straight, screen_color, despill_strength)
        processed = []
        for index, matte in enumerate(alpha):
            processed.extend(
                (
                    self._srgb_to_linear(processed_straight[index * 3]) * matte,
                    self._srgb_to_linear(processed_straight[index * 3 + 1]) * matte,
                    self._srgb_to_linear(processed_straight[index * 3 + 2]) * matte,
                    matte,
                )
            )
        return processed


def _select_fixture_model(model_dir, requested_screen_color, backend):
    candidates = []
    for manifest_path in sorted(Path(model_dir).glob("*/*/model-manifest.json")):
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        if not _fixture_manifest_compatible(manifest, requested_screen_color, backend):
            continue
        try:
            _validate_fixture_files(manifest_path.parent, manifest)
        except (OSError, ValueError):
            continue
        candidates.append((manifest_path.parent, manifest))

    if not candidates:
        raise BlockedBackend(
            f"no {backend} fixture model is available for screen_color {requested_screen_color}",
            {
                "model_status": "missing",
                "install_status": "not_installed",
                "last_error": f"no {backend} fixture model found for screen_color {requested_screen_color}",
            },
        )
    if requested_screen_color == "auto":
        candidates.sort(
            key=lambda item: (
                item[1].get("screen_color") != "green",
                item[1].get("model_id", ""),
                item[1].get("version", ""),
            )
        )
    model_root, selected = candidates[0]
    local_path = selected.get("local_path", "")
    checkpoint_path = model_root / local_path if local_path else None
    return {
        "model_id": selected["model_id"],
        "model_version": selected["version"],
        "screen_color": selected["screen_color"],
        "backend_compatibility": list(selected["backend_compatibility"]),
        "checkpoint_path": str(checkpoint_path) if checkpoint_path else "",
    }


def _checkpoint_path_from_status(status, model_dir):
    model_root = Path(model_dir) / status["model_id"] / status["model_version"]
    try:
        manifest = json.loads((model_root / "model-manifest.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return ""
    local_path = manifest.get("local_path", "")
    if not isinstance(local_path, str) or not local_path:
        return ""
    relative = Path(local_path)
    if relative.is_absolute() or ".." in relative.parts:
        return ""
    try:
        root = model_root.resolve(strict=True)
        checkpoint = (root / relative).resolve(strict=True)
        checkpoint.relative_to(root)
    except (OSError, ValueError):
        return ""
    return str(checkpoint)


def _model_from_status(status, model_dir=None):
    backend_compatibility = status.get("backend_compatibility", "")
    if isinstance(backend_compatibility, str):
        backend_compatibility = [
            item for item in backend_compatibility.split(",") if item
        ]
    model = {
        "model_id": status["model_id"],
        "model_version": status["model_version"],
        "screen_color": status["screen_color"],
        "backend_compatibility": list(backend_compatibility),
    }
    if model_dir is not None:
        checkpoint_path = _checkpoint_path_from_status(status, model_dir)
        if checkpoint_path:
            model["checkpoint_path"] = checkpoint_path
    return model


def _fixture_manifest_compatible(manifest, requested_screen_color, backend):
    if backend not in manifest.get("backend_compatibility", []):
        return False
    screen_color = manifest.get("screen_color")
    if requested_screen_color == "auto":
        return screen_color in ("green", "blue")
    return screen_color == requested_screen_color


def _validate_fixture_files(model_root, manifest):
    expected_files = manifest.get("expected_files", [])
    if not expected_files:
        raise OSError("fixture manifest has no expected files")
    root = Path(model_root).resolve(strict=True)
    for item in expected_files:
        rel_path = item.get("path")
        expected = item.get("sha256")
        if not isinstance(rel_path, str) or not isinstance(expected, str):
            raise OSError("fixture manifest expected file is invalid")
        path = (root / rel_path).resolve(strict=True)
        path.relative_to(root)
        if path.is_symlink() or not path.is_file():
            raise OSError("fixture file is unsafe")
        digest = _sha256(path)
        if digest != expected:
            raise OSError("fixture checksum mismatch")


def _sha256(path):
    import hashlib

    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _torch_mps_runtime_status():
    try:
        import torch
    except Exception as exc:
        return {
            "torch_importable": "false",
            "torch_version": "unavailable",
            "torch_mps_built": "unknown",
            "torch_mps_available": "false",
            "torch_import_error": f"{type(exc).__name__}: {exc}",
        }

    mps_backend = getattr(torch.backends, "mps", None)
    mps_built = bool(mps_backend and mps_backend.is_built())
    mps_available = bool(mps_backend and mps_backend.is_available())
    return {
        "torch_importable": "true",
        "torch_version": str(getattr(torch, "__version__", "unknown")),
        "torch_mps_built": _text_bool(mps_built),
        "torch_mps_available": _text_bool(mps_available),
    }


def _mlx_runtime_status():
    if sys.platform != "darwin" or platform.machine() != "arm64":
        return {
            "mlx_available": "false",
            "mlx_platform": f"{sys.platform}/{platform.machine()}",
            "mlx_unavailable_reason": "MLX backend requires Apple Silicon macOS",
        }
    try:
        import mlx  # noqa: F401
        import mlx.core as mx
        import corridorkey_mlx  # noqa: F401
    except Exception as exc:
        return {
            "mlx_available": "false",
            "mlx_platform": f"{sys.platform}/{platform.machine()}",
            "mlx_unavailable_reason": f"{type(exc).__name__}: {exc}",
        }
    return {
        "mlx_available": "true",
        "mlx_platform": f"{sys.platform}/{platform.machine()}",
        "mlx_version": str(getattr(mx, "__version__", getattr(mlx, "__version__", "unknown"))),
    }


def _text_bool(value):
    return "true" if value else "false"
