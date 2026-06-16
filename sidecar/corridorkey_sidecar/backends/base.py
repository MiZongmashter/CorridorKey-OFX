"""Shared backend contract helpers for sidecar inference adapters."""

from pathlib import Path

from ..protocol import (
    ERROR_INTERNAL,
    ProtocolError,
    _frame_blob_digest,
    _read_frame_blob,
    _write_frame_blob,
    make_error,
    make_response,
)


ERROR_BLOCKED_BACKEND = "blocked_backend"


class BlockedBackend(Exception):
    def __init__(self, message, fields=None, backend_status="blocked"):
        super().__init__(message)
        self.message = message
        self.fields = {} if fields is None else dict(fields)
        self.backend_status = backend_status


def blocked_response(request_id, request_payload, message, fields=None, backend_status="blocked"):
    payload = backend_base_payload(request_payload, backend_status)
    if fields:
        payload.update(fields)
    payload.setdefault("last_error", message)
    return make_response(
        request_id,
        False,
        payload,
        make_error(ERROR_BLOCKED_BACKEND, message),
    )


def backend_base_payload(request_payload, backend_status):
    return {
        "job_id": request_payload.get("job_id", ""),
        "backend": request_payload.get("backend", ""),
        "effective_backend": (
            "torch_cpu" if request_payload.get("backend") == "auto" else request_payload.get("backend", "")
        ),
        "backend_status": backend_status,
        "gpu_backends_enabled": "false",
        "model": "not_loaded",
        "model_version": "not_loaded",
        "model_id": "",
        "model_status": "missing",
        "install_status": "not_installed",
        "model_source_status": "ready",
        "model_source_mode": "local_development",
    }


def read_infer_frame_blobs(payload):
    bounds = (
        int(payload["render_window_x1"]),
        int(payload["render_window_y1"]),
        int(payload["render_window_x2"]),
        int(payload["render_window_y2"]),
    )
    source = _read_frame_blob(
        payload["source_frame_blob_path"],
        expected_bounds=bounds,
        expected_channels=4,
    )
    alpha_hint = _read_frame_blob(
        payload["alpha_hint_frame_blob_path"],
        expected_bounds=bounds,
        expected_channels=1,
    )
    pixel_count = (bounds[2] - bounds[0]) * (bounds[3] - bounds[1])
    return bounds, pixel_count, source, alpha_hint


def write_backend_outputs(payload, bounds, processed, straight, alpha, output_key):
    output_root = Path(payload["source_frame_blob_path"]).parent
    processed_path = output_root / f"{output_key}-processed-rgba.ckfb"
    straight_path = output_root / f"{output_key}-straight-fg.ckfb"
    alpha_path = output_root / f"{output_key}-alpha.ckfb"
    _write_frame_blob(processed_path, bounds, 4, processed)
    _write_frame_blob(straight_path, bounds, 3, straight)
    _write_frame_blob(alpha_path, bounds, 1, alpha)
    return {
        "processed_rgba_frame_blob_path": str(processed_path),
        "straight_fg_frame_blob_path": str(straight_path),
        "alpha_frame_blob_path": str(alpha_path),
    }


def input_digest(source_blob, alpha_hint_blob):
    return _frame_blob_digest(source_blob, alpha_hint_blob)


def clamp01(value):
    return min(1.0, max(0.0, float(value)))


def ensure_float_sequence(name, values, expected_count):
    if not isinstance(values, (list, tuple)) or len(values) != expected_count:
        raise ProtocolError(ERROR_INTERNAL, f"Backend output {name} has invalid shape")
    return [clamp01(value) for value in values]
