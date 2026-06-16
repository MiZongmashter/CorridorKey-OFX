import contextlib
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

from sidecar.corridorkey_sidecar.protocol import (
    COMMAND_INFER,
    COMMAND_INSTALL_MODEL,
    COMMAND_WARMUP,
    handle_line,
    request_hash_for_payload,
    status_payload,
    _write_frame_blob,
)
from sidecar.corridorkey_sidecar.model_source_gate import ModelSourceGate
from sidecar.corridorkey_sidecar.model_manager import ModelManager


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


def write_fixture_package(root, *, fixture=True, payload=b"fixture model bytes"):
    root.mkdir(parents=True, exist_ok=True)
    (root / "weights.fixture").write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    manifest = {
        "model_id": "corridorkey-fixture-green",
        "version": "0.0.0-test",
        "screen_color": "green",
        "backend_compatibility": ["stub"],
        "expected_files": [{"path": "weights.fixture", "sha256": digest}],
        "local_path": "",
        "fixture": fixture,
    }
    (root / "model-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True), encoding="utf-8"
    )
    if fixture:
        (root / ".corridorkey-test-fixture").write_text("fixture-only\n", encoding="utf-8")
    return manifest


def write_local_model_package(root, *, payload=b"local model bytes", screen_color="green"):
    root.mkdir(parents=True, exist_ok=True)
    (root / "weights.bin").write_bytes(payload)
    manifest = {
        "model_id": f"corridorkey-local-{screen_color}",
        "version": "1.0.0-dev",
        "screen_color": screen_color,
        "backend_compatibility": ["torch_cpu"],
        "expected_files": [{"path": "weights.bin", "sha256": hashlib.sha256(payload).hexdigest()}],
        "local_path": "",
    }
    (root / "model-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True), encoding="utf-8"
    )
    return manifest


def expected_model_checksum(manifest):
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


def rewrite_manifest(root, **updates):
    path = root / "model-manifest.json"
    manifest = json.loads(path.read_text(encoding="utf-8"))
    manifest.update(updates)
    path.write_text(json.dumps(manifest, sort_keys=True), encoding="utf-8")
    return manifest


def protocol_request(command, payload):
    response, should_shutdown = handle_line(
        json.dumps(
            {
                "request_id": "req-model-manager",
                "command": command,
                "payload": payload,
            }
        )
    )
    assert not should_shutdown
    return response


def non_stub_infer_payload():
    root = Path(tempfile.gettempdir()) / "corridorkey-render-model-source-blocked"
    payload = {
        "frame_id": "frame-model-source",
        "job_id": "job-model-source-blocked",
        "render_window_x1": "0",
        "render_window_y1": "0",
        "render_window_x2": "1",
        "render_window_y2": "1",
        "source_frame_blob_path": str(root / "source.ckfb"),
        "alpha_hint_frame_blob_path": str(root / "alpha.ckfb"),
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


class ModelManagerTests(unittest.TestCase):
    def test_fixture_only_gate_status_is_blocked_and_model_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            manager = ModelManager(Path(tmp), ModelSourceGate.fixture_only())

            status = manager.status()

            self.assertEqual(status["model_source_status"], "blocked")
            self.assertEqual(status["model_source_mode"], "fixture_only")
            self.assertEqual(status["model_status"], "missing")
            self.assertEqual(status["model"], "not_loaded")
            self.assertEqual(status["install_status"], "not_installed")

    def test_offline_fixture_install_records_manifest_and_ready_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            manifest = write_fixture_package(package)
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())

            installed = manager.install_offline(package)
            status = manager.status("corridorkey-fixture-green", "0.0.0-test")

            self.assertEqual(installed["install_status"], "ready")
            self.assertEqual(installed["model_status"], "ready")
            self.assertEqual(status["model_status"], "ready")
            self.assertEqual(status["model"], "0.0.0-test")
            self.assertEqual(status["screen_color"], "green")
            self.assertEqual(status["backend_compatibility"], "stub")
            self.assertEqual(status["model_checksum"], expected_model_checksum(manifest))

    def test_default_offline_install_accepts_valid_local_non_fixture_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package, fixture=False)
            install_root = Path(tmp) / "installed"
            manager = ModelManager(install_root)

            installed = manager.install_offline(package)
            status = manager.status("corridorkey-fixture-green", "0.0.0-test")

            self.assertEqual(installed["model_source_status"], "ready")
            self.assertEqual(installed["model_source_mode"], "local_development")
            self.assertEqual(installed["install_status"], "ready")
            self.assertEqual(installed["model_status"], "ready")
            self.assertEqual(status["model_status"], "ready")
            self.assertTrue((install_root / "corridorkey-fixture-green" / "0.0.0-test").is_dir())

    def test_default_offline_install_does_not_apply_fixture_size_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            payload = b"0" * (1024 * 1024 + 1)
            write_local_model_package(package, payload=payload)
            manager = ModelManager(Path(tmp) / "installed")

            installed = manager.install_offline(package)

            self.assertEqual(installed["install_status"], "ready")
            self.assertEqual(installed["model_status"], "ready")
            self.assertEqual(installed["model_source_status"], "ready")

    def test_checksum_mismatch_is_reported_without_raw_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package)
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())
            manager.install_offline(package)
            installed_file = (
                Path(tmp)
                / "installed"
                / "corridorkey-fixture-green"
                / "0.0.0-test"
                / "weights.fixture"
            )
            installed_file.write_bytes(b"corrupt")

            status = manager.status("corridorkey-fixture-green", "0.0.0-test")

            self.assertEqual(status["model_status"], "checksum_mismatch")
            self.assertEqual(status["install_status"], "failed")
            self.assertEqual(status["last_error"], "model checksum mismatch")
            self.assertEqual(status["corrupt_file"], "weights.fixture")
            self.assertNotIn(str(installed_file.parent), status["last_error"])

    def test_real_model_package_is_blocked_before_any_file_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package, fixture=False)
            install_root = Path(tmp) / "installed"
            manager = ModelManager(install_root, ModelSourceGate.fixture_only())

            result = manager.install_offline(package)

            self.assertEqual(result["model_source_status"], "blocked")
            self.assertEqual(result["model_status"], "model_source_blocked")
            self.assertEqual(result["install_status"], "blocked")
            self.assertFalse((install_root / "corridorkey-fixture-green").exists())

    def test_fixture_manifest_without_test_marker_is_blocked(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package)
            (package / ".corridorkey-test-fixture").unlink()
            install_root = Path(tmp) / "installed"
            manager = ModelManager(install_root, ModelSourceGate.fixture_only())

            result = manager.install_offline(package)

            self.assertEqual(result["model_status"], "model_source_blocked")
            self.assertEqual(result["install_status"], "blocked")
            self.assertFalse((install_root / "corridorkey-fixture-green").exists())

    def test_fixture_bypass_requires_test_only_manifest_shape(self):
        cases = (
            {"model_id": "corridorkey-real-green"},
            {"version": "1.0.0"},
            {"backend_compatibility": ["torch_cpu"]},
            {"expected_files": [{"path": "weights.ckpt", "sha256": "0" * 64}]},
            {"local_path": "/private/checkpoint"},
        )
        for updates in cases:
            with self.subTest(updates=updates):
                with tempfile.TemporaryDirectory() as tmp:
                    package = Path(tmp) / "package"
                    write_fixture_package(package)
                    rewrite_manifest(package, **updates)
                    install_root = Path(tmp) / "installed"
                    manager = ModelManager(install_root, ModelSourceGate.fixture_only())

                    result = manager.install_offline(package)

                    self.assertEqual(result["model_status"], "model_source_blocked")
                    self.assertEqual(result["install_status"], "blocked")
                    self.assertFalse((install_root / "corridorkey-fixture-green").exists())

    def test_fixture_files_must_stay_small(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            payload = b"0" * (1024 * 1024 + 1)
            write_fixture_package(package, payload=payload)
            install_root = Path(tmp) / "installed"
            manager = ModelManager(install_root, ModelSourceGate.fixture_only())

            result = manager.install_offline(package)

            self.assertEqual(result["install_status"], "failed")
            self.assertIn("too large", result["last_error"])
            self.assertFalse((install_root / "corridorkey-fixture-green").exists())

    def test_manifest_model_id_and_version_cannot_escape_install_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            install_root = Path(tmp) / "installed"
            write_fixture_package(package)
            rewrite_manifest(package, model_id="../escaped")
            manager = ModelManager(install_root, ModelSourceGate.fixture_only())

            result = manager.install_offline(package)

            self.assertEqual(result["install_status"], "failed")
            self.assertFalse((Path(tmp) / "escaped").exists())

            rewrite_manifest(package, model_id="corridorkey-fixture-green", version="bad/version")
            result = manager.install_offline(package)

            self.assertEqual(result["install_status"], "failed")
            self.assertFalse((install_root / "corridorkey-fixture-green" / "bad").exists())

    def test_install_target_rejects_symlink_components(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            install_root = Path(tmp) / "installed"
            external = Path(tmp) / "external-target"
            external.mkdir()
            write_fixture_package(package)
            install_root.mkdir()
            try:
                (install_root / "corridorkey-fixture-green").symlink_to(
                    external, target_is_directory=True
                )
            except OSError:
                self.skipTest("filesystem does not support symlinks")
            manager = ModelManager(install_root, ModelSourceGate.fixture_only())

            result = manager.install_offline(package)

            self.assertEqual(result["install_status"], "failed")
            self.assertFalse((external / "0.0.0-test").exists())

    def test_expected_files_cannot_be_symlinks_or_escape_package_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            install_root = Path(tmp) / "installed"
            external = Path(tmp) / "external.bin"
            external.write_bytes(b"external")
            write_fixture_package(package)
            (package / "weights.fixture").unlink()
            try:
                (package / "weights.fixture").symlink_to(external)
            except OSError:
                self.skipTest("filesystem does not support symlinks")
            rewrite_manifest(
                package,
                expected_files=[
                    {
                        "path": "weights.fixture",
                        "sha256": hashlib.sha256(b"external").hexdigest(),
                    }
                ],
            )
            manager = ModelManager(install_root, ModelSourceGate.fixture_only())

            result = manager.install_offline(package)

            self.assertEqual(result["install_status"], "failed")
            self.assertFalse(
                (install_root / "corridorkey-fixture-green" / "0.0.0-test").exists()
            )

    def test_offline_fixture_install_copies_only_manifest_expected_files_and_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package)
            (package / "unlisted-checkpoint.bin").write_bytes(b"do not copy")
            install_root = Path(tmp) / "installed"
            manager = ModelManager(install_root, ModelSourceGate.fixture_only())

            result = manager.install_offline(package)

            target = install_root / "corridorkey-fixture-green" / "0.0.0-test"
            self.assertEqual(result["model_status"], "ready")
            self.assertTrue((target / "model-manifest.json").is_file())
            self.assertTrue((target / "weights.fixture").is_file())
            self.assertTrue((target / ".corridorkey-test-fixture").is_file())
            self.assertFalse((target / "unlisted-checkpoint.bin").exists())

    def test_protocol_status_includes_model_source_and_model_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            payload = status_payload(ModelManager(Path(tmp) / "models"))

            self.assertEqual(payload["model_source_status"], "ready")
            self.assertEqual(payload["model_source_mode"], "local_development")
            self.assertEqual(payload["model_status"], "missing")
            self.assertEqual(payload["install_status"], "not_installed")
            self.assertEqual(payload["model"], "not_loaded")
            self.assertEqual(payload["model_checksum"], "")

    def test_default_status_uses_corridorkey_model_dir_env(self):
        with tempfile.TemporaryDirectory() as tmp:
            model_root = Path(tmp) / "models"
            package = model_root / "corridorkey-local-green" / "1.0.0-dev"
            manifest = write_local_model_package(package)

            with patched_env(
                CORRIDORKEY_MODEL_DIR=str(model_root),
                CORRIDORKEY_MODEL_ROOT=None,
            ):
                payload = status_payload()

            self.assertEqual(payload["model_status"], "ready")
            self.assertEqual(payload["install_status"], "ready")
            self.assertEqual(payload["model_id"], "corridorkey-local-green")
            self.assertEqual(payload["model_checksum"], expected_model_checksum(manifest))

    def test_auto_backend_model_selection_prefers_green_over_blue(self):
        with tempfile.TemporaryDirectory() as tmp:
            model_root = Path(tmp) / "models"
            write_local_model_package(
                model_root / "corridorkey-local-blue" / "1.0.0-dev",
                screen_color="blue",
            )
            write_local_model_package(
                model_root / "corridorkey-local-green" / "1.0.0-dev",
                screen_color="green",
            )
            manager = ModelManager(model_root)

            status = manager.select_backend_model("auto", "torch_cpu")

            self.assertEqual(status["model_status"], "ready")
            self.assertEqual(status["screen_color"], "green")
            self.assertEqual(status["model_id"], "corridorkey-local-green")

    def test_protocol_offline_install_accepts_fixture_package(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package)
            manager = ModelManager(Path(tmp) / "installed", ModelSourceGate.fixture_only())
            request = {
                "request_id": "req-model-manager",
                "command": COMMAND_INSTALL_MODEL,
                "payload": {"source_path": str(package)},
            }

            response, should_shutdown = handle_line(json.dumps(request), model_manager=manager)

            self.assertFalse(should_shutdown)
            self.assertIs(response["ok"], True)
            self.assertIsNone(response["error"])
            self.assertEqual(response["payload"]["model_status"], "ready")
            self.assertEqual(response["payload"]["install_status"], "ready")

    def test_non_stub_infer_and_warmup_report_backend_unconfigured_without_source_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            manager = ModelManager(Path(tmp) / "models", ModelSourceGate.fixture_only())
            write_fixture_package(Path(tmp) / "package")
            manager.install_offline(Path(tmp) / "package")
            request = {
                "request_id": "req-model-manager",
                "command": COMMAND_INFER,
                "payload": non_stub_infer_payload(),
            }
            infer_response, should_shutdown = handle_line(
                json.dumps(request), model_manager=manager
            )

        self.assertFalse(should_shutdown)
        self.assertIs(infer_response["ok"], False)
        self.assertEqual(infer_response["error"]["code"], "blocked_backend")
        self.assertEqual(infer_response["payload"]["backend"], "torch_cpu")
        self.assertEqual(infer_response["payload"]["model_status"], "missing")
        self.assertEqual(infer_response["payload"]["model_source_status"], "ready")

        warmup_response = protocol_request(
            COMMAND_WARMUP,
            {"job_id": "job-warmup-model-source", "backend": "torch_cpu", "quality": "high_1024"},
        )

        self.assertIs(warmup_response["ok"], False)
        self.assertEqual(warmup_response["error"]["code"], "blocked_backend")
        self.assertEqual(warmup_response["payload"]["job_id"], "job-warmup-model-source")
        self.assertEqual(warmup_response["payload"]["model_status"], "missing")
        self.assertEqual(warmup_response["payload"]["model_source_status"], "ready")

    def test_backend_unconfigured_response_preserves_ready_model_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            manifest = write_local_model_package(package)
            manager = ModelManager(Path(tmp) / "models")
            manager.install_offline(package)
            request = {
                "request_id": "req-model-manager",
                "command": COMMAND_INFER,
                "payload": non_stub_infer_payload(),
            }

            response, should_shutdown = handle_line(json.dumps(request), model_manager=manager)

            self.assertFalse(should_shutdown)
            self.assertIs(response["ok"], False)
            self.assertEqual(response["error"]["code"], "blocked_backend")
            self.assertEqual(response["payload"]["backend_status"], "blocked")
            self.assertEqual(response["payload"]["model_status"], "ready")
            self.assertEqual(response["payload"]["install_status"], "ready")
            self.assertEqual(response["payload"]["model_id"], "corridorkey-local-green")
            self.assertEqual(response["payload"]["model_source_status"], "ready")
            self.assertEqual(response["payload"]["model_checksum"], expected_model_checksum(manifest))

    def test_stub_warmup_surfaces_validated_model_identity_and_checksum(self):
        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            manifest = write_local_model_package(package)
            manager = ModelManager(Path(tmp) / "models")
            manager.install_offline(package)

            response, should_shutdown = handle_line(
                json.dumps(
                    {
                        "request_id": "req-model-warmup",
                        "command": COMMAND_WARMUP,
                        "payload": {
                            "job_id": "job-model-warmup",
                            "backend": "stub",
                            "quality": "high_1024",
                        },
                    }
                ),
                model_manager=manager,
            )

        self.assertFalse(should_shutdown)
        self.assertIs(response["ok"], True)
        self.assertEqual(response["payload"]["model"], "stub")
        self.assertEqual(response["payload"]["model_id"], "corridorkey-local-green")
        self.assertEqual(response["payload"]["model_version"], "1.0.0-dev")
        self.assertEqual(response["payload"]["backend_compatibility"], "torch_cpu")
        self.assertEqual(response["payload"]["model_checksum"], expected_model_checksum(manifest))

    def test_stub_infer_surfaces_validated_model_identity_and_checksum(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-model-status-") as tmp:
            root = Path(tmp)
            package = root / "package"
            manifest = write_local_model_package(package)
            manager = ModelManager(root / "models")
            manager.install_offline(package)
            payload = {
                "frame_id": "frame-model-status",
                "job_id": "job-model-status",
                "render_window_x1": "0",
                "render_window_y1": "0",
                "render_window_x2": "1",
                "render_window_y2": "1",
                "source_frame_blob_path": str(root / "source.ckfb"),
                "alpha_hint_frame_blob_path": str(root / "alpha.ckfb"),
                "alpha_hint_source": "external",
                "screen_color": "green",
                "quality": "high_1024",
                "input_color_space": "host_managed",
                "despill_strength": "5",
                "backend": "stub",
                "output_mode": "processed_rgba",
            }
            payload["request_hash"] = request_hash_for_payload(payload)
            _write_frame_blob(root / "source.ckfb", (0, 0, 1, 1), 4, (0.2, 0.7, 0.1, 1.0))
            _write_frame_blob(root / "alpha.ckfb", (0, 0, 1, 1), 1, (0.6,))

            response, should_shutdown = handle_line(
                json.dumps(
                    {
                        "request_id": "req-model-infer",
                        "command": COMMAND_INFER,
                        "payload": payload,
                    }
                ),
                model_manager=manager,
            )

        self.assertFalse(should_shutdown)
        self.assertIs(response["ok"], True)
        self.assertEqual(response["payload"]["model"], "stub")
        self.assertEqual(response["payload"]["model_id"], "corridorkey-local-green")
        self.assertEqual(response["payload"]["model_version"], "1.0.0-dev")
        self.assertEqual(response["payload"]["backend_compatibility"], "torch_cpu")
        self.assertEqual(response["payload"]["model_checksum"], expected_model_checksum(manifest))


if __name__ == "__main__":
    unittest.main()
