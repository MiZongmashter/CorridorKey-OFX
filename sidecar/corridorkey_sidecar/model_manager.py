"""Local model manifest, checksum, and offline install helpers."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import urllib.error
import urllib.parse
import urllib.request
import time
import string

from .model_source_gate import ModelSourceGate


MANIFEST_NAME = "model-manifest.json"
FIXTURE_MARKER_NAME = ".corridorkey-test-fixture"
MAX_FIXTURE_FILE_BYTES = 1024 * 1024
DOWNLOADS_DIR_NAME = ".downloads"
DOWNLOAD_STATE_NAME = "download-state.json"
DEFAULT_DOWNLOAD_CHUNK_BYTES = 64 * 1024
MAX_DOWNLOAD_MANIFEST_BYTES = 256 * 1024
MAX_DOWNLOAD_MARKER_BYTES = 1024
STALE_DOWNLOAD_SECONDS = 30.0
_HEX_DIGITS = frozenset(string.hexdigits)


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class ModelManagerError(Exception):
    def __init__(self, code, message, fields=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.fields = {} if fields is None else dict(fields)


def _sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _is_safe_component(value):
    path = Path(value)
    return (
        isinstance(value, str)
        and bool(value)
        and not path.is_absolute()
        and len(path.parts) == 1
        and path.parts[0] not in (".", "..")
    )


def _read_manifest(root):
    manifest_path = Path(root) / MANIFEST_NAME
    try:
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ModelManagerError("invalid_manifest", "model manifest is invalid") from exc
    if not isinstance(data, dict):
        raise ModelManagerError("invalid_manifest", "model manifest must be an object")

    required_strings = ("model_id", "version", "screen_color")
    for key in required_strings:
        if not isinstance(data.get(key), str) or not data[key]:
            raise ModelManagerError("invalid_manifest", f"model manifest field {key} is invalid")
    if not _is_safe_component(data["model_id"]) or not _is_safe_component(data["version"]):
        raise ModelManagerError("invalid_manifest", "model manifest identity is invalid")
    if not isinstance(data.get("backend_compatibility"), list) or not all(
        isinstance(item, str) and item for item in data["backend_compatibility"]
    ):
        raise ModelManagerError("invalid_manifest", "model manifest backend compatibility is invalid")
    if not isinstance(data.get("expected_files"), list) or not data["expected_files"]:
        raise ModelManagerError("invalid_manifest", "model manifest expected files are invalid")
    for item in data["expected_files"]:
        if not isinstance(item, dict):
            raise ModelManagerError("invalid_manifest", "model manifest expected file is invalid")
        rel_path = item.get("path")
        checksum = item.get("sha256")
        decoded_rel_path = urllib.parse.unquote(rel_path) if isinstance(rel_path, str) else ""
        if (
            not isinstance(rel_path, str)
            or not rel_path
            or decoded_rel_path != rel_path
            or urllib.parse.urlparse(rel_path).scheme
            or Path(rel_path).is_absolute()
            or ".." in Path(rel_path).parts
            or not isinstance(checksum, str)
            or len(checksum) != 64
            or any(char not in _HEX_DIGITS for char in checksum)
        ):
            raise ModelManagerError("invalid_manifest", "model manifest expected file is invalid")
    return data


def _manifest_fields(manifest):
    return {
        "model_id": manifest["model_id"],
        "model": manifest["version"],
        "model_version": manifest["version"],
        "model_checksum": _manifest_checksum(manifest),
        "screen_color": manifest["screen_color"],
        "backend_compatibility": ",".join(manifest["backend_compatibility"]),
    }


def _manifest_checksum(manifest):
    checksum_payload = {
        "model_id": manifest["model_id"],
        "version": manifest["version"],
        "screen_color": manifest["screen_color"],
        "backend_compatibility": list(manifest["backend_compatibility"]),
        "expected_files": sorted(
            (
                {"path": item["path"], "sha256": item["sha256"]}
                for item in manifest["expected_files"]
            ),
            key=lambda item: item["path"],
        ),
    }
    return hashlib.sha256(
        json.dumps(checksum_payload, separators=(",", ":"), sort_keys=True).encode(
            "utf-8"
        )
    ).hexdigest()


def _validated_file_path(root, rel_path):
    package_root = Path(root).resolve(strict=True)
    path = package_root / rel_path
    if path.is_symlink():
        raise ModelManagerError(
            "model_file_unsafe",
            f"model file is unsafe: {rel_path}",
            {"unsafe_file": rel_path},
        )
    try:
        resolved = path.resolve(strict=True)
        resolved.relative_to(package_root)
    except (OSError, ValueError) as exc:
        raise ModelManagerError(
            "model_file_unsafe",
            f"model file is unsafe: {rel_path}",
            {"unsafe_file": rel_path},
        ) from exc
    if not resolved.is_file():
        raise ModelManagerError(
            "model_missing_file",
            f"model file missing: {rel_path}",
            {"missing_file": rel_path},
        )
    return resolved


def _validate_files(root, manifest):
    for item in manifest["expected_files"]:
        rel_path = item["path"]
        path = _validated_file_path(root, rel_path)
        actual = _sha256(path)
        if actual != item["sha256"]:
            raise ModelManagerError(
                "checksum_mismatch",
                "model checksum mismatch",
                {"corrupt_file": rel_path},
            )


def _validate_fixture_files(root, manifest):
    for item in manifest["expected_files"]:
        rel_path = item["path"]
        path = _validated_file_path(root, rel_path)
        if path.stat().st_size > MAX_FIXTURE_FILE_BYTES:
            raise ModelManagerError(
                "fixture_too_large",
                f"test fixture file is too large: {rel_path}",
                {"oversized_file": rel_path},
            )


def _read_limited_url(url, max_bytes):
    try:
        with _open_local_url(url) as response:
            data = response.read(max_bytes + 1)
    except (OSError, urllib.error.URLError, ValueError) as exc:
        raise ModelManagerError("download_failed", "model download source is unavailable") from exc
    if len(data) > max_bytes:
        raise ModelManagerError("download_failed", "model download metadata is too large")
    return data


def _open_local_url(url_or_request):
    url = url_or_request.full_url if isinstance(url_or_request, urllib.request.Request) else url_or_request
    if not _is_local_download_url(url):
        raise ModelManagerError("download_failed", "model download source is not local")
    try:
        response = urllib.request.build_opener(_NoRedirectHandler).open(url_or_request, timeout=5)
    except urllib.error.HTTPError as exc:
        if 300 <= exc.code < 400:
            raise ModelManagerError("download_failed", "model download redirected outside local source") from exc
        raise
    final_url = response.geturl() if hasattr(response, "geturl") else url
    if not _is_local_download_url(final_url):
        try:
            response.close()
        except Exception:
            pass
        raise ModelManagerError("download_failed", "model download redirected outside local source")
    return response


def _is_local_download_url(url):
    if not isinstance(url, str):
        return False
    try:
        parsed = urllib.parse.urlparse(url)
    except ValueError:
        return False
    if parsed.scheme == "file":
        return parsed.hostname in (None, "", "localhost")
    if parsed.scheme not in ("http", "https"):
        return False
    return parsed.hostname in ("127.0.0.1", "localhost", "::1")


def _file_url_path(url):
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "file":
        return None
    return Path(urllib.request.url2pathname(parsed.path))


def _download_fault_limit(env_name):
    if os.environ.get("CORRIDORKEY_TEST_FAULTS_ALLOWED") != "1":
        return None
    try:
        value = int(os.environ.get(env_name, ""))
    except ValueError:
        return None
    return value if value >= 0 else None


def _download_chunk_delay_seconds():
    if os.environ.get("CORRIDORKEY_TEST_FAULTS_ALLOWED") != "1":
        return 0.0
    try:
        delay_ms = int(os.environ.get("CORRIDORKEY_TEST_DOWNLOAD_CHUNK_DELAY_MS", "0"))
    except ValueError:
        return 0.0
    return max(0, delay_ms) / 1000.0


class ModelManager:
    def __init__(self, root, model_source_gate=None, download_chunk_bytes=DEFAULT_DOWNLOAD_CHUNK_BYTES):
        self.root = Path(root)
        self.model_source_gate = model_source_gate or ModelSourceGate.local_development()
        self.download_chunk_bytes = max(1, int(download_chunk_bytes))

    def _model_root(self, model_id, version):
        return self.root / model_id / version

    def _safe_model_root(self, model_id, version):
        target = self._model_root(model_id, version)
        for path in (self.root / model_id, target):
            if path.is_symlink():
                raise ModelManagerError("install_path_unsafe", "model install path is unsafe")
        try:
            root = self.root.resolve(strict=False)
            target.resolve(strict=False).relative_to(root)
        except ValueError as exc:
            raise ModelManagerError("install_path_unsafe", "model install path is unsafe") from exc
        return target

    def _base_status(self):
        return {
            "model": "not_loaded",
            "model_version": "not_loaded",
            "model_id": "",
            "model_checksum": "",
            "model_status": "missing",
            "install_status": "not_installed",
            "download_status": "not_started",
            "download_progress": "0/0",
            "downloaded_bytes": "0",
            "download_total_bytes": "0",
            "screen_color": "",
            "backend_compatibility": "",
            "last_error": "",
            **self.model_source_gate.status_fields(),
        }

    def _fixture_allowed(self, root):
        marker = Path(root) / FIXTURE_MARKER_NAME
        if not marker.is_file():
            return False
        try:
            return marker.read_text(encoding="utf-8").strip() == "fixture-only"
        except OSError:
            return False

    def _validate_fixture_constraints(self, root, manifest):
        if self._fixture_allowed(root):
            _validate_fixture_files(root, manifest)

    def _download_root(self, manifest_url):
        digest = hashlib.sha256(str(manifest_url).encode("utf-8")).hexdigest()[:24]
        return self.root / DOWNLOADS_DIR_NAME / digest

    def _download_state_path(self, download_root):
        return Path(download_root) / DOWNLOAD_STATE_NAME

    def _write_download_state(self, download_root, fields):
        state = {
            key: str(value)
            for key, value in fields.items()
            if isinstance(key, str)
            and key
            and isinstance(value, (str, int))
        }
        try:
            Path(download_root).mkdir(parents=True, exist_ok=True)
            if state.get("download_status") == "downloading":
                state.setdefault("download_updated_at", f"{time.time():.3f}")
            self._download_state_path(download_root).write_text(
                json.dumps(state, sort_keys=True), encoding="utf-8"
            )
        except OSError:
            return

    def _latest_download_state(self):
        downloads_root = self.root / DOWNLOADS_DIR_NAME
        if not downloads_root.exists():
            return {}
        states = []
        for state_path in downloads_root.glob("*/" + DOWNLOAD_STATE_NAME):
            try:
                states.append((state_path.stat().st_mtime, state_path))
            except OSError:
                continue
        if not states:
            return {}
        state_path = max(states)[1]
        try:
            data = json.loads(state_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return {}
        if not isinstance(data, dict):
            return {}
        state = {key: str(value) for key, value in data.items() if isinstance(key, str)}
        if state.get("download_status") == "downloading":
            try:
                updated_at = float(state.get("download_updated_at", "0"))
            except ValueError:
                updated_at = 0.0
            if updated_at <= 0.0 or time.time() - updated_at > STALE_DOWNLOAD_SECONDS:
                state["download_status"] = "retryable_failure"
                state["install_status"] = "failed"
                state["last_error"] = "model download interrupted"
        return state

    def _download_result(self, job_id, manifest=None, fields=None, persist_root=None):
        result = self._base_status()
        if manifest is not None:
            result.update(_manifest_fields(manifest))
        result["job_id"] = job_id
        if fields:
            result.update(fields)
        if persist_root is not None:
            self._write_download_state(persist_root, result)
        return result

    def _safe_download_path(self, download_root, rel_path, suffix=""):
        path = Path(rel_path)
        if path.is_absolute() or ".." in path.parts:
            raise ModelManagerError("download_path_unsafe", "model download path is unsafe")
        target = Path(download_root) / rel_path
        if suffix:
            target = target.with_name(target.name + suffix)
        try:
            resolved_root = Path(download_root).resolve(strict=False)
            target.resolve(strict=False).relative_to(resolved_root)
        except ValueError as exc:
            raise ModelManagerError("download_path_unsafe", "model download path is unsafe") from exc
        return target

    def _copy_url_to_part(self, source_url, part_path, cancel_requested=None):
        if cancel_requested is not None and cancel_requested():
            raise ModelManagerError("cancelled", "model download cancelled")

        part_path = Path(part_path)
        part_path.parent.mkdir(parents=True, exist_ok=True)
        existing_bytes = part_path.stat().st_size if part_path.exists() else 0
        file_source = _file_url_path(source_url)
        interrupted_after = _download_fault_limit("CORRIDORKEY_TEST_DOWNLOAD_FAIL_AFTER_BYTES")
        write_failed_after = _download_fault_limit(
            "CORRIDORKEY_TEST_DOWNLOAD_WRITE_FAILURE_AFTER_BYTES"
        )
        session_written = 0

        try:
            if file_source is not None:
                source_handle = file_source.open("rb")
                source_handle.seek(existing_bytes)
                response_handle = source_handle
                append = True
            else:
                headers = {}
                if existing_bytes > 0:
                    headers["Range"] = f"bytes={existing_bytes}-"
                request = urllib.request.Request(source_url, headers=headers)
                try:
                    response_handle = _open_local_url(request)
                except urllib.error.HTTPError as exc:
                    if exc.code == 416 and part_path.exists():
                        part_path.unlink()
                        return self._copy_url_to_part(source_url, part_path, cancel_requested)
                    raise
                status = getattr(response_handle, "status", 200)
                append = existing_bytes == 0 or status == 206
                if not append:
                    existing_bytes = 0

            mode = "ab" if append else "wb"
            with response_handle:
                with part_path.open(mode) as output:
                    while True:
                        if cancel_requested is not None and cancel_requested():
                            raise ModelManagerError("cancelled", "model download cancelled")
                        chunk = response_handle.read(self.download_chunk_bytes)
                        if not chunk:
                            break

                        for limit, code, message in (
                            (
                                write_failed_after,
                                "write_failed",
                                "model download write failed",
                            ),
                            (
                                interrupted_after,
                                "download_interrupted",
                                "model download interrupted",
                            ),
                        ):
                            if limit is not None and session_written + len(chunk) > limit:
                                allowed = max(0, limit - session_written)
                                if allowed:
                                    output.write(chunk[:allowed])
                                    session_written += allowed
                                raise ModelManagerError(code, message)

                        output.write(chunk)
                        session_written += len(chunk)
                        delay = _download_chunk_delay_seconds()
                        if delay > 0:
                            time.sleep(delay)
        except ModelManagerError:
            raise
        except (OSError, urllib.error.URLError, ValueError) as exc:
            raise ModelManagerError("write_failed", "model download write failed") from exc

        return existing_bytes + session_written

    def _download_expected_file(
        self, manifest_url, download_root, item, cancel_requested=None, retry_corrupt=True
    ):
        rel_path = item["path"]
        source_url = urllib.parse.urljoin(manifest_url, rel_path)
        part_path = self._safe_download_path(download_root, rel_path, ".part")
        final_path = self._safe_download_path(download_root, rel_path)
        if part_path.exists():
            try:
                if _sha256(part_path) == item["sha256"]:
                    downloaded = part_path.stat().st_size
                    final_path.parent.mkdir(parents=True, exist_ok=True)
                    part_path.replace(final_path)
                    return downloaded
            except OSError as exc:
                raise ModelManagerError("write_failed", "model download write failed") from exc
        downloaded = self._copy_url_to_part(source_url, part_path, cancel_requested)
        actual = _sha256(part_path)
        if actual != item["sha256"]:
            try:
                part_path.unlink()
            except OSError:
                pass
            if retry_corrupt:
                return self._download_expected_file(
                    manifest_url,
                    download_root,
                    item,
                    cancel_requested,
                    retry_corrupt=False,
                )
            raise ModelManagerError(
                "checksum_mismatch",
                "model checksum mismatch",
                {"corrupt_file": rel_path},
            )
        final_path.parent.mkdir(parents=True, exist_ok=True)
        part_path.replace(final_path)
        return downloaded

    def _partial_download_bytes(self, download_root):
        total = 0
        try:
            part_files = list(Path(download_root).glob("**/*.part"))
        except OSError:
            return 0
        for part_path in part_files:
            try:
                total += part_path.stat().st_size
            except OSError:
                continue
        return total

    def status(self, model_id=None, version=None):
        result = self._base_status()
        candidates = []
        if model_id and version:
            candidates.append(self._model_root(model_id, version))
        elif self.root.is_dir():
            for manifest_path in sorted(self.root.glob("*/*/" + MANIFEST_NAME)):
                try:
                    if manifest_path.relative_to(self.root).parts[0] == DOWNLOADS_DIR_NAME:
                        continue
                except ValueError:
                    continue
                candidates.append(manifest_path.parent)

        if not candidates:
            result.update(self._latest_download_state())
            return result

        model_root = candidates[0]
        try:
            manifest = _read_manifest(model_root)
            result.update(_manifest_fields(manifest))
            if not self.model_source_gate.allows_manifest(
                manifest, fixture_allowed=self._fixture_allowed(model_root)
            ):
                result.update(
                    {
                        "model_status": "model_source_blocked",
                        "install_status": "blocked",
                        "last_error": result["model_source_blocker"],
                    }
                )
                return result
            self._validate_fixture_constraints(model_root, manifest)
            _validate_files(model_root, manifest)
        except ModelManagerError as exc:
            result.update(exc.fields)
            result["model_status"] = "checksum_mismatch" if exc.code == "checksum_mismatch" else "missing"
            result["install_status"] = "failed"
            result["last_error"] = exc.message
            return result

        result["model_status"] = "ready"
        result["install_status"] = "ready"
        result["download_status"] = "ready"
        result["download_progress"] = "complete"
        latest_download = self._latest_download_state()
        if latest_download.get("download_status") in (
            "cancelled",
            "downloading",
            "retryable_failure",
        ):
            for key in (
                "download_status",
                "download_progress",
                "downloaded_bytes",
                "download_total_bytes",
                "last_error",
            ):
                if key in latest_download:
                    result[key] = latest_download[key]
        return result

    def select_backend_model(self, screen_color, backend):
        result = self._base_status()
        candidates = []
        if self.root.is_dir():
            for manifest_path in sorted(self.root.glob("*/*/" + MANIFEST_NAME)):
                try:
                    if manifest_path.relative_to(self.root).parts[0] == DOWNLOADS_DIR_NAME:
                        continue
                except ValueError:
                    continue
                candidates.append(manifest_path.parent)

        requested_color = screen_color
        compatible = []
        for model_root in candidates:
            try:
                manifest = _read_manifest(model_root)
            except ModelManagerError:
                continue
            if requested_color != "auto" and manifest["screen_color"] != requested_color:
                continue
            if backend not in manifest["backend_compatibility"]:
                continue
            compatible.append((model_root, manifest))

        if requested_color == "auto":
            compatible.sort(
                key=lambda item: (
                    item[1]["screen_color"] != "green",
                    item[1]["model_id"],
                    item[1]["version"],
                )
            )

        for model_root, manifest in compatible:
            result.update(_manifest_fields(manifest))
            if not self.model_source_gate.allows_manifest(
                manifest, fixture_allowed=self._fixture_allowed(model_root)
            ):
                result.update(
                    {
                        "model_status": "model_source_blocked",
                        "install_status": "blocked",
                        "last_error": result["model_source_blocker"],
                    }
                )
                return result
            try:
                self._validate_fixture_constraints(model_root, manifest)
                _validate_files(model_root, manifest)
            except ModelManagerError as exc:
                result.update(exc.fields)
                result["model_status"] = (
                    "checksum_mismatch" if exc.code == "checksum_mismatch" else "missing"
                )
                result["install_status"] = "failed"
                result["last_error"] = exc.message
                return result
            result["model_status"] = "ready"
            result["install_status"] = "ready"
            result["download_status"] = "ready"
            result["download_progress"] = "complete"
            return result

        result.update(
            {
                "model_status": "missing",
                "install_status": "not_installed",
                "last_error": f"no {backend} model installed for screen_color {requested_color}",
            }
        )
        return result

    def download_model(self, manifest_url, job_id, cancel_requested=None):
        download_root = self._download_root(manifest_url)
        if not _is_safe_component(job_id):
            # Protocol-level validation owns the public job-id shape; direct callers still
            # get a safe failure instead of a path-bearing exception.
            return self._download_result(
                "",
                fields={
                    "model_status": "missing",
                    "install_status": "failed",
                    "download_status": "retryable_failure",
                    "last_error": "model download job id is invalid",
                },
                persist_root=download_root,
            )

        try:
            if cancel_requested is not None and cancel_requested():
                raise ModelManagerError("cancelled", "model download cancelled")
            if not _is_local_download_url(manifest_url):
                fields = {
                    "model_status": "missing",
                    "install_status": "not_installed",
                    "download_status": "retryable_failure",
                    "last_error": "model download source must be a local file or loopback URL",
                }
                return self._download_result(job_id, fields=fields, persist_root=download_root)

            download_root.mkdir(parents=True, exist_ok=True)
            manifest_bytes = _read_limited_url(manifest_url, MAX_DOWNLOAD_MANIFEST_BYTES)
            (download_root / MANIFEST_NAME).write_bytes(manifest_bytes)
            marker_url = urllib.parse.urljoin(manifest_url, FIXTURE_MARKER_NAME)
            try:
                marker = _read_limited_url(marker_url, MAX_DOWNLOAD_MARKER_BYTES)
            except ModelManagerError:
                marker = b""
            if marker.decode("utf-8", errors="replace").strip() == "fixture-only":
                (download_root / FIXTURE_MARKER_NAME).write_text(
                    "fixture-only\n", encoding="utf-8"
                )

            manifest = _read_manifest(download_root)
            if not self.model_source_gate.allows_manifest(
                manifest, fixture_allowed=self._fixture_allowed(download_root)
            ):
                fields = {
                    "model_status": "model_source_blocked",
                    "install_status": "blocked",
                    "download_status": "blocked",
                    "last_error": self.model_source_gate.reason,
                }
                return self._download_result(
                    job_id, manifest, fields, persist_root=download_root
                )

            total_files = len(manifest["expected_files"])
            self._write_download_state(
                download_root,
                self._download_result(
                    job_id,
                    manifest,
                    {
                        "model_status": "missing",
                        "install_status": "downloading",
                        "download_status": "downloading",
                        "download_progress": f"0/{total_files}",
                    },
                ),
            )

            downloaded_bytes = 0
            for index, item in enumerate(manifest["expected_files"], start=1):
                downloaded_bytes += self._download_expected_file(
                    manifest_url, download_root, item, cancel_requested
                )
                self._write_download_state(
                    download_root,
                    self._download_result(
                        job_id,
                        manifest,
                        {
                            "model_status": "missing",
                            "install_status": "downloading",
                            "download_status": "downloading",
                            "download_progress": f"{index}/{total_files}",
                            "downloaded_bytes": str(downloaded_bytes),
                        },
                    ),
                )

            self._validate_fixture_constraints(download_root, manifest)
            _validate_files(download_root, manifest)
            installed = self.install_offline(download_root)
            if installed.get("install_status") != "ready":
                fields = {
                    **installed,
                    "download_status": "retryable_failure",
                    "download_progress": f"{total_files}/{total_files}",
                    "downloaded_bytes": str(downloaded_bytes),
                }
                return self._download_result(
                    job_id, manifest, fields, persist_root=download_root
                )
            installed["job_id"] = job_id
            installed["download_status"] = "ready"
            installed["download_progress"] = "complete"
            installed["downloaded_bytes"] = str(downloaded_bytes)
            installed["download_total_bytes"] = str(downloaded_bytes)
            shutil.rmtree(download_root, ignore_errors=True)
            return installed
        except (ModelManagerError, OSError) as exc:
            manifest = None
            try:
                manifest = _read_manifest(download_root)
            except ModelManagerError:
                pass
            partial_bytes = self._partial_download_bytes(download_root)
            code = exc.code if isinstance(exc, ModelManagerError) else "write_failed"
            message = exc.message if isinstance(exc, ModelManagerError) else "model download write failed"
            fields_from_error = exc.fields if isinstance(exc, ModelManagerError) else {}
            if code == "cancelled":
                fields = {
                    "model_status": "missing",
                    "install_status": "failed",
                    "download_status": "cancelled",
                    "download_progress": "partial" if partial_bytes else "0/0",
                    "downloaded_bytes": str(partial_bytes),
                    "last_error": message,
                }
            elif code == "checksum_mismatch":
                fields = {
                    **fields_from_error,
                    "model_status": "checksum_mismatch",
                    "install_status": "failed",
                    "download_status": "retryable_failure",
                    "download_progress": "partial" if partial_bytes else "0/0",
                    "downloaded_bytes": str(partial_bytes),
                    "last_error": message,
                }
            else:
                fields = {
                    **fields_from_error,
                    "model_status": "missing",
                    "install_status": "failed",
                    "download_status": "retryable_failure",
                    "download_progress": "partial" if partial_bytes else "0/0",
                    "downloaded_bytes": str(partial_bytes),
                    "last_error": message,
                }
            return self._download_result(job_id, manifest, fields, persist_root=download_root)

    def install_offline(self, source):
        source = Path(source)
        try:
            manifest = _read_manifest(source)
            result = {**self._base_status(), **_manifest_fields(manifest)}
            if not self.model_source_gate.allows_manifest(
                manifest, fixture_allowed=self._fixture_allowed(source)
            ):
                result.update(
                    {
                        "model_status": "model_source_blocked",
                        "install_status": "blocked",
                        "last_error": result["model_source_blocker"],
                    }
                )
                return result
            self._validate_fixture_constraints(source, manifest)
            _validate_files(source, manifest)

            target = self._safe_model_root(manifest["model_id"], manifest["version"])
            if target.exists():
                if target.is_symlink() or not target.is_dir():
                    raise ModelManagerError(
                        "install_path_unsafe", "model install path is unsafe"
                    )
                shutil.rmtree(target)
            target.mkdir(parents=True)
            shutil.copy2(source / MANIFEST_NAME, target / MANIFEST_NAME)
            if self._fixture_allowed(source):
                shutil.copy2(source / FIXTURE_MARKER_NAME, target / FIXTURE_MARKER_NAME)
            for item in manifest["expected_files"]:
                source_file = _validated_file_path(source, item["path"])
                target_file = target / item["path"]
                target_file.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source_file, target_file)
        except ModelManagerError as exc:
            result = self._base_status()
            result.update(exc.fields)
            result["model_status"] = (
                "checksum_mismatch" if exc.code == "checksum_mismatch" else "missing"
            )
            result["install_status"] = "failed"
            result["last_error"] = exc.message
            return result
        except OSError as exc:
            result = self._base_status()
            result["model_status"] = "missing"
            result["install_status"] = "failed"
            result["last_error"] = "offline model install failed"
            return result

        return self.status(manifest["model_id"], manifest["version"])
