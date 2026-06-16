"""Deterministic fake-inference cache keys for the CorridorKey sidecar."""

from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path
import shutil
from typing import Dict, Optional, Tuple

from .protocol import request_hash_for_payload


MODEL_VERSION = "stub"
_FNV_OFFSET = 14695981039346656037
_FNV_PRIME = 1099511628211
_PARAM_FIELDS = (
    "alpha_hint_source",
    "render_window_x1",
    "render_window_y1",
    "render_window_x2",
    "render_window_y2",
    "screen_color",
    "input_color_space",
    "despill_strength",
    "output_mode",
)


@dataclass(frozen=True)
class CacheKey:
    frame: str
    source_hash: str
    alpha_hint_hash: str
    params_hash: str
    model_version: str
    backend: str
    quality: str


@dataclass
class CacheEntry:
    payload: Dict[str, str]
    key: CacheKey


def hash_bytes(data: bytes) -> str:
    hash_value = _FNV_OFFSET
    for byte in data:
        hash_value ^= byte
        hash_value = (hash_value * _FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return f"{hash_value:016x}"


def hash_frame_blob(path_text: str) -> str:
    hash_value = _FNV_OFFSET
    with Path(path_text).open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            for byte in chunk:
                hash_value ^= byte
                hash_value = (hash_value * _FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return f"{hash_value:016x}"


def params_hash_for_payload(payload: Dict[str, str]) -> str:
    params = {field: payload[field] for field in _PARAM_FIELDS}
    return request_hash_for_payload(params)


def cache_key_for_payload(payload: Dict[str, str], model_version: str = MODEL_VERSION) -> CacheKey:
    return CacheKey(
        frame=payload["frame_id"],
        source_hash=hash_frame_blob(payload["source_frame_blob_path"]),
        alpha_hint_hash=hash_frame_blob(payload["alpha_hint_frame_blob_path"]),
        params_hash=params_hash_for_payload(payload),
        model_version=model_version,
        backend=payload["backend"],
        quality=payload["quality"],
    )


class InferenceCache:
    def __init__(
        self,
        enabled: bool = True,
        model_version: str = MODEL_VERSION,
        max_entries: int = 128,
    ):
        self.enabled = enabled
        self.model_version = model_version
        self.max_entries = max(1, int(max_entries))
        self._entries: "OrderedDict[CacheKey, CacheEntry]" = OrderedDict()
        self._latest_by_frame: Dict[str, CacheKey] = {}

    def lookup(self, payload: Dict[str, str]) -> Tuple[str, Optional[Dict[str, str]], Optional[CacheKey]]:
        if not self.enabled:
            return "disabled", None, None
        key = cache_key_for_payload(payload, self.model_version)
        found = self._entries.get(key)
        if found is not None:
            self._entries.move_to_end(key)
            return "hit", self._materialize_hit(found.payload, payload), key
        return self.diagnostic_for_key(key), None, key

    def store(self, key: Optional[CacheKey], payload: Dict[str, str]) -> None:
        if not self.enabled or key is None:
            return
        if payload.get("downgraded_quality") == "true" or payload.get("oom") == "true":
            return
        cached_payload = dict(payload)
        cached_payload["cache"] = "hit"
        self._entries.pop(key, None)
        self._entries[key] = CacheEntry(cached_payload, key)
        self._latest_by_frame[key.frame] = key
        self._evict_oldest_entries()

    def diagnostic_for_key(self, key: CacheKey) -> str:
        previous = self._latest_by_frame.get(key.frame)
        if previous is None:
            return "miss"
        if previous == key:
            return "miss"
        if (
            previous.source_hash != key.source_hash
            or previous.alpha_hint_hash != key.alpha_hint_hash
        ):
            return "invalidated_input"
        return "invalidated_params"

    def _evict_oldest_entries(self) -> None:
        while len(self._entries) > self.max_entries:
            old_key, _entry = self._entries.popitem(last=False)
            if self._latest_by_frame.get(old_key.frame) == old_key:
                del self._latest_by_frame[old_key.frame]

    def _materialize_hit(
        self, cached_payload: Dict[str, str], request_payload: Dict[str, str]
    ) -> Dict[str, str]:
        materialized = dict(cached_payload)
        materialized["job_id"] = request_payload["job_id"]
        parts = materialized.get("deterministic_key", "").split(":")
        if len(parts) == 5:
            parts[3] = request_hash_for_payload(request_payload)
            materialized["deterministic_key"] = ":".join(parts)
        job_root = Path(request_payload["source_frame_blob_path"]).parent
        for key, suffix in (
            ("processed_rgba_frame_blob_path", "processed-rgba"),
            ("straight_fg_frame_blob_path", "straight-fg"),
            ("alpha_frame_blob_path", "alpha"),
        ):
            if key not in cached_payload:
                materialized.pop(key, None)
                continue
            source = Path(cached_payload[key])
            target = job_root / f"cache-hit-{hash_bytes(str(source).encode('utf-8'))}-{suffix}.ckfb"
            if source != target:
                shutil.copyfile(source, target)
            materialized[key] = str(target)
        materialized["cache"] = "hit"
        return materialized
