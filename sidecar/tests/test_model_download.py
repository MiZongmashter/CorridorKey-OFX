import contextlib
import hashlib
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import unittest

from sidecar.corridorkey_sidecar.model_source_gate import ModelSourceGate
import sidecar.corridorkey_sidecar.model_manager as model_manager_module
from sidecar.corridorkey_sidecar.model_manager import ModelManager
from sidecar.corridorkey_sidecar.protocol import (
    COMMAND_CANCEL_DOWNLOAD,
    COMMAND_DOWNLOAD_MODEL,
    COMMAND_MODEL_STATUS,
    ERROR_MODEL_SOURCE_BLOCKED,
    handle_line,
)


def write_fixture_package(root, *, fixture=True, payload=b"fixture download bytes"):
    root.mkdir(parents=True, exist_ok=True)
    (root / "weights.fixture").write_bytes(payload)
    manifest = {
        "model_id": "corridorkey-fixture-green",
        "version": "0.0.0-test",
        "screen_color": "green",
        "backend_compatibility": ["stub"],
        "expected_files": [
            {
                "path": "weights.fixture",
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
        ],
        "local_path": "",
        "fixture": fixture,
    }
    (root / "model-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True), encoding="utf-8"
    )
    if fixture:
        (root / ".corridorkey-test-fixture").write_text("fixture-only\n", encoding="utf-8")
    return manifest


def write_local_model_package(root, *, payload=b"local model bytes"):
    root.mkdir(parents=True, exist_ok=True)
    (root / "weights.bin").write_bytes(payload)
    manifest = {
        "model_id": "corridorkey-local-green",
        "version": "1.0.0-dev",
        "screen_color": "green",
        "backend_compatibility": ["torch_cpu"],
        "expected_files": [
            {
                "path": "weights.bin",
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
        ],
        "local_path": "",
    }
    (root / "model-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True), encoding="utf-8"
    )
    return manifest


def file_url(path):
    return Path(path).resolve().as_uri()


@contextlib.contextmanager
def patched_env(**values):
    previous = {key: os.environ.get(key) for key in values}
    try:
        for key, value in values.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        yield
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


@contextlib.contextmanager
def patched_build_opener(opener):
    previous = model_manager_module.urllib.request.build_opener
    try:
        model_manager_module.urllib.request.build_opener = lambda *args, **kwargs: opener
        yield
    finally:
        model_manager_module.urllib.request.build_opener = previous


def protocol_request(command, payload, manager):
    return handle_line(
        json.dumps(
            {
                "request_id": "req-download-test",
                "command": command,
                "payload": payload,
            }
        ),
        model_manager=manager,
    )


class ModelDownloadTests(unittest.TestCase):
    def test_download_model_installs_fixture_from_local_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package)
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())

            result = manager.download_model(
                file_url(package / "model-manifest.json"),
                job_id="job-download-success",
            )

            target = Path(tmp) / "installed" / "corridorkey-fixture-green" / "0.0.0-test"
            self.assertEqual(result["model_status"], "ready")
            self.assertEqual(result["install_status"], "ready")
            self.assertEqual(result["download_status"], "ready")
            self.assertEqual(result["download_progress"], "complete")
            self.assertTrue((target / "weights.fixture").is_file())

    def test_download_model_installs_local_loopback_source(self):
        class LocalResponse:
            status = 200

            def __init__(self, url, data):
                self._url = url
                self._data = data

            def read(self, size=-1):
                if size < 0:
                    result = self._data
                    self._data = b""
                    return result
                result = self._data[:size]
                self._data = self._data[size:]
                return result

            def geturl(self):
                return self._url

            def close(self):
                return None

            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc, tb):
                self.close()
                return False

        class LocalOpener:
            def __init__(self, root):
                self.root = Path(root)

            def open(self, request, timeout=0):
                url = request.full_url if hasattr(request, "full_url") else request
                rel_path = model_manager_module.urllib.parse.urlparse(url).path.lstrip("/")
                return LocalResponse(url, (self.root / rel_path).read_bytes())

        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_local_model_package(package, payload=b"loopback model bytes")
            installed = Path(tmp) / "installed"
            manager = ModelManager(installed)

            with patched_build_opener(LocalOpener(package)):
                result = manager.download_model(
                    "http://127.0.0.1/model-manifest.json",
                    job_id="job-download-loopback",
                )

            target = installed / "corridorkey-local-green" / "1.0.0-dev"
            self.assertEqual(result["model_status"], "ready")
            self.assertEqual(result["install_status"], "ready")
            self.assertEqual(result["download_status"], "ready")
            self.assertEqual((target / "weights.bin").read_bytes(), b"loopback model bytes")

    def test_interrupted_download_leaves_temp_file_and_successfully_resumes(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package, payload=b"0123456789abcdef")
            manager = ModelManager(
                Path(tmp) / "installed",
                ModelSourceGate.fixture_only(),
                download_chunk_bytes=4,
            )
            manifest_url = file_url(package / "model-manifest.json")

            with patched_env(
                CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                CORRIDORKEY_TEST_DOWNLOAD_FAIL_AFTER_BYTES="6",
            ):
                failed = manager.download_model(
                    manifest_url,
                    job_id="job-download-resume",
                )

            self.assertEqual(failed["download_status"], "retryable_failure")
            self.assertIn("interrupted", failed["last_error"])
            part_files = list((Path(tmp) / "installed" / ".downloads").glob("**/*.part"))
            self.assertTrue(part_files)
            self.assertLess(part_files[0].stat().st_size, 16)

            resumed = manager.download_model(
                manifest_url,
                job_id="job-download-resume",
            )

            self.assertEqual(resumed["model_status"], "ready")
            self.assertEqual(resumed["download_status"], "ready")
            self.assertEqual(
                (Path(tmp) / "installed" / "corridorkey-fixture-green" / "0.0.0-test" / "weights.fixture").read_bytes(),
                b"0123456789abcdef",
            )

    def test_corrupt_partial_file_is_discarded_and_redownloaded(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            payload = b"0123456789abcdef"
            write_fixture_package(package, payload=payload)
            installed = Path(tmp) / "installed"
            manager = ModelManager(installed, ModelSourceGate.fixture_only(), download_chunk_bytes=4)
            manifest_url = file_url(package / "model-manifest.json")
            download_root = manager._download_root(manifest_url)
            download_root.mkdir(parents=True)
            (download_root / "weights.fixture.part").write_bytes(b"corrupt-corrupt!!")

            result = manager.download_model(
                manifest_url,
                job_id="job-download-corrupt-part",
            )

            self.assertEqual(result["model_status"], "ready")
            self.assertEqual(result["download_status"], "ready")
            self.assertEqual(
                (installed / "corridorkey-fixture-green" / "0.0.0-test" / "weights.fixture").read_bytes(),
                payload,
            )

    def test_checksum_mismatch_is_retryable_and_does_not_install(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package)
            manifest_path = package / "model-manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["expected_files"][0]["sha256"] = "0" * 64
            manifest_path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())

            result = manager.download_model(
                file_url(manifest_path),
                job_id="job-download-checksum",
            )

            self.assertEqual(result["model_status"], "checksum_mismatch")
            self.assertEqual(result["download_status"], "retryable_failure")
            self.assertEqual(result["last_error"], "model checksum mismatch")
            self.assertFalse(
                (Path(tmp) / "installed" / "corridorkey-fixture-green" / "0.0.0-test").exists()
            )

    def test_expected_file_url_scheme_is_blocked_before_fetch(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            manifest = write_fixture_package(package)
            secret = Path(tmp) / "outside.fixture"
            secret.write_bytes(b"outside bytes")
            manifest["expected_files"] = [
                {
                    "path": file_url(secret),
                    "sha256": hashlib.sha256(b"outside bytes").hexdigest(),
                }
            ]
            (package / "model-manifest.json").write_text(
                json.dumps(manifest, sort_keys=True), encoding="utf-8"
            )
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())

            result = manager.download_model(
                file_url(package / "model-manifest.json"),
                job_id="job-download-url-path",
            )

            self.assertEqual(result["download_status"], "retryable_failure")
            self.assertEqual(result["install_status"], "failed")
            self.assertIn("invalid", result["last_error"])
            self.assertFalse((Path(tmp) / "installed" / "corridorkey-fixture-green").exists())

    def test_expected_file_encoded_path_traversal_is_blocked_before_fetch(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            manifest = write_fixture_package(package)
            manifest["expected_files"] = [
                {
                    "path": "nested%2F..%2F..%2Fsecret.fixture",
                    "sha256": hashlib.sha256(b"secret").hexdigest(),
                }
            ]
            (package / "model-manifest.json").write_text(
                json.dumps(manifest, sort_keys=True), encoding="utf-8"
            )
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())

            result = manager.download_model(
                file_url(package / "model-manifest.json"),
                job_id="job-download-encoded-path",
            )

            self.assertEqual(result["download_status"], "retryable_failure")
            self.assertEqual(result["install_status"], "failed")
            self.assertIn("invalid", result["last_error"])
            self.assertFalse((Path(tmp) / "installed" / "corridorkey-fixture-green").exists())

    def test_http_redirect_is_blocked_before_fetch(self):
        class RedirectResponse:
            def read(self, size=-1):
                return b""

            def geturl(self):
                return "https://example.invalid/model-manifest.json"

            def close(self):
                return None

            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc, tb):
                self.close()
                return False

        class RedirectOpener:
            def open(self, request, timeout=0):
                return RedirectResponse()

        with tempfile.TemporaryDirectory() as tmp:
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())

            with patched_build_opener(RedirectOpener()):
                result = manager.download_model(
                    "http://127.0.0.1/model-manifest.json",
                    job_id="job-download-redirect",
                )

            self.assertEqual(result["download_status"], "retryable_failure")
            self.assertIn("redirected", result["last_error"])
            self.assertFalse((Path(tmp) / "installed" / "corridorkey-fixture-green").exists())

    def test_non_explicit_local_manifest_urls_are_blocked(self):
        with tempfile.TemporaryDirectory() as tmp:
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())

            for manifest_url in (
                "model-manifest.json",
                "file://example.invalid/tmp/model-manifest.json",
                "http://[::1/model-manifest.json",
                None,
            ):
                with self.subTest(manifest_url=manifest_url):
                    result = manager.download_model(
                        manifest_url,
                        job_id="job-download-nonlocal-url",
                    )

                    self.assertEqual(result["model_status"], "missing")
                    self.assertEqual(result["download_status"], "retryable_failure")
                    self.assertIn("local file or loopback", result["last_error"])

    def test_non_hex_checksum_is_rejected_before_download(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            manifest = write_fixture_package(package)
            manifest["expected_files"][0]["sha256"] = "z" * 64
            (package / "model-manifest.json").write_text(
                json.dumps(manifest, sort_keys=True), encoding="utf-8"
            )
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())

            result = manager.download_model(
                file_url(package / "model-manifest.json"),
                job_id="job-download-bad-checksum",
            )

            self.assertEqual(result["download_status"], "retryable_failure")
            self.assertEqual(result["install_status"], "failed")
            self.assertIn("invalid", result["last_error"])

    def test_cancelled_download_reports_cancelled_without_installing(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package, payload=b"0123456789abcdef")
            manager = ModelManager(
                Path(tmp) / "installed",
                ModelSourceGate.fixture_only(),
                download_chunk_bytes=4,
            )

            result = manager.download_model(
                file_url(package / "model-manifest.json"),
                job_id="job-download-cancel",
                cancel_requested=lambda: True,
            )

            self.assertEqual(result["download_status"], "cancelled")
            self.assertEqual(result["last_error"], "model download cancelled")
            self.assertFalse((Path(tmp) / "installed" / "corridorkey-fixture-green").exists())

    def test_disk_full_like_write_failure_is_retryable(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package, payload=b"0123456789abcdef")
            manager = ModelManager(
                Path(tmp) / "installed",
                ModelSourceGate.fixture_only(),
                download_chunk_bytes=4,
            )

            with patched_env(
                CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                CORRIDORKEY_TEST_DOWNLOAD_WRITE_FAILURE_AFTER_BYTES="6",
            ):
                result = manager.download_model(
                    file_url(package / "model-manifest.json"),
                    job_id="job-download-disk-full",
                )

            self.assertEqual(result["download_status"], "retryable_failure")
            self.assertIn("write failed", result["last_error"])
            self.assertFalse((Path(tmp) / "installed" / "corridorkey-fixture-green").exists())

    def test_filesystem_write_failure_returns_retryable_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package)
            model_root_file = Path(tmp) / "models-as-file"
            model_root_file.write_text("not a directory", encoding="utf-8")
            manager = ModelManager(model_root_file, ModelSourceGate.fixture_only())

            result = manager.download_model(
                file_url(package / "model-manifest.json"),
                job_id="job-download-fs-failure",
            )

            self.assertEqual(result["download_status"], "retryable_failure")
            self.assertEqual(result["install_status"], "failed")
            self.assertIn("write failed", result["last_error"])

    def test_download_model_command_is_model_source_blocked_for_non_fixture_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package, fixture=False)
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())

            response, should_shutdown = protocol_request(
                COMMAND_DOWNLOAD_MODEL,
                {
                    "job_id": "job-download-model-source",
                    "manifest_url": file_url(package / "model-manifest.json"),
                },
                manager,
            )

            self.assertFalse(should_shutdown)
            self.assertIs(response["ok"], False)
            self.assertEqual(response["error"]["code"], ERROR_MODEL_SOURCE_BLOCKED)
            self.assertEqual(response["payload"]["model_status"], "model_source_blocked")
            self.assertEqual(response["payload"]["download_status"], "blocked")
            self.assertFalse((Path(tmp) / "installed" / "corridorkey-fixture-green").exists())

    def test_default_download_accepts_valid_local_non_fixture_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_local_model_package(package)
            installed = Path(tmp) / "installed"
            manager = ModelManager(installed)

            result = manager.download_model(
                file_url(package / "model-manifest.json"),
                job_id="job-download-local-model",
            )

            self.assertEqual(result["download_status"], "ready")
            self.assertEqual(result["install_status"], "ready")
            self.assertEqual(result["model_status"], "ready")
            self.assertTrue((installed / "corridorkey-local-green" / "1.0.0-dev").exists())

    def test_protocol_download_status_and_cancel_download_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package)
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())

            download_response, should_shutdown = protocol_request(
                COMMAND_DOWNLOAD_MODEL,
                {
                    "job_id": "job-download-protocol",
                    "manifest_url": file_url(package / "model-manifest.json"),
                },
                manager,
            )

            self.assertFalse(should_shutdown)
            self.assertIs(download_response["ok"], True)
            self.assertEqual(download_response["payload"]["download_status"], "ready")

            status_response, should_shutdown = protocol_request(
                COMMAND_MODEL_STATUS,
                {},
                manager,
            )

            self.assertFalse(should_shutdown)
            self.assertIs(status_response["ok"], True)
            self.assertEqual(status_response["payload"]["model_status"], "ready")
            self.assertEqual(status_response["payload"]["download_status"], "ready")

            cancel_response, should_shutdown = protocol_request(
                COMMAND_CANCEL_DOWNLOAD,
                {"job_id": "job-download-protocol"},
                manager,
            )

            self.assertFalse(should_shutdown)
            self.assertIs(cancel_response["ok"], True)
            self.assertEqual(
                cancel_response["payload"],
                {
                    "job_id": "job-download-protocol",
                    "cancel": "accepted",
                    "download_status": "cancelled",
                },
            )

    def test_protocol_cancel_download_stops_running_download(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package, payload=b"0123456789abcdef" * 256)
            install_root = Path(tmp) / "installed"
            manager = ModelManager(
                install_root,
                ModelSourceGate.fixture_only(),
                download_chunk_bytes=4,
            )
            holder = {}

            def run_download():
                holder["download"] = protocol_request(
                    COMMAND_DOWNLOAD_MODEL,
                    {
                        "job_id": "job-download-running",
                        "manifest_url": file_url(package / "model-manifest.json"),
                    },
                    manager,
                )

            with patched_env(
                CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                CORRIDORKEY_TEST_DOWNLOAD_CHUNK_DELAY_MS="5",
            ):
                thread = threading.Thread(target=run_download)
                thread.start()
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    part_files = list((install_root / ".downloads").glob("**/*.part"))
                    if part_files and part_files[0].stat().st_size > 0:
                        break
                    time.sleep(0.01)
                cancel_response, should_shutdown = protocol_request(
                    COMMAND_CANCEL_DOWNLOAD,
                    {"job_id": "job-download-running"},
                    manager,
                )
                thread.join(timeout=2)

            self.assertFalse(should_shutdown)
            self.assertFalse(thread.is_alive())
            self.assertIs(cancel_response["ok"], True)
            download_response, download_shutdown = holder["download"]
            self.assertFalse(download_shutdown)
            self.assertIs(download_response["ok"], False)
            self.assertEqual(download_response["error"]["code"], "cancelled")
            self.assertEqual(download_response["payload"]["download_status"], "cancelled")
            self.assertFalse((install_root / "corridorkey-fixture-green").exists())

    def test_model_status_preserves_active_downloading_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package, payload=b"0123456789abcdef" * 256)
            install_root = Path(tmp) / "installed"
            manager = ModelManager(
                install_root,
                ModelSourceGate.fixture_only(),
                download_chunk_bytes=4,
            )
            holder = {}

            def run_download():
                holder["download"] = manager.download_model(
                    file_url(package / "model-manifest.json"),
                    job_id="job-download-active",
                )

            with patched_env(
                CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                CORRIDORKEY_TEST_DOWNLOAD_CHUNK_DELAY_MS="5",
            ):
                thread = threading.Thread(target=run_download)
                thread.start()
                deadline = time.monotonic() + 2.0
                state_path = None
                while time.monotonic() < deadline:
                    states = list((install_root / ".downloads").glob("*/download-state.json"))
                    if states:
                        state_path = states[0]
                        break
                    time.sleep(0.01)
                self.assertIsNotNone(state_path)
                deadline = time.monotonic() + 2.0
                status = manager.status()
                while status["download_status"] == "not_started" and time.monotonic() < deadline:
                    time.sleep(0.01)
                    status = manager.status()
                thread.join(timeout=8)

            self.assertFalse(thread.is_alive())
            self.assertEqual(status["download_status"], "downloading")
            self.assertEqual(status["install_status"], "downloading")
            self.assertEqual(holder["download"]["download_status"], "ready")

    def test_model_status_marks_stale_downloading_state_retryable(self):
        with tempfile.TemporaryDirectory() as tmp:
            installed = Path(tmp) / "installed"
            stale = installed / ".downloads" / "stale-download"
            stale.mkdir(parents=True)
            (stale / "weights.fixture.part").write_bytes(b"partial")
            (stale / "download-state.json").write_text(
                json.dumps(
                    {
                        "job_id": "job-download-stale",
                        "model_status": "missing",
                        "install_status": "downloading",
                        "download_status": "downloading",
                        "download_progress": "0/1",
                        "downloaded_bytes": "7",
                    },
                    sort_keys=True,
                ),
                encoding="utf-8",
            )
            manager = ModelManager(installed, ModelSourceGate.fixture_only())

            status = manager.status()

            self.assertEqual(status["download_status"], "retryable_failure")
            self.assertEqual(status["install_status"], "failed")
            self.assertEqual(status["downloaded_bytes"], "7")
            self.assertIn("interrupted", status["last_error"])

    def test_model_status_preserves_download_state_with_existing_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package)
            installed = Path(tmp) / "installed"
            manager = ModelManager(installed, ModelSourceGate.fixture_only())
            ready = manager.download_model(
                file_url(package / "model-manifest.json"),
                job_id="job-download-ready-model",
            )
            self.assertEqual(ready["model_status"], "ready")
            active = installed / ".downloads" / "active-download"
            active.mkdir(parents=True)
            (active / "download-state.json").write_text(
                json.dumps(
                    {
                        "model_status": "missing",
                        "install_status": "downloading",
                        "download_status": "downloading",
                        "download_progress": "1/2",
                        "downloaded_bytes": "12",
                        "download_updated_at": f"{time.time():.3f}",
                        "last_error": "",
                    },
                    sort_keys=True,
                ),
                encoding="utf-8",
            )

            status = manager.status()

            self.assertEqual(status["model_status"], "ready")
            self.assertEqual(status["install_status"], "ready")
            self.assertEqual(status["download_status"], "downloading")
            self.assertEqual(status["download_progress"], "1/2")
            self.assertEqual(status["downloaded_bytes"], "12")


if __name__ == "__main__":
    unittest.main()
