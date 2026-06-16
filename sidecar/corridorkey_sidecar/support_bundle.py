"""Create local, redacted CorridorKey support bundles."""

from __future__ import annotations

import argparse
import json
import platform
import re
import time
from pathlib import Path
from typing import Any, Iterable

from .logging_config import collect_diagnostic_status, missing_diagnostic_fields
from .redaction import redact_support_text, redact_value


SESSION21_SUPPORT_BUNDLE_EVIDENCE = (
    "private-artifacts/support-bundle-sufficiency.json"
)

_MINIMUM_FILES = (
    "manifest.json",
    "diagnostics.json",
    "doctor.txt",
    "logs/redacted.log",
    "manifest_status.json",
    "backend_status.json",
    "recent_errors.json",
    "redaction_proof.json",
)

_PROHIBITED_DIAGNOSTIC_KEYS = {
    "frame_content",
    "frame_contents",
    "frame_data",
    "image_data",
    "pixel_samples",
    "pixels",
    "raw_frame",
    "sampled_pixels",
    "thumbnail",
    "thumbnail_base64",
}

_PROHIBITED_TEXT_MARKERS = (
    "file://",
    "frame_contents",
    "frame_content",
    "framecontents",
    "framecontent",
    "sampled_pixels",
    "sampledpixels",
    "pixel_samples",
    "pixelsamples",
    "thumbnail_base64",
    "thumbnailbase64",
    "\"thumbnail\"",
)

def create_support_bundle(
    output_root: Path | str,
    *,
    logs: Iterable[str] | None = None,
    log_paths: Iterable[Path | str] | None = None,
    doctor_output: str = "",
    manifest_status: dict[str, Any] | None = None,
    backend_status: dict[str, Any] | None = None,
    recent_errors: list[dict[str, Any]] | None = None,
    diagnostics: dict[str, Any] | None = None,
    bundle_name: str | None = None,
) -> Path:
    root = Path(output_root)
    name = bundle_name or f"corridorkey-support-{time.strftime('%Y%m%d-%H%M%S')}"
    bundle_dir = root / name
    (bundle_dir / "logs").mkdir(parents=True, exist_ok=False)

    diagnostics = _sanitize_diagnostics(diagnostics or collect_diagnostic_status())
    manifest_status = redact_value(manifest_status or {})
    backend_status = redact_value(backend_status or {})
    recent_errors = redact_value(recent_errors or [])
    doctor_text = redact_support_text(doctor_output or "Doctor output unavailable")
    log_text = _redacted_logs(logs or [], log_paths or [])

    manifest = {
        "bundle_schema": "1",
        "created_unix_ms": str(int(time.time() * 1000)),
        "platform": platform.system() or "unknown",
        "minimum_files": list(_MINIMUM_FILES),
        "session21_evidence_artifact": SESSION21_SUPPORT_BUNDLE_EVIDENCE,
    }

    _write_json(bundle_dir / "manifest.json", manifest, redact=False)
    _write_json(bundle_dir / "diagnostics.json", diagnostics)
    _write_text(bundle_dir / "doctor.txt", doctor_text + "\n")
    _write_text(bundle_dir / "logs" / "redacted.log", log_text)
    _write_json(bundle_dir / "manifest_status.json", manifest_status)
    _write_json(bundle_dir / "backend_status.json", backend_status)
    _write_json(bundle_dir / "recent_errors.json", _recent_error_summary(recent_errors))
    _write_json(bundle_dir / "redaction_proof.json", _redaction_proof())

    return bundle_dir


def evaluate_support_bundle_sufficiency(bundle_dir: Path | str) -> dict[str, Any]:
    root = Path(bundle_dir)
    files = {relative: (root / relative).is_file() for relative in _MINIMUM_FILES}
    diagnostics = _read_json(root / "diagnostics.json")
    recent_errors = _read_json(root / "recent_errors.json")
    redaction_proof = _read_json(root / "redaction_proof.json")
    manifest = _read_json(root / "manifest.json")

    criteria = {
        "minimum_files": all(files.values()),
        "required_fields": not missing_diagnostic_fields(diagnostics),
        "redaction_proof": redaction_proof.get("project_paths_redacted") == "true"
        and redaction_proof.get("project_names_redacted") == "true"
        and redaction_proof.get("media_paths_redacted") == "true",
        "recent_error_summary": isinstance(recent_errors.get("errors"), list),
        "session21_evidence_artifact": manifest.get("session21_evidence_artifact")
        == SESSION21_SUPPORT_BUNDLE_EVIDENCE,
        "prohibited_content_absent": not _bundle_contains_prohibited_content(root),
    }
    return {
        "sufficient": all(criteria.values()),
        "criteria": criteria,
        "files": files,
        "session21_evidence_artifact": SESSION21_SUPPORT_BUNDLE_EVIDENCE,
    }


def _redacted_logs(logs: Iterable[str], log_paths: Iterable[Path | str]) -> str:
    chunks = [_redact_log_line(str(line)) for line in logs]
    for path_value in log_paths:
        try:
            raw_lines = Path(path_value).read_text(encoding="utf-8").splitlines()
            chunks.extend(_redact_log_line(line) for line in raw_lines)
        except OSError:
            chunks.append("log read failed: <redacted-path>")
    text = "\n".join(chunk for chunk in chunks if chunk)
    return text + ("\n" if text else "")


def _redact_log_line(line: str) -> str:
    try:
        parsed = json.loads(line)
    except json.JSONDecodeError:
        return redact_support_text(line)
    return json.dumps(redact_value(parsed), separators=(",", ":"), sort_keys=True)


def _sanitize_diagnostics(value: dict[str, Any]) -> dict[str, Any]:
    redacted = redact_value(value)
    sanitized = _drop_prohibited_diagnostic_keys(redacted)
    return sanitized if isinstance(sanitized, dict) else {}


def _drop_prohibited_diagnostic_keys(value: Any, key: str | None = None) -> Any:
    if key is not None and _normalize_key(key) in _PROHIBITED_DIAGNOSTIC_KEYS:
        return None
    if isinstance(value, dict):
        result = {}
        for item_key, item_value in value.items():
            if _normalize_key(item_key) in _PROHIBITED_DIAGNOSTIC_KEYS:
                continue
            result[item_key] = _drop_prohibited_diagnostic_keys(item_value, item_key)
        return result
    if isinstance(value, list):
        return [_drop_prohibited_diagnostic_keys(item, key) for item in value]
    return value


def _bundle_contains_prohibited_content(bundle_dir: Path) -> bool:
    for path in bundle_dir.rglob("*"):
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        lowered = text.lower()
        if any(marker in lowered for marker in _PROHIBITED_TEXT_MARKERS):
            return True
        leak_scan_text = _remove_allowed_support_paths(text)
        if _contains_redactable_leak(leak_scan_text):
            return True
    return False


def _contains_redactable_leak(text: str) -> bool:
    return any(redact_support_text(line) != line for line in text.splitlines())


def _remove_allowed_support_paths(text: str) -> str:
    for allowed in (*_MINIMUM_FILES, SESSION21_SUPPORT_BUNDLE_EVIDENCE):
        text = text.replace(allowed, "")
    return text


def _normalize_key(key: str) -> str:
    text = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", str(key))
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


def _recent_error_summary(recent_errors: list[dict[str, Any]]) -> dict[str, Any]:
    errors = []
    for item in recent_errors[-20:]:
        if not isinstance(item, dict):
            continue
        errors.append(
            redact_value(
                {
                    "error_code": item.get("error_code", item.get("code", "unknown")),
                    "message": item.get("message", item.get("last_error", "")),
                }
            )
        )
    return {"errors": errors}


def _redaction_proof() -> dict[str, str]:
    samples = {
        "project_paths_redacted": "/Users/alice/Projects/SecretShow/shot010/plate.exr",
        "project_names_redacted": {"project_name": "SecretShow"},
        "media_paths_redacted": r"C:\Users\alice\Projects\SecretShow\plate.mov",
    }
    return {
        "project_paths_redacted": _bool_text(
            redact_support_text(samples["project_paths_redacted"]) == "<redacted-path>"
        ),
        "project_names_redacted": _bool_text(
            redact_value(samples["project_names_redacted"])["project_name"] == "<redacted>"
        ),
        "media_paths_redacted": _bool_text(
            redact_support_text(samples["media_paths_redacted"]) == "<redacted-path>"
        ),
    }


def _write_json(path: Path, value: Any, *, redact: bool = True) -> None:
    serialized_value = redact_value(value) if redact else value
    path.write_text(
        json.dumps(serialized_value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _write_text(path: Path, value: str) -> None:
    path.write_text(redact_support_text(value), encoding="utf-8")


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def _bool_text(value: bool) -> str:
    return "true" if value else "false"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Create a redacted CorridorKey support bundle")
    parser.add_argument("--output", required=True, help="Directory where the bundle directory is created")
    parser.add_argument("--doctor-output", default="", help="Doctor output text to include")
    parser.add_argument("--log", action="append", default=[], help="Log file to redact and include")
    args = parser.parse_args(argv)

    bundle = create_support_bundle(
        Path(args.output),
        log_paths=[Path(value) for value in args.log],
        doctor_output=args.doctor_output,
        diagnostics=collect_diagnostic_status(),
        manifest_status={},
        backend_status={},
        recent_errors=[],
    )
    print(bundle.name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
