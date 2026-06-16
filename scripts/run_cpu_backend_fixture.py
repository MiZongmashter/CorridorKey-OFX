#!/usr/bin/env python3
"""Run one CorridorKey Torch CPU backend fixture and write diagnostics JSON."""

import argparse
import json
from pathlib import Path
import shutil
import sys
import tempfile
import time

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from sidecar.corridorkey_sidecar.model_manager import ModelManager
from sidecar.corridorkey_sidecar.protocol import COMMAND_INFER, request_hash_for_payload
from sidecar.corridorkey_sidecar.redaction import redact_value
from sidecar.corridorkey_sidecar.runtime_config import RuntimeConfig
from sidecar.corridorkey_sidecar.server import handle_line_with_model_manager


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    started = time.monotonic()
    config = RuntimeConfig.from_env()
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="corridorkey-render-cpu-fixture-") as tmp:
        temp_root = Path(tmp)
        source_path = temp_root / "source.ckfb"
        alpha_path = temp_root / "alpha.ckfb"
        preflight = _prepare_fixture_inputs(config, source_path, alpha_path)
        if preflight is not None:
            result = preflight
        else:
            payload = _infer_payload(source_path, alpha_path)
            response, should_shutdown = handle_line_with_model_manager(
                json.dumps(
                    {
                        "request_id": "req-cpu-backend-fixture",
                        "command": COMMAND_INFER,
                        "payload": payload,
                    }
                ),
                model_manager=ModelManager(config.model_dir),
            )
            result = {
                "ok": response["ok"],
                "status": "passed" if response["ok"] else "blocked_backend",
                "should_shutdown": should_shutdown,
                "response": response,
            }

    result.setdefault("ok", False)
    result.setdefault("status", "blocked_backend")
    response_payload = result.get("response", {}).get("payload", {})
    if isinstance(response_payload, dict) and response_payload.get("missing_runtime_paths"):
        result["missing_runtime_env_vars"] = response_payload["missing_runtime_paths"]
    result["backend"] = "torch_cpu"
    result["gpu_backends_enabled"] = "false"
    result["elapsed_ms"] = str(max(0, int(round((time.monotonic() - started) * 1000))))
    result["fixture_path_redaction_status"] = "redacted"
    result["visual_notes"] = (
        "CPU fixture produced contract frame blobs"
        if result["ok"]
        else "CPU fixture did not run; see blocked_backend diagnostic"
    )
    redacted_result = redact_value(result)
    redacted_text = json.dumps(redacted_result, indent=2, sort_keys=True)
    redacted_result["fixture_path_redaction_status"] = (
        "redacted" if _redaction_passed(redacted_text, config, temp_root) else "failed"
    )
    output_path.write_text(
        json.dumps(redacted_result, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    return 0


def _prepare_fixture_inputs(config, source_path, alpha_path):
    path_issues = config.required_fixture_path_issues()
    if path_issues:
        names = ",".join(issue.env_name for issue in path_issues)
        return _blocked(
            "CorridorKey CPU backend runtime paths are missing",
            {
                **config.status_fields(),
                "missing_runtime_paths": names,
                "last_error": "missing required runtime paths: " + names,
            },
        )
    if not config.fixture_runtime_enabled():
        return _blocked(
            "CorridorKey CPU backend fixture marker is missing",
            {
                **config.status_fields(),
                "last_error": "missing fixture marker: .corridorkey-backend-fixture",
            },
        )
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
        return _blocked(
            "CorridorKey CPU backend fixture files are missing",
            {
                **config.status_fields(),
                "missing_fixture_files": ",".join(missing),
                "last_error": "missing backend fixture files: " + ",".join(missing),
            },
        )
    shutil.copy2(fixture_source, source_path)
    shutil.copy2(fixture_alpha, alpha_path)
    return None


def _blocked(message, payload):
    return {
        "ok": False,
        "status": "blocked_backend",
        "response": {
            "request_id": "req-cpu-backend-fixture",
            "ok": False,
            "payload": payload,
            "error": {"code": "blocked_backend", "message": message},
        },
    }


def _infer_payload(source_path, alpha_path):
    payload = {
        "frame_id": "frame-cpu-backend-fixture",
        "job_id": "job-cpu-backend-fixture",
        "render_window_x1": "0",
        "render_window_y1": "0",
        "render_window_x2": "2",
        "render_window_y2": "1",
        "source_frame_blob_path": str(source_path),
        "alpha_hint_frame_blob_path": str(alpha_path),
        "alpha_hint_source": "external",
        "screen_color": "green",
        "quality": "high_1024",
        "input_color_space": "host_managed",
        "despill_strength": "5",
        "backend": "torch_cpu",
        "output_mode": "processed_rgba",
    }
    payload["request_hash"] = request_hash_for_payload(payload)
    return payload


def _redaction_passed(text, config, temp_root):
    sensitive_values = [
        config.corridorkey_repo,
        config.model_dir,
        config.backend_fixture_dir,
        temp_root,
        REPO_ROOT,
    ]
    return not any(str(value) in text for value in sensitive_values if value is not None)


if __name__ == "__main__":
    raise SystemExit(main())
