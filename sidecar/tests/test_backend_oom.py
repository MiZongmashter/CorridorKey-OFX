import contextlib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest

from sidecar.corridorkey_sidecar.model_manager import ModelManager
from sidecar.corridorkey_sidecar.protocol import COMMAND_INFER, request_hash_for_payload
from sidecar.corridorkey_sidecar.server import handle_line_with_model_manager
from scripts.run_gpu_backend_fixture import _redact_artifact_paths


HEADER = struct.Struct("<4sIiiiiI")
REPO_ROOT = Path(__file__).resolve().parents[2]


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


def write_blob(path, bounds, channels, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as handle:
        handle.write(HEADER.pack(b"CKFB", 1, *bounds, channels))
        handle.write(struct.pack(f"<{len(values)}f", *values))


def write_fixture_repo(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / "corridorkey_ofx_adapter.py").write_text(
        "\n".join(
            (
                "def infer_cpu(request):",
                "    straight = []",
                "    alpha = []",
                "    source = request['source_rgba']",
                "    hint = request['alpha_hint']",
                "    for index in range(request['pixel_count']):",
                "        r, g, b = source[index * 4:index * 4 + 3]",
                "        matte = max(0.0, min(1.0, hint[index] * 0.8 + 0.1))",
                "        straight.extend((r, g, b))",
                "        alpha.append(matte)",
                "    return {'straight_fg': straight, 'alpha': alpha, 'warnings': []}",
            )
        ),
        encoding="utf-8",
    )


def write_backend_fixture_model(root, screen_color, *, compat=("torch_cpu",)):
    model_root = root / f"corridorkey-backend-{screen_color}" / "0.0.0-backend-test"
    model_root.mkdir(parents=True, exist_ok=True)
    payload = f"{screen_color} backend fixture".encode("utf-8")
    (model_root / "weights.fixture").write_bytes(payload)
    manifest = {
        "model_id": f"corridorkey-backend-{screen_color}",
        "version": "0.0.0-backend-test",
        "screen_color": screen_color,
        "backend_compatibility": list(compat),
        "expected_files": [
            {
                "path": "weights.fixture",
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
        ],
        "fixture": True,
        "local_path": "",
    }
    (model_root / "model-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True), encoding="utf-8"
    )


def write_runtime_fixture(root, *, colors=("green", "blue")):
    repo = root / "external" / "CorridorKey"
    model_dir = root / "models"
    fixture_dir = root / "fixtures"
    write_fixture_repo(repo)
    fixture_dir.mkdir(parents=True)
    (fixture_dir / ".corridorkey-backend-fixture").write_text(
        "fixture-only\n", encoding="utf-8"
    )
    write_blob(
        fixture_dir / "source.ckfb",
        (0, 0, 2, 1),
        4,
        (0.20, 0.70, 0.10, 1.00, 0.80, 0.30, 0.20, 0.50),
    )
    write_blob(fixture_dir / "alpha.ckfb", (0, 0, 2, 1), 1, (0.25, 0.75))
    for color in colors:
        write_backend_fixture_model(model_dir, color)
    return repo, model_dir, fixture_dir


def infer_payload(root, *, backend="torch_mps", screen_color="green", quality="high_1024"):
    source_path = root / "source.ckfb"
    alpha_path = root / "alpha.ckfb"
    write_blob(
        source_path,
        (0, 0, 2, 1),
        4,
        (0.20, 0.70, 0.10, 1.00, 0.80, 0.30, 0.20, 0.50),
    )
    write_blob(alpha_path, (0, 0, 2, 1), 1, (0.25, 0.75))
    payload = {
        "frame_id": "frame-backend-mps",
        "job_id": "job-backend-mps",
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
        "despill_strength": "5",
        "backend": backend,
        "output_mode": "processed_rgba",
    }
    payload["request_hash"] = request_hash_for_payload(payload)
    return payload


def handle_infer(payload, model_dir):
    response, should_shutdown = handle_line_with_model_manager(
        json.dumps(
            {
                "request_id": "req-backend-mps",
                "command": COMMAND_INFER,
                "payload": payload,
            }
        ),
        model_manager=ModelManager(model_dir),
    )
    assert not should_shutdown
    return response


def run_gpu_fixture(output, env):
    return subprocess.run(
        [
            sys.executable,
            str(REPO_ROOT / "scripts" / "run_gpu_backend_fixture.py"),
            "--backend",
            "torch_mps",
            "--output",
            str(output),
        ],
        cwd=REPO_ROOT,
        env=env,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


class BackendOomAndMpsGateTests(unittest.TestCase):
    def test_torch_mps_blocked_reports_model_runtime_gate_without_enabling_gpu(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-mps-gate-") as tmp:
            root = Path(tmp)
            repo, model_dir, fixture_dir = write_runtime_fixture(root)
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                response = handle_infer(infer_payload(root), model_dir)

        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], "blocked_backend")
        self.assertEqual(response["payload"]["backend"], "torch_mps")
        self.assertEqual(response["payload"]["backend_status"], "unsupported")
        self.assertEqual(response["payload"]["gpu_backends_enabled"], "false")
        self.assertEqual(response["payload"]["cpu_fallback_backend"], "torch_cpu")
        self.assertEqual(response["payload"]["torch_mps_model_status"], "missing")
        self.assertIn(
            response["payload"]["torch_mps_available"], ("true", "false", "unknown")
        )
        self.assertIn("HG-03/HG-04a", response["payload"]["last_error"])

    def test_gpu_fixture_runner_records_blocked_torch_mps_row_and_cpu_fallback(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-mps-fixture-") as tmp:
            root = Path(tmp)
            repo, model_dir, fixture_dir = write_runtime_fixture(root)
            output = root / "validation" / "torch-mps-fixture.json"
            env = {
                **os.environ,
                "CORRIDORKEY_REPO": str(repo),
                "CORRIDORKEY_MODEL_DIR": str(model_dir),
                "CORRIDORKEY_BACKEND_FIXTURE_DIR": str(fixture_dir),
            }

            completed = run_gpu_fixture(output, env)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            output_text = output.read_text(encoding="utf-8")
            diagnostic = json.loads(output_text)
            self.assertIs(diagnostic["ok"], False)
            self.assertEqual(diagnostic["status"], "blocked_backend")
            self.assertEqual(diagnostic["target_row"], "ck_resolve20_torch_mps")
            self.assertEqual(diagnostic["backend"], "torch_mps")
            self.assertEqual(diagnostic["torch_mps_enabled"], "false")
            self.assertEqual(diagnostic["cpu_fallback"]["status"], "passed")
            self.assertGreaterEqual(len(diagnostic["cases"]), 7)
            for case in diagnostic["cases"]:
                self.assertEqual(case["status"], "blocked_backend")
                self.assertEqual(case["backend_requested"], "torch_mps")
                self.assertEqual(case["requested_backend"], "torch_mps")
                self.assertEqual(case["backend_status"], "unsupported")
                self.assertEqual(case["gpu_backends_enabled"], "false")
                self.assertEqual(case["error_code"], "blocked_backend")
            for path in (repo, model_dir, fixture_dir, REPO_ROOT):
                self.assertNotIn(str(path), output_text)

    def test_gpu_fixture_redacts_error_text_without_hiding_quality_enums(self):
        redacted = _redact_artifact_paths(
            {
                "last_error": "failed reading /Users/alice/SecretShow/shot010/plate.exr",
                "quality": "high_1024",
            },
            (),
        )

        self.assertNotIn("/Users/alice", redacted["last_error"])
        self.assertIn("<redacted-path>", redacted["last_error"])
        self.assertEqual(redacted["quality"], "high_1024")


if __name__ == "__main__":
    unittest.main()
