#!/usr/bin/env python3
"""Run one GPU backend validation fixture or write a blocked diagnostic."""

import argparse
import json
import platform
from pathlib import Path
import shutil
import sys
import tempfile
import time

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from sidecar.corridorkey_sidecar.backends.torch_backend import _torch_mps_runtime_status
from sidecar.corridorkey_sidecar.model_manager import ModelManager
from sidecar.corridorkey_sidecar.protocol import COMMAND_INFER, request_hash_for_payload
from sidecar.corridorkey_sidecar.redaction import redact_text
from sidecar.corridorkey_sidecar.runtime_config import RuntimeConfig
from sidecar.corridorkey_sidecar.server import handle_line_with_model_manager


TARGET_ROW = "ck_resolve20_torch_mps"
PARITY_THRESHOLDS = {
    "alpha_max_abs": 0.015,
    "alpha_mean_abs": 0.005,
    "processed_rgba_max_abs": 0.025,
    "processed_rgba_mean_abs": 0.010,
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", default="torch_mps", choices=("torch_mps",))
    parser.add_argument(
        "--output",
        default="build/backend-validation/torch-mps-fixture.json",
    )
    args = parser.parse_args()

    started = time.monotonic()
    config = RuntimeConfig.from_env()
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    model_manager = ModelManager(config.model_dir)

    result = {
        "ok": False,
        "status": "blocked_backend",
        "target_row": TARGET_ROW,
        "backend": args.backend,
        "backend_requested": args.backend,
        "torch_mps_enabled": "false",
        "gpu_backends_enabled": "false",
        "python": {
            "version": sys.version.split()[0],
            "executable": sys.executable,
            "platform": platform.platform(),
        },
        "runtime_preflight": _torch_mps_runtime_status(),
        "model_preflight": _model_preflight(model_manager),
        "parity_thresholds": PARITY_THRESHOLDS,
        "parity": {
            "status": "not_run",
            "reason": "torch_mps is blocked before inference",
        },
        "real_oom": {
            "status": "not_run",
            "reason": "real MPS OOM evidence cannot be collected while torch_mps is blocked before inference",
        },
        "cpu_fallback": {"status": "not_run"},
        "cases": [],
    }

    with tempfile.TemporaryDirectory(prefix="corridorkey-render-gpu-fixture-") as tmp:
        temp_root = Path(tmp)
        source_path = temp_root / "source.ckfb"
        alpha_path = temp_root / "alpha.ckfb"
        preflight = _prepare_fixture_inputs(config, source_path, alpha_path)
        result["fixture_preflight"] = preflight or {"status": "ready"}
        if preflight is None:
            result["cpu_fallback"] = _run_case(
                model_manager,
                source_path,
                alpha_path,
                backend="torch_cpu",
                screen_color="green",
                quality="high_1024",
                despill="5",
            )
            for case in _case_matrix():
                result["cases"].append(
                    _run_case(
                        model_manager,
                        source_path,
                        alpha_path,
                        backend=args.backend,
                        screen_color=case["screen_color"],
                        quality=case["quality"],
                        despill=case["despill"],
                        case_name=case["name"],
                    )
                )

        result["blocker"] = _blocker_reason(result)
        result["elapsed_ms"] = str(max(0, int(round((time.monotonic() - started) * 1000))))
        result["fixture_path_redaction_status"] = "redacted"
        redacted = _redact_artifact_paths(result, _sensitive_values(config, temp_root))
        redacted_text = json.dumps(redacted, indent=2, sort_keys=True)
        redacted["fixture_path_redaction_status"] = (
            "redacted"
            if _redaction_passed(redacted_text, config, temp_root, output_path)
            else "failed"
        )
        output_path.write_text(
            json.dumps(redacted, indent=2, sort_keys=True),
            encoding="utf-8",
        )
    return 0


def _model_preflight(model_manager):
    return {
        "torch_cpu_green": model_manager.select_backend_model("green", "torch_cpu"),
        "torch_cpu_blue": model_manager.select_backend_model("blue", "torch_cpu"),
        "torch_mps_green": model_manager.select_backend_model("green", "torch_mps"),
        "torch_mps_blue": model_manager.select_backend_model("blue", "torch_mps"),
    }


def _prepare_fixture_inputs(config, source_path, alpha_path):
    path_issues = config.required_fixture_path_issues()
    if path_issues:
        names = ",".join(issue.env_name for issue in path_issues)
        return {
            "status": "blocked_backend",
            "missing_runtime_paths": names,
            "last_error": "missing required runtime paths: " + names,
        }
    if not config.fixture_runtime_enabled():
        return {
            "status": "blocked_backend",
            "last_error": "missing fixture marker: .corridorkey-backend-fixture",
        }
    fixture_source = config.backend_fixture_dir / "source.ckfb"
    fixture_alpha = config.backend_fixture_dir / "alpha.ckfb"
    missing = [
        name
        for name, path in (
            ("source.ckfb", fixture_source),
            ("alpha.ckfb", fixture_alpha),
        )
        if not path.is_file()
    ]
    if missing:
        return {
            "status": "blocked_backend",
            "missing_fixture_files": ",".join(missing),
            "last_error": "missing backend fixture files: " + ",".join(missing),
        }
    shutil.copy2(fixture_source, source_path)
    shutil.copy2(fixture_alpha, alpha_path)
    return None


def _case_matrix():
    return (
        {"name": "green_screen", "screen_color": "green", "quality": "high_1024", "despill": "5"},
        {"name": "blue_screen", "screen_color": "blue", "quality": "high_1024", "despill": "5"},
        {"name": "quality_draft_512", "screen_color": "green", "quality": "draft_512", "despill": "5"},
        {"name": "quality_high_1024", "screen_color": "green", "quality": "high_1024", "despill": "5"},
        {"name": "quality_full_2048", "screen_color": "green", "quality": "full_2048", "despill": "5"},
        {"name": "despill_strength_0", "screen_color": "green", "quality": "high_1024", "despill": "0"},
        {"name": "despill_strength_10", "screen_color": "green", "quality": "high_1024", "despill": "10"},
    )


def _run_case(
    model_manager,
    source_path,
    alpha_path,
    *,
    backend,
    screen_color,
    quality,
    despill,
    case_name="cpu_fallback_green",
):
    payload = _infer_payload(
        source_path,
        alpha_path,
        backend=backend,
        screen_color=screen_color,
        quality=quality,
        despill=despill,
    )
    response, should_shutdown = handle_line_with_model_manager(
        json.dumps(
            {
                "request_id": f"req-gpu-fixture-{case_name}",
                "command": COMMAND_INFER,
                "payload": payload,
            }
        ),
        model_manager=model_manager,
    )
    response_payload = response.get("payload", {})
    error = response.get("error") or {}
    return {
        "case": case_name,
        "ok": response["ok"],
        "status": "passed" if response["ok"] else error.get("code", "failed"),
        "backend_requested": backend,
        "requested_backend": response_payload.get("requested_backend", backend),
        "response_backend": response_payload.get("backend", backend),
        "effective_backend": response_payload.get("effective_backend", ""),
        "backend_status": response_payload.get("backend_status", ""),
        "gpu_backends_enabled": response_payload.get("gpu_backends_enabled", ""),
        "backend_gate": response_payload.get("backend_gate", ""),
        "parity_evidence": response_payload.get("parity_evidence", ""),
        "real_oom_evidence": response_payload.get("real_oom_evidence", ""),
        "screen_color": screen_color,
        "quality": quality,
        "despill_strength": despill,
        "error_code": error.get("code", ""),
        "last_error": response_payload.get("last_error", error.get("message", "")),
        "model_status": response_payload.get("model_status", ""),
        "model_id": response_payload.get("model_id", ""),
        "model_version": response_payload.get("model_version", ""),
        "should_shutdown": should_shutdown,
    }


def _infer_payload(source_path, alpha_path, *, backend, screen_color, quality, despill):
    payload = {
        "frame_id": f"frame-gpu-fixture-{backend}-{screen_color}-{quality}-{despill}",
        "job_id": f"job-gpu-fixture-{backend}-{screen_color}-{quality}-{despill}",
        "render_window_x1": "0",
        "render_window_y1": "0",
        "render_window_x2": "2",
        "render_window_y2": "1",
        "source_frame_blob_path": str(source_path),
        "alpha_hint_frame_blob_path": str(alpha_path),
        "alpha_hint_source": "external",
        "screen_color": screen_color,
        "quality": quality,
        "input_color_space": "host_managed",
        "despill_strength": despill,
        "backend": backend,
        "output_mode": "processed_rgba",
    }
    payload["request_hash"] = request_hash_for_payload(payload)
    return payload


def _blocker_reason(result):
    runtime = result["runtime_preflight"]
    if runtime.get("torch_importable") != "true":
        return "PyTorch is unavailable in this Python runtime"
    if runtime.get("torch_mps_available") != "true":
        return "PyTorch MPS is unavailable in this Python runtime"
    model_preflight = result["model_preflight"]
    green = model_preflight["torch_mps_green"]
    blue = model_preflight["torch_mps_blue"]
    if green.get("model_status") != "ready" or blue.get("model_status") != "ready":
        return "Installed model manifests do not declare torch_mps compatibility"
    if result.get("fixture_preflight", {}).get("status") == "blocked_backend":
        return result["fixture_preflight"].get("last_error", "fixture preflight blocked")
    return "torch_mps remains disabled until parity and real OOM evidence are collected"


def _sensitive_values(config, temp_root):
    return (
        config.corridorkey_repo,
        config.model_dir,
        config.backend_fixture_dir,
        temp_root,
        REPO_ROOT,
    )


def _redact_artifact_paths(value, sensitive_values, key=""):
    if isinstance(value, dict):
        return {
            item_key: _redact_artifact_paths(item_value, sensitive_values, item_key)
            for item_key, item_value in value.items()
        }
    if isinstance(value, list):
        return [_redact_artifact_paths(item, sensitive_values, key) for item in value]
    if isinstance(value, tuple):
        return [_redact_artifact_paths(item, sensitive_values, key) for item in value]
    if isinstance(value, str):
        normalized = key.lower()
        if normalized.endswith(("path", "paths", "dir", "dirs", "executable")):
            return "<redacted-path>" if value else value
        redacted = value
        for sensitive in sensitive_values:
            text = str(sensitive)
            if text:
                redacted = redacted.replace(text, "<redacted-path>")
        if _is_error_text_key(normalized):
            return redact_text(redacted)
        return redacted
    return value


def _is_error_text_key(key):
    return key in {
        "blocker",
        "last_error",
        "reason",
        "torch_import_error",
    } or key.endswith("_error")


def _redaction_passed(text, config, temp_root, output_path):
    sensitive_values = list(_sensitive_values(config, temp_root)) + [output_path]
    return not any(str(value) in text for value in sensitive_values if value is not None)


if __name__ == "__main__":
    raise SystemExit(main())
