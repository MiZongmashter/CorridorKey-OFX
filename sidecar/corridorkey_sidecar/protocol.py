"""Lightweight NDJSON protocol for the CorridorKey sidecar."""

import hashlib
import json
import math
import os
from pathlib import Path
import re
from json import JSONDecodeError
import struct
import threading
import tempfile
import time

VERSION = "0.1.0"

_REQUEST_ID_RE = re.compile(r"^req-[a-z0-9][a-z0-9_.:-]{0,123}$")
_JOB_ID_RE = re.compile(r"^job-[a-z0-9][a-z0-9_.:-]{0,123}$")

ERROR_INVALID_REQUEST = "invalid_request"
ERROR_INTERNAL = "internal_error"
ERROR_MALFORMED_JSON = "malformed_json"
ERROR_NOT_IMPLEMENTED = "not_implemented"
ERROR_UNKNOWN_COMMAND = "unknown_command"
ERROR_GPU_OOM = "gpu_oom"
ERROR_BLOCKED_BACKEND = "blocked_backend"
ERROR_MODEL_SOURCE_BLOCKED = "model_source_blocked"

COMMAND_INFER = "infer"
COMMAND_WARMUP = "warmup"
COMMAND_CANCEL = "cancel"
COMMAND_INSTALL_MODEL = "install_model"
COMMAND_DOWNLOAD_MODEL = "download_model"
COMMAND_CANCEL_DOWNLOAD = "cancel_download"
COMMAND_MODEL_STATUS = "model_status"

OUTPUT_MODES = frozenset(
    (
        "processed_rgba",
        "matte",
        "straight_fg",
        "alpha_hint_view",
        "checker_comp",
        "status",
    )
)
SCREEN_COLORS = frozenset(("auto", "green", "blue"))
QUALITIES = frozenset(("draft_512", "high_1024", "full_2048"))
INPUT_COLOR_SPACES = frozenset(("host_managed", "srgb_rec709", "linear"))
ALPHA_HINT_SOURCES = frozenset(("external", "source_alpha", "red_channel", "rough_fallback"))
BACKENDS = frozenset(("auto", "torch_cuda", "torch_cpu", "torch_mps", "mlx", "stub"))

INFER_REQUIRED_FIELDS = frozenset(
    (
        "frame_id",
        "job_id",
        "render_window_x1",
        "render_window_y1",
        "render_window_x2",
        "render_window_y2",
        "source_frame_blob_path",
        "alpha_hint_frame_blob_path",
        "alpha_hint_source",
        "screen_color",
        "quality",
        "input_color_space",
        "despill_strength",
        "backend",
        "output_mode",
        "request_hash",
    )
)
FORBIDDEN_INFER_PATH_FIELDS = frozenset(("source_media_path", "raw_media_path", "project_path"))


class ProtocolError(Exception):
    def __init__(self, code, message, request_id=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.request_id = request_id


def _default_model_manager():
    from .model_manager import ModelManager
    from .runtime_config import RuntimeConfig

    if os.environ.get("CORRIDORKEY_MODEL_ROOT") and not os.environ.get("CORRIDORKEY_MODEL_DIR"):
        return ModelManager(Path(os.environ["CORRIDORKEY_MODEL_ROOT"]))
    return ModelManager(RuntimeConfig.from_env().model_dir)


def _model_status_fields(model_manager=None):
    manager = model_manager if model_manager is not None else _default_model_manager()
    return manager.status()


def _visible_model_status_fields(model_manager=None):
    status = _model_status_fields(model_manager)
    return {
        "model_version": status["model_version"],
        "model_id": status["model_id"],
        "model_checksum": status["model_checksum"],
        "screen_color": status["screen_color"],
        "backend_compatibility": status["backend_compatibility"],
        "model_status": status["model_status"],
        "model_source_status": status["model_source_status"],
        "model_source_mode": status["model_source_mode"],
        "install_status": status["install_status"],
    }


def health_payload():
    return {
        "version": VERSION,
        "backend": "stub",
        "model": "not_loaded",
        "warmup": "cold",
        "cache": "disabled",
        "gpu": "unknown",
        "vram": "unknown",
    }


def status_payload(model_manager=None):
    payload = health_payload()
    payload.update(_model_status_fields(model_manager))
    return payload


def _matches(pattern, value):
    return isinstance(value, str) and pattern.match(value) is not None


_FRAME_BLOB_HEADER = struct.Struct("<4sIiiiiI")
_MAX_STUB_PIXELS = 16384 * 16384
_GPU_CLASS_QUEUE_LOCK = threading.Lock()
_DOWNLOAD_CANCEL_LOCK = threading.Lock()
_DOWNLOAD_CANCELLED_JOBS = set()
_QUALITY_DOWNGRADE = {
    "full_2048": "high_1024",
    "high_1024": "draft_512",
}


class SyntheticOOM(ProtocolError):
    def __init__(self, quality):
        super().__init__(
            ERROR_GPU_OOM,
            f"Synthetic GPU out of memory at quality {quality}",
        )
        self.quality = quality


def _clamp01(value):
    return min(1.0, max(0.0, value))


def _test_delay(env_name):
    if os.environ.get("CORRIDORKEY_TEST_FAULTS_ALLOWED") != "1":
        return
    try:
        delay_ms = int(os.environ.get(env_name, "0"))
    except ValueError:
        delay_ms = 0
    remaining = max(0, delay_ms) / 1000.0
    while remaining > 0:
        step = min(0.025, remaining)
        time.sleep(step)
        remaining -= step


def _bool_text(value):
    return "true" if value else "false"


def _timing_ms(start):
    return str(max(0, int(round((time.monotonic() - start) * 1000))))


def _mark_download_cancelled(job_id):
    with _DOWNLOAD_CANCEL_LOCK:
        _DOWNLOAD_CANCELLED_JOBS.add(job_id)


def _clear_download_cancelled(job_id):
    with _DOWNLOAD_CANCEL_LOCK:
        _DOWNLOAD_CANCELLED_JOBS.discard(job_id)


def _download_cancel_requested(job_id):
    with _DOWNLOAD_CANCEL_LOCK:
        return job_id in _DOWNLOAD_CANCELLED_JOBS


def _synthetic_oom_qualities():
    if os.environ.get("CORRIDORKEY_TEST_FAULTS_ALLOWED") != "1":
        return frozenset()
    raw = os.environ.get("CORRIDORKEY_TEST_SYNTHETIC_OOM_QUALITIES", "")
    tokens = re.split(r"[\s,;:]+", raw)
    return frozenset(token for token in tokens if token in QUALITIES)


def _raise_if_synthetic_oom(quality):
    if quality in _synthetic_oom_qualities():
        raise SyntheticOOM(quality)


def _diagnostic_fields(requested_quality, effective_quality, queue_time_ms, oom, downgraded, failed):
    return {
        "requested_quality": requested_quality,
        "effective_quality": effective_quality,
        "queue_time_ms": str(queue_time_ms),
        "oom": _bool_text(oom),
        "downgraded_quality": _bool_text(downgraded),
        "final_failure": _bool_text(failed),
    }


def _read_frame_blob(path_text, expected_bounds=None, expected_channels=None):
    try:
        with Path(path_text).open("rb") as handle:
            header = handle.read(_FRAME_BLOB_HEADER.size)
            if len(header) != _FRAME_BLOB_HEADER.size:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob header is invalid")
            magic, version, x1, y1, x2, y2, channels = _FRAME_BLOB_HEADER.unpack(header)
            if magic != b"CKFB" or version != 1:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob header is invalid")
            if x2 <= x1 or y2 <= y1 or channels not in (1, 3, 4):
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob shape is invalid")
            bounds = (x1, y1, x2, y2)
            if expected_bounds is not None and bounds != expected_bounds:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob bounds must match render window")
            if expected_channels is not None and channels != expected_channels:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob channels are invalid")
            pixel_count = (x2 - x1) * (y2 - y1)
            if pixel_count > _MAX_STUB_PIXELS:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob is too large")
            count = pixel_count * channels
            data = handle.read(count * 4)
            if len(data) != count * 4:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob data is incomplete")
            if handle.read(1):
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob has trailing data")
            return {
                "bounds": bounds,
                "channels": channels,
                "values": struct.unpack(f"<{count}f", data),
            }
    except (OSError, OverflowError, struct.error) as exc:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob is invalid") from exc


def _validate_frame_blob_header(path_text, expected_bounds=None, expected_channels=None):
    try:
        with Path(path_text).open("rb") as handle:
            header = handle.read(_FRAME_BLOB_HEADER.size)
            if len(header) != _FRAME_BLOB_HEADER.size:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob header is invalid")
            magic, version, x1, y1, x2, y2, channels = _FRAME_BLOB_HEADER.unpack(header)
            if magic != b"CKFB" or version != 1:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob header is invalid")
            if x2 <= x1 or y2 <= y1 or channels not in (1, 3, 4):
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob shape is invalid")
            bounds = (x1, y1, x2, y2)
            if expected_bounds is not None and bounds != expected_bounds:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob bounds must match render window")
            if expected_channels is not None and channels != expected_channels:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob channels are invalid")
            pixel_count = (x2 - x1) * (y2 - y1)
            if pixel_count > _MAX_STUB_PIXELS:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob is too large")
            expected_size = _FRAME_BLOB_HEADER.size + pixel_count * channels * 4
            try:
                actual_size = Path(path_text).stat().st_size
            except OSError as exc:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob is invalid") from exc
            if actual_size != expected_size:
                raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob data is incomplete")
            return {"bounds": bounds, "channels": channels, "pixel_count": pixel_count}
    except (OSError, OverflowError, struct.error) as exc:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob is invalid") from exc


def _write_frame_blob(path, bounds, channels, values):
    if channels not in (1, 3, 4):
        raise ProtocolError(ERROR_INTERNAL, "Internal fake output shape is invalid")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("wb") as handle:
            handle.write(_FRAME_BLOB_HEADER.pack(b"CKFB", 1, *bounds, channels))
            if isinstance(values, (list, tuple)):
                handle.write(struct.pack(f"<{len(values)}f", *values))
            else:
                chunk = []
                for value in values:
                    chunk.append(value)
                    if len(chunk) >= 65536:
                        handle.write(struct.pack(f"<{len(chunk)}f", *chunk))
                        chunk.clear()
                if chunk:
                    handle.write(struct.pack(f"<{len(chunk)}f", *chunk))
    except OSError as exc:
        raise ProtocolError(ERROR_INTERNAL, "Internal fake output could not be written") from exc


def _blob_value(blob, pixel_index, channel):
    channels = blob["channels"]
    if channel >= channels:
        return 1.0 if channel == 3 else 0.0
    return blob["values"][pixel_index * channels + channel]


def _frame_blob_digest(*blobs):
    digest = hashlib.sha256()
    for blob in blobs:
        digest.update(_FRAME_BLOB_HEADER.pack(b"CKFB", 1, *blob["bounds"], blob["channels"]))
        digest.update(struct.pack(f"<{len(blob['values'])}f", *blob["values"]))
    return digest.hexdigest()[:16]


def _frame_blob_file_digest(*paths):
    digest = hashlib.sha256()
    try:
        for path in paths:
            with Path(path).open("rb") as handle:
                for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                    digest.update(chunk)
        return digest.hexdigest()[:16]
    except OSError as exc:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob is invalid") from exc


def _fake_infer_parameters(payload):
    screen_bias = {
        "auto": 0.0,
        "green": 0.035,
        "blue": 0.07,
    }[payload["screen_color"]]
    quality_bias = {
        "draft_512": -0.025,
        "high_1024": 0.0,
        "full_2048": 0.035,
    }[payload["quality"]]
    despill = float(payload["despill_strength"])
    despill_bias = despill / 10.0
    alpha_scale = 0.88 + screen_bias + quality_bias + despill_bias * 0.08
    quality_gain = 0.94 + quality_bias * 1.2
    return alpha_scale, quality_gain, despill_bias


def _fake_infer_pixel_values(source, alpha_hint, index, payload, alpha_scale, quality_gain, despill_bias):
    src_r = _blob_value(source, index, 0)
    src_g = _blob_value(source, index, 1)
    src_b = _blob_value(source, index, 2)
    src_a = _blob_value(source, index, 3)
    hint = alpha_hint["values"][index]

    matte = _clamp01(hint * alpha_scale + src_a * 0.05 + 0.005)
    if payload["screen_color"] == "green":
        fg_r = _clamp01((src_r + 0.018 * despill_bias) * quality_gain)
        fg_g = _clamp01((src_g - 0.12 * despill_bias) * quality_gain)
        fg_b = _clamp01(src_b * quality_gain)
    elif payload["screen_color"] == "blue":
        fg_r = _clamp01(src_r * quality_gain)
        fg_g = _clamp01((src_g + 0.012 * despill_bias) * quality_gain)
        fg_b = _clamp01((src_b - 0.12 * despill_bias) * quality_gain)
    else:
        fg_r = _clamp01((src_r + 0.006 * despill_bias) * quality_gain)
        fg_g = _clamp01((src_g - 0.006 * despill_bias) * quality_gain)
        fg_b = _clamp01(src_b * quality_gain)
    return fg_r, fg_g, fg_b, matte


def _fake_infer_small_outputs(payload, bounds, source, alpha_hint, pixel_count):
    alpha_scale, quality_gain, despill_bias = _fake_infer_parameters(payload)

    processed = []
    straight = []
    alpha = []
    for index in range(pixel_count):
        fg_r, fg_g, fg_b, matte = _fake_infer_pixel_values(
            source, alpha_hint, index, payload, alpha_scale, quality_gain, despill_bias
        )
        straight.extend((fg_r, fg_g, fg_b))
        alpha.append(matte)
        processed.extend((fg_r * matte, fg_g * matte, fg_b * matte, matte))
    return processed, straight, alpha


def _fake_infer_streaming_outputs(payload, bounds, output_paths):
    alpha_scale, quality_gain, despill_bias = _fake_infer_parameters(payload)
    source_path = Path(payload["source_frame_blob_path"])
    alpha_path = Path(payload["alpha_hint_frame_blob_path"])
    processed_path, straight_path, output_alpha_path = output_paths
    output_mode = payload["output_mode"]
    write_processed = output_mode != "straight_fg"
    write_straight = True
    write_alpha = True
    try:
        with source_path.open("rb") as source_handle, alpha_path.open("rb") as alpha_handle:
            source_handle.seek(_FRAME_BLOB_HEADER.size)
            alpha_handle.seek(_FRAME_BLOB_HEADER.size)
            handles = {}
            try:
                if write_processed:
                    handles["processed"] = processed_path.open("wb")
                    handles["processed"].write(_FRAME_BLOB_HEADER.pack(b"CKFB", 1, *bounds, 4))
                if write_straight:
                    handles["straight"] = straight_path.open("wb")
                    handles["straight"].write(_FRAME_BLOB_HEADER.pack(b"CKFB", 1, *bounds, 3))
                if write_alpha:
                    handles["alpha"] = output_alpha_path.open("wb")
                    handles["alpha"].write(_FRAME_BLOB_HEADER.pack(b"CKFB", 1, *bounds, 1))
                while True:
                    source_data = source_handle.read(4 * 4 * 16384)
                    alpha_data = alpha_handle.read(4 * 16384)
                    if not source_data and not alpha_data:
                        break
                    if len(source_data) % 16 != 0 or len(alpha_data) * 4 != len(source_data):
                        raise ProtocolError(ERROR_INVALID_REQUEST, "Frame blob data is incomplete")
                    source_values = struct.unpack(f"<{len(source_data) // 4}f", source_data)
                    alpha_values = struct.unpack(f"<{len(alpha_data) // 4}f", alpha_data)
                    processed = []
                    straight = []
                    alpha = []
                    for index, hint in enumerate(alpha_values):
                        base = index * 4
                        src_r = source_values[base]
                        src_g = source_values[base + 1]
                        src_b = source_values[base + 2]
                        src_a = source_values[base + 3]
                        matte = _clamp01(hint * alpha_scale + src_a * 0.05 + 0.005)
                        if payload["screen_color"] == "green":
                            fg_r = _clamp01((src_r + 0.018 * despill_bias) * quality_gain)
                            fg_g = _clamp01((src_g - 0.12 * despill_bias) * quality_gain)
                            fg_b = _clamp01(src_b * quality_gain)
                        elif payload["screen_color"] == "blue":
                            fg_r = _clamp01(src_r * quality_gain)
                            fg_g = _clamp01((src_g + 0.012 * despill_bias) * quality_gain)
                            fg_b = _clamp01((src_b - 0.12 * despill_bias) * quality_gain)
                        else:
                            fg_r = _clamp01((src_r + 0.006 * despill_bias) * quality_gain)
                            fg_g = _clamp01((src_g - 0.006 * despill_bias) * quality_gain)
                            fg_b = _clamp01(src_b * quality_gain)
                        if write_straight:
                            straight.extend((fg_r, fg_g, fg_b))
                        if write_alpha:
                            alpha.append(matte)
                        if write_processed:
                            processed.extend((fg_r * matte, fg_g * matte, fg_b * matte, matte))
                    if write_processed:
                        handles["processed"].write(struct.pack(f"<{len(processed)}f", *processed))
                    if write_straight:
                        handles["straight"].write(struct.pack(f"<{len(straight)}f", *straight))
                    if write_alpha:
                        handles["alpha"].write(struct.pack(f"<{len(alpha)}f", *alpha))
            finally:
                for handle in handles.values():
                    handle.close()
    except OSError as exc:
        raise ProtocolError(ERROR_INTERNAL, "Internal fake output could not be written") from exc


def _fake_infer_payload(payload):
    job_id = payload["job_id"]
    _raise_if_synthetic_oom(payload["quality"])
    _test_delay("CORRIDORKEY_TEST_FAKE_INFER_DELAY_MS")
    bounds = (
        int(payload["render_window_x1"]),
        int(payload["render_window_y1"]),
        int(payload["render_window_x2"]),
        int(payload["render_window_y2"]),
    )
    pixel_count = (bounds[2] - bounds[0]) * (bounds[3] - bounds[1])

    request_key = payload["request_hash"]
    input_key = _frame_blob_file_digest(
        payload["source_frame_blob_path"], payload["alpha_hint_frame_blob_path"]
    )
    output_key = f"{request_key}-{input_key}"
    output_root = Path(payload["source_frame_blob_path"]).parent
    processed_path = output_root / f"{output_key}-processed-rgba.ckfb"
    straight_path = output_root / f"{output_key}-straight-fg.ckfb"
    alpha_path = output_root / f"{output_key}-alpha.ckfb"
    if pixel_count <= 262144:
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
        processed, straight, alpha = _fake_infer_small_outputs(
            payload, bounds, source, alpha_hint, pixel_count
        )
        if payload["output_mode"] != "straight_fg":
            _write_frame_blob(processed_path, bounds, 4, processed)
        _write_frame_blob(straight_path, bounds, 3, straight)
        _write_frame_blob(alpha_path, bounds, 1, alpha)
    else:
        _validate_frame_blob_header(
            payload["source_frame_blob_path"],
            expected_bounds=bounds,
            expected_channels=4,
        )
        _validate_frame_blob_header(
            payload["alpha_hint_frame_blob_path"],
            expected_bounds=bounds,
            expected_channels=1,
        )
        _fake_infer_streaming_outputs(
            payload, bounds, (processed_path, straight_path, alpha_path)
        )
    if (
        processed_path.exists()
        and os.environ.get("CORRIDORKEY_TEST_FAULTS_ALLOWED") == "1"
        and os.environ.get(
        "CORRIDORKEY_TEST_APPEND_OUTPUT_TRAILING_DATA"
        )
        == "1"
    ):
        with processed_path.open("ab") as handle:
            handle.write(b"trailing")

    response = {
        "backend": "stub",
        "job_id": job_id,
        "model": "stub",
        "cache": "miss",
        "timing_infer_ms": "0",
        "deterministic_key": (
            f"{payload['screen_color']}:{payload['quality']}:"
            f"{payload['despill_strength']}:{request_key}:{input_key}"
        ),
    }
    if processed_path.exists():
        response["processed_rgba_frame_blob_path"] = str(processed_path)
    if straight_path.exists():
        response["straight_fg_frame_blob_path"] = str(straight_path)
    if alpha_path.exists():
        response["alpha_frame_blob_path"] = str(alpha_path)
    return response


def _fake_infer_with_downgrade(payload, queue_time_ms):
    requested_quality = payload["quality"]
    effective_quality = requested_quality
    saw_oom = False
    downgraded = False

    try:
        result = _fake_infer_payload(payload)
    except SyntheticOOM as exc:
        saw_oom = True
        fallback_quality = _QUALITY_DOWNGRADE.get(requested_quality)
        if fallback_quality is None:
            diagnostics = _diagnostic_fields(
                requested_quality,
                effective_quality,
                queue_time_ms,
                saw_oom,
                downgraded,
                True,
            )
            diagnostics["job_id"] = payload["job_id"]
            raise SyntheticOOMFailure(exc.message, diagnostics) from exc

        retry_payload = dict(payload)
        retry_payload["quality"] = fallback_quality
        effective_quality = fallback_quality
        downgraded = True
        try:
            result = _fake_infer_payload(retry_payload)
        except SyntheticOOM as retry_exc:
            diagnostics = _diagnostic_fields(
                requested_quality,
                effective_quality,
                queue_time_ms,
                saw_oom,
                downgraded,
                True,
            )
            diagnostics["job_id"] = payload["job_id"]
            raise SyntheticOOMFailure(retry_exc.message, diagnostics) from retry_exc
        except ProtocolError as retry_exc:
            diagnostics = _diagnostic_fields(
                requested_quality,
                effective_quality,
                queue_time_ms,
                saw_oom,
                downgraded,
                True,
            )
            diagnostics["job_id"] = payload["job_id"]
            raise DiagnosticProtocolFailure(retry_exc, diagnostics) from retry_exc

    result.update(
        _diagnostic_fields(
            requested_quality,
            effective_quality,
            queue_time_ms,
            saw_oom,
            downgraded,
            False,
        )
    )
    return result


class SyntheticOOMFailure(ProtocolError):
    def __init__(self, message, diagnostics):
        super().__init__(ERROR_GPU_OOM, message)
        self.diagnostics = diagnostics


class DiagnosticProtocolFailure(ProtocolError):
    def __init__(self, cause, diagnostics):
        super().__init__(cause.code, cause.message)
        self.diagnostics = diagnostics


def _with_backend_queue(infer_fn):
    queue_start = time.monotonic()
    with _GPU_CLASS_QUEUE_LOCK:
        queue_time_ms = _timing_ms(queue_start)
        try:
            return infer_fn(queue_time_ms)
        except (SyntheticOOMFailure, DiagnosticProtocolFailure):
            raise
        except ProtocolError as exc:
            diagnostics = _diagnostic_fields("", "", queue_time_ms, False, False, True)
            raise DiagnosticProtocolFailure(exc, diagnostics) from exc


def make_error(code, message):
    return {
        "code": code,
        "message": message,
    }


def make_response(request_id, ok, payload=None, error=None):
    return {
        "request_id": request_id,
        "ok": bool(ok),
        "payload": {} if payload is None else payload,
        "error": error,
    }


def encode_response(response):
    return json.dumps(response, allow_nan=False, separators=(",", ":"), sort_keys=True)


def _reject_json_constant(value):
    raise ValueError("Non-finite JSON constant is not allowed: " + value)


def _validate_request_id(value):
    return _matches(_REQUEST_ID_RE, value)


def _format_contract_float(value):
    text = f"{value:.3f}"
    text = text.rstrip("0").rstrip(".")
    return text if text else "0"


def request_hash_for_payload(payload):
    hash_value = 14695981039346656037

    def update(text):
        nonlocal hash_value
        for byte in text.encode("utf-8"):
            hash_value ^= byte
            hash_value = (hash_value * 1099511628211) & 0xFFFFFFFFFFFFFFFF

    for key in sorted(payload):
        if key == "request_hash":
            continue
        update(key)
        update("\0")
        update(str(payload[key]))
        update("\0")
    return f"{hash_value:016x}"


def _require_string(payload, key):
    value = payload.get(key)
    if not isinstance(value, str) or not value:
        raise ProtocolError(ERROR_INVALID_REQUEST, f"Infer payload field {key} must be a string")
    return value


def _require_choice(payload, key, allowed, label="Infer"):
    value = _require_string(payload, key)
    if value not in allowed:
        raise ProtocolError(ERROR_INVALID_REQUEST, f"{label} payload field {key} is invalid")
    return value


def _require_job_id(payload):
    value = payload.get("job_id")
    if not _matches(_JOB_ID_RE, value):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Request payload field job_id is invalid")
    return value


def _require_int_string(payload, key):
    value = payload.get(key)
    if isinstance(value, bool):
        raise ProtocolError(ERROR_INVALID_REQUEST, f"Infer payload field {key} must be an integer")
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        try:
            parsed = int(value)
        except ValueError as exc:
            raise ProtocolError(
                ERROR_INVALID_REQUEST, f"Infer payload field {key} must be an integer"
            ) from exc
        if str(parsed) == value:
            return value
    raise ProtocolError(ERROR_INVALID_REQUEST, f"Infer payload field {key} must be an integer")


def _require_despill_strength(payload):
    value = payload.get("despill_strength")
    if isinstance(value, bool):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Infer payload despill_strength is out of range")
    try:
        parsed = float(value)
    except (TypeError, ValueError) as exc:
        raise ProtocolError(
            ERROR_INVALID_REQUEST, "Infer payload despill_strength is out of range"
        ) from exc
    if not math.isfinite(parsed) or parsed < 0.0 or parsed > 10.0:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Infer payload despill_strength is out of range")
    return _format_contract_float(parsed)


def _temp_frame_blob_job_path(value):
    if not isinstance(value, str) or not value:
        return None
    path = Path(value)
    if not path.is_absolute():
        return None
    try:
        temp_root = Path(tempfile.gettempdir()).resolve(strict=False)
        normalized = path.resolve(strict=False)
    except (OSError, ValueError):
        return None
    if normalized.suffix != ".ckfb":
        return None
    try:
        relative = normalized.relative_to(temp_root)
    except ValueError:
        return None
    if len(relative.parts) != 2:
        return None
    job_name = relative.parts[0]
    if not job_name.startswith("corridorkey-render-"):
        return None
    return temp_root / job_name


def _is_temp_frame_blob_path(value):
    return _temp_frame_blob_job_path(value) is not None


def _require_temp_frame_blob_path(payload, key):
    value = _require_string(payload, key)
    if not _is_temp_frame_blob_path(value):
        raise ProtocolError(
            ERROR_INVALID_REQUEST,
            f"Infer payload field {key} must be a temp frame blob path",
        )
    return value


def validate_infer_payload(payload):
    if not isinstance(payload, dict):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Request payload must be an object")
    forbidden = FORBIDDEN_INFER_PATH_FIELDS.intersection(payload)
    if forbidden:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Infer payload must not include raw media paths")
    extra = sorted(set(payload).difference(INFER_REQUIRED_FIELDS))
    if extra:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Infer payload fields are invalid")
    missing = sorted(INFER_REQUIRED_FIELDS.difference(payload))
    if missing:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Infer payload is missing required fields")

    normalized = {
        "frame_id": _require_string(payload, "frame_id"),
        "job_id": _require_job_id(payload),
        "render_window_x1": _require_int_string(payload, "render_window_x1"),
        "render_window_y1": _require_int_string(payload, "render_window_y1"),
        "render_window_x2": _require_int_string(payload, "render_window_x2"),
        "render_window_y2": _require_int_string(payload, "render_window_y2"),
        "source_frame_blob_path": _require_temp_frame_blob_path(payload, "source_frame_blob_path"),
        "alpha_hint_frame_blob_path": _require_temp_frame_blob_path(
            payload, "alpha_hint_frame_blob_path"
        ),
        "alpha_hint_source": _require_choice(payload, "alpha_hint_source", ALPHA_HINT_SOURCES),
        "screen_color": _require_choice(payload, "screen_color", SCREEN_COLORS),
        "quality": _require_choice(payload, "quality", QUALITIES),
        "input_color_space": _require_choice(payload, "input_color_space", INPUT_COLOR_SPACES),
        "despill_strength": _require_despill_strength(payload),
        "backend": _require_choice(payload, "backend", BACKENDS),
        "output_mode": _require_choice(payload, "output_mode", OUTPUT_MODES),
    }

    x1 = int(normalized["render_window_x1"])
    y1 = int(normalized["render_window_y1"])
    x2 = int(normalized["render_window_x2"])
    y2 = int(normalized["render_window_y2"])
    if x2 <= x1 or y2 <= y1:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Infer payload render window must be non-empty")
    if _temp_frame_blob_job_path(normalized["source_frame_blob_path"]) != _temp_frame_blob_job_path(
        normalized["alpha_hint_frame_blob_path"]
    ):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Infer payload frame blobs must share a job path")

    expected_hash = request_hash_for_payload(normalized)
    provided_hash = payload.get("request_hash")
    if provided_hash != expected_hash:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Infer payload request_hash mismatch")
    normalized["request_hash"] = expected_hash
    return normalized


def validate_warmup_payload(payload):
    if not isinstance(payload, dict):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Request payload must be an object")
    if set(payload) != {"job_id", "backend", "quality"}:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Warmup payload fields are invalid")
    return {
        "job_id": _require_job_id(payload),
        "backend": _require_choice(payload, "backend", BACKENDS, "Warmup"),
        "quality": _require_choice(payload, "quality", QUALITIES, "Warmup"),
    }


def validate_cancel_payload(payload):
    if not isinstance(payload, dict):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Request payload must be an object")
    if set(payload) != {"job_id"}:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Cancel payload fields are invalid")
    return {"job_id": _require_job_id(payload)}


def validate_download_model_payload(payload):
    if not isinstance(payload, dict):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Request payload must be an object")
    if set(payload) != {"job_id", "manifest_url"}:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Download model payload fields are invalid")
    return {
        "job_id": _require_job_id(payload),
        "manifest_url": _require_string(payload, "manifest_url"),
    }


def validate_model_status_payload(payload):
    if not isinstance(payload, dict):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Request payload must be an object")
    if payload:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Model status payload fields are invalid")
    return {}


def validate_install_model_payload(payload):
    if not isinstance(payload, dict):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Request payload must be an object")
    if set(payload) != {"source_path"}:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Install model payload fields are invalid")
    return {"source_path": _require_string(payload, "source_path")}


def decode_request(line):
    try:
        request = json.loads(line, parse_constant=_reject_json_constant)
    except (JSONDecodeError, ValueError) as exc:
        raise ProtocolError(ERROR_MALFORMED_JSON, "Malformed JSON request") from exc

    if not isinstance(request, dict):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Request must be a JSON object")

    request_id = request.get("request_id")
    command = request.get("command")
    payload = request.get("payload")

    if not _validate_request_id(request_id):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Request must include a safe string request_id")
    if not isinstance(command, str) or not command:
        raise ProtocolError(ERROR_INVALID_REQUEST, "Request must include command", request_id)
    if not isinstance(payload, dict):
        raise ProtocolError(ERROR_INVALID_REQUEST, "Request payload must be an object", request_id)
    validators = {
        COMMAND_INFER: validate_infer_payload,
        COMMAND_WARMUP: validate_warmup_payload,
        COMMAND_CANCEL: validate_cancel_payload,
        COMMAND_INSTALL_MODEL: validate_install_model_payload,
        COMMAND_DOWNLOAD_MODEL: validate_download_model_payload,
        COMMAND_CANCEL_DOWNLOAD: validate_cancel_payload,
        COMMAND_MODEL_STATUS: validate_model_status_payload,
    }
    validator = validators.get(command)
    if validator is not None:
        try:
            payload = validator(payload)
        except ProtocolError as exc:
            exc.request_id = request_id
            raise

    return {
        "request_id": request_id,
        "command": command,
        "payload": payload,
    }


def _backend_unavailable_response(
    request_id, job_id="", backend="", screen_color="auto", model_manager=None
):
    payload = {
        "job_id": job_id,
        "backend": backend,
        "backend_status": "blocked",
        "gpu_backends_enabled": "false",
        "model": "not_loaded",
        "model_version": "not_loaded",
        "model_id": "",
        "model_status": "missing",
        "install_status": "not_installed",
        "model_source_status": "ready",
        "model_source_mode": "local_development",
        "last_error": "backend runtime is not configured",
    }
    if model_manager is not None:
        selection_backend = "torch_cpu" if backend == "auto" else backend
        selected = model_manager.select_backend_model(screen_color, selection_backend)
        if selected.get("model_status") != "missing":
            payload.update(selected)
            payload["last_error"] = "backend runtime is not configured"
    return make_response(
        request_id,
        False,
        payload,
        make_error(ERROR_BLOCKED_BACKEND, payload["last_error"]),
    )


def handle_request(request, model_manager=None):
    request_id = request["request_id"]
    command = request["command"]

    if command == "health":
        return make_response(request_id, True, health_payload(), None), False
    if command == "status":
        return make_response(request_id, True, status_payload(model_manager), None), False
    if command == COMMAND_MODEL_STATUS:
        manager = model_manager if model_manager is not None else _default_model_manager()
        return make_response(request_id, True, manager.status(), None), False
    if command == "shutdown":
        return make_response(request_id, True, {"shutdown": "accepted"}, None), True
    if command == COMMAND_INSTALL_MODEL:
        payload = request["payload"]
        manager = model_manager if model_manager is not None else _default_model_manager()
        result = manager.install_offline(Path(payload["source_path"]))
        ok = result.get("install_status") == "ready"
        error = (
            None
            if ok
            else make_error(
                result.get("model_status", "install_failed"),
                result.get("last_error", "offline install failed"),
            )
        )
        return make_response(request_id, ok, result, error), False
    if command == COMMAND_DOWNLOAD_MODEL:
        payload = request["payload"]
        manager = model_manager if model_manager is not None else _default_model_manager()
        _clear_download_cancelled(payload["job_id"])
        try:
            result = manager.download_model(
                payload["manifest_url"],
                payload["job_id"],
                cancel_requested=lambda: _download_cancel_requested(payload["job_id"]),
            )
        finally:
            _clear_download_cancelled(payload["job_id"])
        ok = result.get("download_status") == "ready" and result.get("install_status") == "ready"
        if ok:
            error = None
        elif result.get("download_status") == "cancelled":
            error = make_error("cancelled", result.get("last_error", "model download cancelled"))
        elif result.get("model_status") == "model_source_blocked":
            error = make_error(
                ERROR_MODEL_SOURCE_BLOCKED,
                result.get("last_error", "Model source blocked by test configuration"),
            )
        else:
            error = make_error(
                result.get("model_status") or result.get("download_status") or "download_failed",
                result.get("last_error", "model download failed"),
            )
        return make_response(request_id, ok, result, error), False
    if command == COMMAND_CANCEL:
        job_id = request["payload"]["job_id"]
        _mark_download_cancelled(job_id)
        return (
            make_response(request_id, True, {"job_id": job_id, "cancel": "accepted"}, None),
            False,
        )
    if command == COMMAND_CANCEL_DOWNLOAD:
        job_id = request["payload"]["job_id"]
        _mark_download_cancelled(job_id)
        return (
            make_response(
                request_id,
                True,
                {"job_id": job_id, "cancel": "accepted", "download_status": "cancelled"},
                None,
            ),
            False,
        )
    if command == COMMAND_WARMUP:
        payload = request["payload"]
        if payload["backend"] != "stub":
            return _backend_unavailable_response(
                request_id,
                payload["job_id"],
                payload["backend"],
                "auto",
                model_manager,
            ), False
        return (
            make_response(
                request_id,
                True,
                {
                    "job_id": payload["job_id"],
                    "backend": payload["backend"],
                    "model": "stub",
                    "warmup": "ready",
                    "quality": payload["quality"],
                    "timing_warmup_ms": "0",
                    **_visible_model_status_fields(model_manager),
                },
                None,
            ),
            False,
        )
    if command == COMMAND_INFER:
        if request["payload"]["backend"] != "stub":
            return _backend_unavailable_response(
                request_id,
                request["payload"].get("job_id", ""),
                request["payload"].get("backend", ""),
                request["payload"].get("screen_color", "auto"),
                model_manager,
            ), False
        try:
            payload = _with_backend_queue(
                lambda queue_time_ms: _fake_infer_with_downgrade(
                    request["payload"], queue_time_ms
                ),
            )
        except SyntheticOOMFailure as exc:
            return (
                make_response(
                    request_id,
                    False,
                    exc.diagnostics,
                    make_error(exc.code, exc.message),
                ),
                False,
            )
        except DiagnosticProtocolFailure as exc:
            exc.diagnostics.setdefault("job_id", request["payload"].get("job_id", ""))
            if not exc.diagnostics.get("requested_quality"):
                exc.diagnostics["requested_quality"] = request["payload"]["quality"]
            if not exc.diagnostics.get("effective_quality"):
                exc.diagnostics["effective_quality"] = request["payload"]["quality"]
            return (
                make_response(
                    request_id,
                    False,
                    exc.diagnostics,
                    make_error(exc.code, exc.message),
                ),
                False,
            )
        except ProtocolError as exc:
            diagnostics = _diagnostic_fields(
                request["payload"]["quality"],
                request["payload"]["quality"],
                "0",
                False,
                False,
                True,
            )
            diagnostics["job_id"] = request["payload"].get("job_id", "")
            return (
                make_response(
                    request_id,
                    False,
                    diagnostics,
                    make_error(exc.code, exc.message),
                ),
                False,
            )
        payload.update(_visible_model_status_fields(model_manager))
        return make_response(request_id, True, payload, None), False
    return (
        make_response(
            request_id,
            False,
            {},
            make_error(ERROR_UNKNOWN_COMMAND, "Unknown command"),
        ),
        False,
    )


def handle_line(line, model_manager=None):
    try:
        request = decode_request(line)
        try:
            return handle_request(request, model_manager)
        except ProtocolError as exc:
            exc.request_id = request["request_id"]
            raise
    except ProtocolError as exc:
        payload = _decode_error_diagnostics(line)
        return (
            make_response(
                exc.request_id,
                False,
                payload,
                make_error(exc.code, exc.message),
            ),
            False,
        )


def _decode_error_diagnostics(line):
    try:
        request = json.loads(line, parse_constant=_reject_json_constant)
    except (JSONDecodeError, ValueError, TypeError):
        return {}
    if not isinstance(request, dict) or request.get("command") != COMMAND_INFER:
        return {}
    payload = request.get("payload")
    if not isinstance(payload, dict):
        return {}
    quality = payload.get("quality")
    if quality not in QUALITIES:
        quality = ""
    diagnostics = _diagnostic_fields(quality, quality, "0", False, False, True)
    job_id = payload.get("job_id")
    if isinstance(job_id, str):
        diagnostics["job_id"] = job_id
    return diagnostics
