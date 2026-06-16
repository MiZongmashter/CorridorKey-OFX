"""Diagnostic record helpers for CorridorKey sidecar logs and support data."""

from __future__ import annotations

import json
import os
import platform
import time
from typing import Any

from .protocol import VERSION as SIDECAR_VERSION
from .redaction import redact_value


PLUGIN_VERSION = os.environ.get("CORRIDORKEY_PLUGIN_VERSION", "0.1.0")

REQUIRED_DIAGNOSTIC_FIELDS = (
    "plugin_version",
    "sidecar_version",
    "host",
    "os",
    "cpu_summary",
    "gpu_summary",
    "backend",
    "model",
    "pixel_format",
    "frame_size",
    "timings",
    "warning_codes",
    "error_codes",
)


def collect_diagnostic_status(
    *,
    plugin_version: str | None = None,
    sidecar_version: str | None = None,
    host_name: str | None = None,
    host_version: str | None = None,
    backend: str | None = None,
    model: str | None = None,
    pixel_format: str | None = None,
    frame_size: str | None = None,
    timings: dict[str, Any] | None = None,
    warning_codes: list[str] | tuple[str, ...] | None = None,
    error_codes: list[str] | tuple[str, ...] | None = None,
    gpu_summary: str | None = None,
) -> dict[str, Any]:
    host = {
        "name": host_name or os.environ.get("CORRIDORKEY_HOST_NAME", "unavailable"),
        "version": host_version or os.environ.get("CORRIDORKEY_HOST_VERSION", "unavailable"),
    }
    status = {
        "plugin_version": plugin_version or PLUGIN_VERSION,
        "sidecar_version": sidecar_version or SIDECAR_VERSION,
        "host": host,
        "os": {
            "system": platform.system() or "unknown",
            "release": platform.release() or "unknown",
            "machine": platform.machine() or "unknown",
        },
        "cpu_summary": _cpu_summary(),
        "gpu_summary": gpu_summary or os.environ.get("CORRIDORKEY_GPU_SUMMARY", "unknown"),
        "backend": backend or "unknown",
        "model": model or "unknown",
        "pixel_format": pixel_format or "unknown",
        "frame_size": frame_size or "unknown",
        "timings": timings or {},
        "warning_codes": list(warning_codes or []),
        "error_codes": list(error_codes or []),
    }
    return redact_value(status)


def diagnostic_record(level: str, event: str, **fields: Any) -> dict[str, Any]:
    record = {
        "level": level,
        "event": event,
        "timestamp_unix_ms": str(int(time.time() * 1000)),
    }
    record.update(fields)
    return redact_value(record)


def diagnostic_json_line(level: str, event: str, **fields: Any) -> str:
    return json.dumps(
        diagnostic_record(level, event, **fields),
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    )


def missing_diagnostic_fields(status: dict[str, Any]) -> list[str]:
    missing = [field for field in REQUIRED_DIAGNOSTIC_FIELDS if field not in status]
    for field in ("name", "version"):
        if not isinstance(status.get("host"), dict) or not status["host"].get(field):
            missing.append(f"host.{field}")
    for field in ("system", "release", "machine"):
        if not isinstance(status.get("os"), dict) or not status["os"].get(field):
            missing.append(f"os.{field}")
    return missing


def _cpu_summary() -> str:
    processor = platform.processor()
    machine = platform.machine()
    if processor and machine and processor != machine:
        return f"{processor} ({machine})"
    return processor or machine or "unknown"
