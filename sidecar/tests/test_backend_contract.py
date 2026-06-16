import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest

from sidecar.corridorkey_sidecar.model_manager import ModelManager
from sidecar.corridorkey_sidecar.protocol import (
    COMMAND_INFER,
    COMMAND_WARMUP,
    request_hash_for_payload,
)
from sidecar.corridorkey_sidecar.server import handle_line_with_model_manager
from sidecar.corridorkey_sidecar.server import main as server_main


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


@contextlib.contextmanager
def chdir(path):
    previous = Path.cwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(previous)


def write_blob(path, bounds, channels, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as handle:
        handle.write(HEADER.pack(b"CKFB", 1, *bounds, channels))
        handle.write(struct.pack(f"<{len(values)}f", *values))


def read_blob(path):
    with Path(path).open("rb") as handle:
        magic, version, x1, y1, x2, y2, channels = HEADER.unpack(handle.read(HEADER.size))
        assert magic == b"CKFB"
        assert version == 1
        count = (x2 - x1) * (y2 - y1) * channels
        return {
            "bounds": (x1, y1, x2, y2),
            "channels": channels,
            "values": struct.unpack(f"<{count}f", handle.read(count * 4)),
        }


def srgb_to_linear(value):
    value = max(0.0, min(1.0, float(value)))
    if value <= 0.04045:
        return value / 12.92
    return ((value + 0.055) / 1.055) ** 2.4


def despill_rgb(rgb, screen_color, despill_strength):
    strength = max(0.0, min(1.0, float(despill_strength) / 10.0))
    if strength <= 0.0 or screen_color not in ("green", "blue"):
        return tuple(rgb)
    screen = 1 if screen_color == "green" else 2
    other = [index for index in (0, 1, 2) if index != screen]
    values = list(rgb)
    limit = (values[other[0]] + values[other[1]]) / 2.0
    spill = max(values[screen] - limit, 0.0)
    despilled = list(values)
    despilled[screen] = values[screen] - spill
    despilled[other[0]] = values[other[0]] + spill * 0.5
    despilled[other[1]] = values[other[1]] + spill * 0.5
    return tuple(
        values[index] * (1.0 - strength) + despilled[index] * strength
        for index in (0, 1, 2)
    )


def expected_processed_values(straight, alpha, screen_color, despill_strength):
    values = []
    for index, matte in enumerate(alpha):
        rgb = straight[index * 3 : index * 3 + 3]
        despilled = despill_rgb(rgb, screen_color, despill_strength)
        values.extend(
            (
                srgb_to_linear(despilled[0]) * matte,
                srgb_to_linear(despilled[1]) * matte,
                srgb_to_linear(despilled[2]) * matte,
                matte,
            )
        )
    return tuple(values)


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
                "        straight.extend((max(0.0, min(1.0, r)), max(0.0, min(1.0, g)), max(0.0, min(1.0, b))))",
                "        alpha.append(matte)",
                "    return {",
                "        'straight_fg': straight,",
                "        'alpha': alpha,",
                "        'model_version': request['model']['model_version'],",
                "        'warnings': ['fixture-adapter'],",
                "    }",
            )
        ),
        encoding="utf-8",
    )


def write_mlx_fixture_repo(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / "corridorkey_ofx_adapter.py").write_text(
        "\n".join(
            (
                "def infer_mlx(request):",
                "    straight = []",
                "    alpha = []",
                "    source = request['source_rgba']",
                "    hint = request['alpha_hint']",
                "    for index in range(request['pixel_count']):",
                "        r, g, b = source[index * 4:index * 4 + 3]",
                "        matte = max(0.0, min(1.0, hint[index] * 0.6 + 0.2))",
                "        straight.extend((max(0.0, min(1.0, r * 0.5)), max(0.0, min(1.0, g * 0.5)), max(0.0, min(1.0, b * 0.5))))",
                "        alpha.append(matte)",
                "    return {",
                "        'straight_fg': straight,",
                "        'alpha': alpha,",
                "        'model_version': request['model']['model_version'],",
                "        'warnings': ['mlx-fixture-adapter'],",
                "    }",
            )
        ),
        encoding="utf-8",
    )


def write_checkpoint_required_repo(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / "corridorkey_ofx_adapter.py").write_text(
        "\n".join(
            (
                "from pathlib import Path",
                "def infer_cpu(request):",
                "    checkpoint = request['model'].get('checkpoint_path', '')",
                "    if not checkpoint or not Path(checkpoint).is_file():",
                "        raise RuntimeError('missing checkpoint_path')",
                "    return {",
                "        'straight_fg': [0.2, 0.3, 0.4] * request['pixel_count'],",
                "        'alpha': [0.5] * request['pixel_count'],",
                "    }",
            )
        ),
        encoding="utf-8",
    )


def write_cpu_input_color_probe_repo(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / "corridorkey_ofx_adapter.py").write_text(
        "\n".join(
            (
                "def infer_cpu(request):",
                "    if 'input_color_space' in request:",
                "        raise RuntimeError('unexpected input_color_space')",
                "    return {",
                "        'straight_fg': [0.2, 0.3, 0.4] * request['pixel_count'],",
                "        'alpha': [0.5] * request['pixel_count'],",
                "    }",
            )
        ),
        encoding="utf-8",
    )


def write_processed_passthrough_repo(root):
    root.mkdir(parents=True, exist_ok=True)
    (root / "corridorkey_ofx_adapter.py").write_text(
        "\n".join(
            (
                "def infer_cpu(request):",
                "    return {",
                "        'straight_fg': [0.2, 0.7, 0.1] * request['pixel_count'],",
                "        'alpha': [0.5] * request['pixel_count'],",
                "        'processed_rgba': [0.01, 0.02, 0.03, 0.5] * request['pixel_count'],",
                "    }",
            )
        ),
        encoding="utf-8",
    )


def write_backend_fixture_model(root, screen_color, *, fixture=True, compat=("torch_cpu",)):
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
        "fixture": fixture,
        "local_path": "weights.fixture",
    }
    (model_root / "model-manifest.json").write_text(
        json.dumps(manifest, sort_keys=True), encoding="utf-8"
    )
    return model_root


def write_runtime_fixture(root, *, colors=("green",)):
    repo = root / "external" / "CorridorKey"
    model_dir = root / "models"
    fixture_dir = root / "fixtures"
    write_fixture_repo(repo)
    fixture_dir.mkdir(parents=True)
    (fixture_dir / ".corridorkey-backend-fixture").write_text(
        "fixture-only\n", encoding="utf-8"
    )
    for color in colors:
        write_backend_fixture_model(model_dir, color)
    return repo, model_dir, fixture_dir


def write_fixture_blobs(root):
    write_blob(
        root / "source.ckfb",
        (0, 0, 2, 1),
        4,
        (
            0.20,
            0.70,
            0.10,
            1.00,
            0.80,
            0.30,
            0.20,
            0.50,
        ),
    )
    write_blob(root / "alpha.ckfb", (0, 0, 2, 1), 1, (0.25, 0.75))


def write_runtime_local_model(root, *, colors=("green",)):
    repo = root / "external" / "CorridorKey"
    model_dir = root / "models"
    write_fixture_repo(repo)
    for color in colors:
        write_backend_fixture_model(model_dir, color, fixture=False)
    return repo, model_dir


def infer_payload(
    root,
    *,
    backend="torch_cpu",
    screen_color="green",
    quality="high_1024",
    despill="0",
    input_color_space="host_managed",
):
    source_path = root / "source.ckfb"
    alpha_path = root / "alpha.ckfb"
    write_blob(
        source_path,
        (0, 0, 2, 1),
        4,
        (
            0.20,
            0.70,
            0.10,
            1.00,
            0.80,
            0.30,
            0.20,
            0.50,
        ),
    )
    write_blob(alpha_path, (0, 0, 2, 1), 1, (0.25, 0.75))
    payload = {
        "frame_id": "frame-backend",
        "job_id": "job-backend-contract",
        "render_window_x1": "0",
        "render_window_y1": "0",
        "render_window_x2": "2",
        "render_window_y2": "1",
        "source_frame_blob_path": str(source_path),
        "alpha_hint_frame_blob_path": str(alpha_path),
        "alpha_hint_source": "external",
        "screen_color": screen_color,
        "quality": quality,
        "input_color_space": input_color_space,
        "despill_strength": despill,
        "backend": backend,
        "output_mode": "processed_rgba",
    }
    payload["request_hash"] = request_hash_for_payload(payload)
    return payload


def handle_infer(payload, model_dir, request_id="req-backend-contract"):
    response, should_shutdown = handle_line_with_model_manager(
        json.dumps(
            {
                "request_id": request_id,
                "command": COMMAND_INFER,
                "payload": payload,
            }
        ),
        model_manager=ModelManager(model_dir),
    )
    assert not should_shutdown
    return response


def handle_warmup(model_dir, backend="torch_cpu"):
    response, should_shutdown = handle_line_with_model_manager(
        json.dumps(
            {
                "request_id": "req-backend-warmup",
                "command": COMMAND_WARMUP,
                "payload": {
                    "job_id": "job-backend-warmup",
                    "backend": backend,
                    "quality": "high_1024",
                },
            }
        ),
        model_manager=ModelManager(model_dir),
    )
    assert not should_shutdown
    return response


def run_cpu_fixture(output, env):
    return subprocess.run(
        [
            sys.executable,
            str(REPO_ROOT / "scripts" / "run_cpu_backend_fixture.py"),
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


class BackendContractTests(unittest.TestCase):
    def test_torch_cpu_fixture_backend_returns_contract_outputs_and_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-contract-") as tmp:
            root = Path(tmp)
            repo, model_dir, fixture_dir = write_runtime_fixture(root)
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                response = handle_infer(
                    infer_payload(root, quality="full_2048", despill="4"), model_dir
                )

            self.assertIs(response["ok"], True)
            self.assertIsNone(response["error"])
            self.assertEqual(response["payload"]["backend"], "torch_cpu")
            self.assertEqual(response["payload"]["model"], "0.0.0-backend-test")
            self.assertEqual(response["payload"]["requested_quality"], "full_2048")
            self.assertEqual(response["payload"]["effective_quality"], "full_2048")
            self.assertEqual(response["payload"]["gpu_backends_enabled"], "false")
            self.assertEqual(response["payload"]["backend_status"], "ready")
            self.assertEqual(response["payload"]["fixture_runtime"], "true")
            self.assertIn("fixture-adapter", response["payload"]["warnings"])

            processed = read_blob(response["payload"]["processed_rgba_frame_blob_path"])
            straight = read_blob(response["payload"]["straight_fg_frame_blob_path"])
            alpha = read_blob(response["payload"]["alpha_frame_blob_path"])

            self.assertEqual(processed["bounds"], (0, 0, 2, 1))
            self.assertEqual(processed["channels"], 4)
            self.assertEqual(straight["channels"], 3)
            self.assertEqual(alpha["channels"], 1)
            for value in processed["values"] + straight["values"] + alpha["values"]:
                self.assertGreaterEqual(value, 0.0)
                self.assertLessEqual(value, 1.0)

            self.assertAlmostEqual(straight["values"][0], 0.20, places=6)
            self.assertAlmostEqual(straight["values"][1], 0.70, places=6)
            self.assertAlmostEqual(straight["values"][2], 0.10, places=6)
            self.assertAlmostEqual(straight["values"][3], 0.80, places=6)
            self.assertAlmostEqual(straight["values"][4], 0.30, places=6)
            self.assertAlmostEqual(straight["values"][5], 0.20, places=6)

            expected_processed = expected_processed_values(
                straight["values"], alpha["values"], "green", "4"
            )
            for index, matte in enumerate(alpha["values"]):
                self.assertAlmostEqual(
                    processed["values"][index * 4 + 3], matte, places=6
                )
            for actual, expected in zip(processed["values"], expected_processed):
                self.assertAlmostEqual(actual, expected, places=6)

    def test_torch_cpu_uses_configured_local_model_without_fixture_runtime_marker(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-local-") as tmp:
            root = Path(tmp)
            repo, model_dir = write_runtime_local_model(root)
            missing_fixture_dir = root / "missing-private-fixture"
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(missing_fixture_dir),
            ):
                response = handle_infer(infer_payload(root), model_dir)

            self.assertIs(response["ok"], True)
            self.assertEqual(response["payload"]["backend_status"], "ready")
            self.assertEqual(response["payload"]["fixture_runtime"], "false")
            self.assertEqual(response["payload"]["model_source_status"], "ready")
            self.assertEqual(response["payload"]["model_source_mode"], "local_development")
            self.assertEqual(response["payload"]["model"], "0.0.0-backend-test")
            self.assertIsNone(response["error"])

    def test_torch_cpu_passes_checkpoint_path_to_real_adapter(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-checkpoint-") as tmp:
            root = Path(tmp)
            repo = root / "external" / "CorridorKey"
            model_dir = root / "models"
            write_checkpoint_required_repo(repo)
            write_backend_fixture_model(model_dir, "green", fixture=False)
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(root / "missing-private-fixture"),
            ):
                response = handle_infer(infer_payload(root), model_dir)

            self.assertIs(response["ok"], True)
            self.assertEqual(response["payload"]["backend_status"], "ready")
            self.assertIsNone(response["error"])

    def test_torch_cpu_does_not_forward_input_color_space_to_adapter(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-input-color-") as tmp:
            root = Path(tmp)
            repo = root / "external" / "CorridorKey"
            model_dir = root / "models"
            write_cpu_input_color_probe_repo(repo)
            write_backend_fixture_model(model_dir, "green", fixture=False)
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(root / "missing-private-fixture"),
            ):
                response = handle_infer(
                    infer_payload(root, input_color_space="srgb_rec709"), model_dir
                )

            self.assertIs(response["ok"], True)
            self.assertEqual(response["payload"]["backend_status"], "ready")
            self.assertIsNone(response["error"])

    def test_torch_cpu_prefers_adapter_processed_rgba_when_provided(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-processed-") as tmp:
            root = Path(tmp)
            repo = root / "external" / "CorridorKey"
            model_dir = root / "models"
            write_processed_passthrough_repo(repo)
            write_backend_fixture_model(model_dir, "green", fixture=False)
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(root / "missing-private-fixture"),
            ):
                response = handle_infer(infer_payload(root, despill="10"), model_dir)

            self.assertIs(response["ok"], True)
            processed = read_blob(response["payload"]["processed_rgba_frame_blob_path"])
            expected = (0.01, 0.02, 0.03, 0.5, 0.01, 0.02, 0.03, 0.5)
            for actual, expected_value in zip(processed["values"], expected):
                self.assertAlmostEqual(actual, expected_value, places=6)

    def test_auto_backend_routes_to_torch_cpu_fixture(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-auto-") as tmp:
            root = Path(tmp)
            repo, model_dir, fixture_dir = write_runtime_fixture(root)
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                response = handle_infer(infer_payload(root, backend="auto"), model_dir)

        self.assertIs(response["ok"], True)
        self.assertEqual(response["payload"]["backend"], "torch_cpu")
        self.assertEqual(response["payload"]["requested_backend"], "auto")
        self.assertEqual(response["payload"]["effective_backend"], "torch_cpu")
        self.assertEqual(response["payload"]["backend_status"], "ready")
        self.assertIsNone(response["error"])

    def test_torch_cpu_quality_reports_requested_and_effective_quality(self):
        for quality in ("draft_512", "high_1024", "full_2048"):
            with self.subTest(quality=quality):
                with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-quality-") as tmp:
                    root = Path(tmp)
                    repo, model_dir, fixture_dir = write_runtime_fixture(root)
                    with patched_env(
                        CORRIDORKEY_REPO=str(repo),
                        CORRIDORKEY_MODEL_DIR=str(model_dir),
                        CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
                    ):
                        response = handle_infer(
                            infer_payload(root, quality=quality), model_dir
                        )

                self.assertIs(response["ok"], True)
                self.assertEqual(response["payload"]["requested_quality"], quality)
                self.assertEqual(response["payload"]["effective_quality"], quality)

    def test_despill_changes_processed_rgb_without_changing_alpha(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-despill-") as tmp:
            root = Path(tmp)
            repo, model_dir, fixture_dir = write_runtime_fixture(root)
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                baseline = handle_infer(infer_payload(root, despill="0"), model_dir)
                changed = handle_infer(infer_payload(root, despill="10"), model_dir)

            self.assertIs(baseline["ok"], True)
            self.assertIs(changed["ok"], True)
            self.assertEqual(
                Path(baseline["payload"]["alpha_frame_blob_path"]).read_bytes(),
                Path(changed["payload"]["alpha_frame_blob_path"]).read_bytes(),
            )
            baseline_processed = read_blob(baseline["payload"]["processed_rgba_frame_blob_path"])
            changed_processed = read_blob(changed["payload"]["processed_rgba_frame_blob_path"])
            baseline_straight = read_blob(baseline["payload"]["straight_fg_frame_blob_path"])
            changed_straight = read_blob(changed["payload"]["straight_fg_frame_blob_path"])
            self.assertEqual(
                baseline_processed["values"][3::4],
                changed_processed["values"][3::4],
            )
            self.assertEqual(baseline_straight["values"], changed_straight["values"])
            self.assertNotEqual(
                Path(baseline["payload"]["processed_rgba_frame_blob_path"]).read_bytes(),
                Path(changed["payload"]["processed_rgba_frame_blob_path"]).read_bytes(),
            )

    def test_screen_color_selects_green_and_blue_compatible_models(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-screen-select-") as tmp:
            root = Path(tmp)
            repo, model_dir, fixture_dir = write_runtime_fixture(root, colors=("blue", "green"))
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                green = handle_infer(infer_payload(root, screen_color="green"), model_dir)
                blue = handle_infer(infer_payload(root, screen_color="blue"), model_dir)

        self.assertIs(green["ok"], True)
        self.assertIs(blue["ok"], True)
        self.assertEqual(green["payload"]["screen_color"], "green")
        self.assertEqual(blue["payload"]["screen_color"], "blue")
        self.assertEqual(green["payload"]["model_id"], "corridorkey-backend-green")
        self.assertEqual(blue["payload"]["model_id"], "corridorkey-backend-blue")

    def test_screen_color_auto_prefers_green_fixture_model(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-screen-auto-") as tmp:
            root = Path(tmp)
            repo, model_dir, fixture_dir = write_runtime_fixture(root, colors=("blue", "green"))
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                response = handle_infer(infer_payload(root, screen_color="auto"), model_dir)

        self.assertIs(response["ok"], True)
        self.assertEqual(response["payload"]["screen_color"], "green")
        self.assertEqual(response["payload"]["model_id"], "corridorkey-backend-green")

    def test_screen_color_selects_compatible_model_or_returns_blocked_status(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-screen-") as tmp:
            root = Path(tmp)
            repo, model_dir, fixture_dir = write_runtime_fixture(root, colors=("green",))
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                green = handle_infer(infer_payload(root, screen_color="green"), model_dir)
                blue = handle_infer(infer_payload(root, screen_color="blue"), model_dir)

            self.assertIs(green["ok"], True)
            self.assertEqual(green["payload"]["screen_color"], "green")
            self.assertIs(blue["ok"], False)
            self.assertEqual(blue["error"]["code"], "blocked_backend")
            self.assertEqual(blue["payload"]["model_status"], "missing")
            self.assertEqual(blue["payload"]["backend_status"], "blocked")
            self.assertIn("screen_color blue", blue["payload"]["last_error"])

    def test_mlx_fixture_backend_returns_contract_outputs_and_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-mlx-") as tmp:
            root = Path(tmp)
            repo = root / "external" / "CorridorKey"
            model_dir = root / "models"
            fixture_dir = root / "fixtures"
            write_mlx_fixture_repo(repo)
            fixture_dir.mkdir(parents=True)
            (fixture_dir / ".corridorkey-backend-fixture").write_text(
                "fixture-only\n", encoding="utf-8"
            )
            write_backend_fixture_model(model_dir, "green", compat=("mlx",))
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                response = handle_infer(infer_payload(root, backend="mlx"), model_dir)

            self.assertIs(response["ok"], True)
            self.assertIsNone(response["error"])
            self.assertEqual(response["payload"]["backend"], "mlx")
            self.assertEqual(response["payload"]["requested_backend"], "mlx")
            self.assertEqual(response["payload"]["effective_backend"], "mlx")
            self.assertEqual(response["payload"]["backend_status"], "ready")
            self.assertEqual(response["payload"]["gpu_backends_enabled"], "true")
            self.assertEqual(response["payload"]["model_id"], "corridorkey-backend-green")
            self.assertEqual(response["payload"]["backend_compatibility"], "mlx")
            self.assertIn("mlx-fixture-adapter", response["payload"]["warnings"])
            processed = read_blob(response["payload"]["processed_rgba_frame_blob_path"])
            straight = read_blob(response["payload"]["straight_fg_frame_blob_path"])
            alpha = read_blob(response["payload"]["alpha_frame_blob_path"])
            self.assertEqual(processed["channels"], 4)
            self.assertEqual(straight["channels"], 3)
            self.assertEqual(alpha["channels"], 1)
            for value in processed["values"] + straight["values"] + alpha["values"]:
                self.assertGreaterEqual(value, 0.0)
                self.assertLessEqual(value, 1.0)

    def test_mlx_requires_adapter_contract(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-mlx-contract-") as tmp:
            root = Path(tmp)
            repo = root / "external" / "CorridorKey"
            model_dir = root / "models"
            fixture_dir = root / "fixtures"
            write_fixture_repo(repo)
            fixture_dir.mkdir(parents=True)
            (fixture_dir / ".corridorkey-backend-fixture").write_text(
                "fixture-only\n", encoding="utf-8"
            )
            write_backend_fixture_model(model_dir, "green", compat=("mlx",))
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                response = handle_infer(infer_payload(root, backend="mlx"), model_dir)

        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], "blocked_backend")
        self.assertEqual(response["payload"]["backend"], "mlx")
        self.assertEqual(response["payload"]["backend_status"], "blocked")
        self.assertIn("infer_mlx", response["payload"]["last_error"])

    def test_mlx_blue_screen_returns_contract_outputs_and_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-mlx-blue-") as tmp:
            root = Path(tmp)
            repo = root / "external" / "CorridorKey"
            model_dir = root / "models"
            fixture_dir = root / "fixtures"
            write_mlx_fixture_repo(repo)
            fixture_dir.mkdir(parents=True)
            (fixture_dir / ".corridorkey-backend-fixture").write_text(
                "fixture-only\n", encoding="utf-8"
            )
            write_backend_fixture_model(model_dir, "blue", compat=("mlx",))
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                response = handle_infer(
                    infer_payload(root, backend="mlx", screen_color="blue"), model_dir
                )

            self.assertIs(response["ok"], True)
            self.assertIsNone(response["error"])
            self.assertEqual(response["payload"]["backend"], "mlx")
            self.assertEqual(response["payload"]["requested_backend"], "mlx")
            self.assertEqual(response["payload"]["effective_backend"], "mlx")
            self.assertEqual(response["payload"]["backend_status"], "ready")
            self.assertEqual(response["payload"]["model_status"], "ready")
            self.assertEqual(response["payload"]["screen_color"], "blue")
            self.assertEqual(response["payload"]["backend_compatibility"], "mlx")
            self.assertIn("mlx-fixture-adapter", response["payload"]["warnings"])
            processed = read_blob(response["payload"]["processed_rgba_frame_blob_path"])
            straight = read_blob(response["payload"]["straight_fg_frame_blob_path"])
            alpha = read_blob(response["payload"]["alpha_frame_blob_path"])
            self.assertEqual(processed["channels"], 4)
            self.assertEqual(straight["channels"], 3)
            self.assertEqual(alpha["channels"], 1)

    def test_missing_runtime_paths_report_blocked_backend_without_import_failure(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-missing-") as tmp:
            root = Path(tmp)
            missing_repo = root / "missing-repo"
            missing_models = root / "missing-models"
            missing_fixtures = root / "missing-fixtures"
            with patched_env(
                CORRIDORKEY_REPO=str(missing_repo),
                CORRIDORKEY_MODEL_DIR=str(missing_models),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(missing_fixtures),
            ):
                response = handle_infer(infer_payload(root), missing_models)

            self.assertIs(response["ok"], False)
            self.assertEqual(response["error"]["code"], "blocked_backend")
            self.assertEqual(response["payload"]["backend"], "torch_cpu")
            self.assertEqual(response["payload"]["backend_status"], "blocked")
            self.assertIn("CORRIDORKEY_REPO", response["payload"]["missing_runtime_paths"])
            self.assertNotIn("Traceback", response["error"]["message"])

    def test_missing_runtime_paths_keep_valid_local_model_status_visible(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-model-ready-") as tmp:
            root = Path(tmp)
            missing_repo = root / "missing-repo"
            model_dir = root / "models"
            missing_fixtures = root / "missing-fixtures"
            write_backend_fixture_model(model_dir, "green", fixture=False)
            with patched_env(
                CORRIDORKEY_REPO=str(missing_repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(missing_fixtures),
            ):
                response = handle_infer(infer_payload(root), model_dir)

        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], "blocked_backend")
        self.assertEqual(response["payload"]["backend_status"], "blocked")
        self.assertEqual(response["payload"]["model_status"], "ready")
        self.assertEqual(response["payload"]["install_status"], "ready")
        self.assertEqual(response["payload"]["model_source_status"], "ready")
        self.assertIn("CORRIDORKEY_REPO", response["payload"]["missing_runtime_paths"])

    def test_gpu_backends_remain_disabled_with_clear_unsupported_status(self):
        for backend in ("torch_cuda", "torch_mps"):
            with self.subTest(backend=backend):
                with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-gpu-") as tmp:
                    root = Path(tmp)
                    repo, model_dir, fixture_dir = write_runtime_fixture(root)
                    with patched_env(
                        CORRIDORKEY_REPO=str(repo),
                        CORRIDORKEY_MODEL_DIR=str(model_dir),
                        CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
                    ):
                        response = handle_infer(
                            infer_payload(root, backend=backend), model_dir
                        )

                self.assertIs(response["ok"], False)
                self.assertEqual(response["error"]["code"], "blocked_backend")
                self.assertEqual(response["payload"]["backend"], backend)
                self.assertEqual(response["payload"]["backend_status"], "unsupported")
                self.assertEqual(response["payload"]["gpu_backends_enabled"], "false")
                self.assertIn("disabled", response["payload"]["last_error"])

    def test_warmup_missing_model_returns_blocked_backend_without_crashing(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-warmup-") as tmp:
            root = Path(tmp)
            repo = root / "external" / "CorridorKey"
            model_dir = root / "models"
            fixture_dir = root / "fixtures"
            repo.mkdir(parents=True)
            model_dir.mkdir()
            fixture_dir.mkdir()
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                response = handle_warmup(model_dir)

        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], "blocked_backend")
        self.assertEqual(response["payload"]["job_id"], "job-backend-warmup")
        self.assertEqual(response["payload"]["backend"], "torch_cpu")
        self.assertEqual(response["payload"]["backend_status"], "blocked")
        self.assertEqual(response["payload"]["gpu_backends_enabled"], "false")
        self.assertIn("no torch_cpu model", response["payload"]["last_error"])

    def test_server_uses_runtime_config_default_model_dir_without_env_override(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-defaults-") as tmp:
            root = Path(tmp)
            repo = root / "external" / "CorridorKey"
            model_dir = root / ".local" / "models" / "corridorkey"
            write_fixture_repo(repo)
            write_backend_fixture_model(model_dir, "green", fixture=False)
            with patched_env(
                CORRIDORKEY_REPO=None,
                CORRIDORKEY_MODEL_DIR=None,
                CORRIDORKEY_BACKEND_FIXTURE_DIR=None,
            ):
                with chdir(root):
                    instream = io.StringIO(
                        json.dumps(
                            {
                                "request_id": "req-default-warmup",
                                "command": COMMAND_WARMUP,
                                "payload": {
                                    "job_id": "job-default-warmup",
                                    "backend": "auto",
                                    "quality": "high_1024",
                                },
                            }
                        )
                        + "\n"
                        + json.dumps(
                            {
                                "request_id": "req-default-shutdown",
                                "command": "shutdown",
                                "payload": {},
                            }
                        )
                        + "\n"
                    )
                    outstream = io.StringIO()
                    errstream = io.StringIO()
                    self.assertEqual(server_main(instream, outstream, errstream), 0)

            warmup = json.loads(outstream.getvalue().splitlines()[0])
            self.assertIs(warmup["ok"], True)
            self.assertEqual(warmup["payload"]["backend_status"], "ready")
            self.assertEqual(warmup["payload"]["model_status"], "ready")
            self.assertEqual(warmup["payload"]["model_id"], "corridorkey-backend-green")

    def test_unsafe_fixture_manifest_path_returns_blocked_backend(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-unsafe-") as tmp:
            root = Path(tmp)
            repo, model_dir, fixture_dir = write_runtime_fixture(root, colors=())
            outside = model_dir / "corridorkey-backend-green" / "outside.fixture"
            outside.parent.mkdir(parents=True)
            outside.write_bytes(b"outside")
            model_root = outside.parent / "0.0.0-backend-test"
            model_root.mkdir()
            manifest = {
                "model_id": "corridorkey-backend-green",
                "version": "0.0.0-backend-test",
                "screen_color": "green",
                "backend_compatibility": ["torch_cpu"],
                "expected_files": [
                    {
                        "path": "../outside.fixture",
                        "sha256": hashlib.sha256(b"outside").hexdigest(),
                    }
                ],
                "fixture": True,
                "local_path": "",
            }
            (model_root / "model-manifest.json").write_text(
                json.dumps(manifest, sort_keys=True), encoding="utf-8"
            )
            with patched_env(
                CORRIDORKEY_REPO=str(repo),
                CORRIDORKEY_MODEL_DIR=str(model_dir),
                CORRIDORKEY_BACKEND_FIXTURE_DIR=str(fixture_dir),
            ):
                response = handle_infer(infer_payload(root), model_dir)

        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], "blocked_backend")
        self.assertEqual(response["payload"]["backend_status"], "blocked")
        self.assertNotEqual(response["error"]["code"], "backend_runtime_error")

    def test_cpu_fixture_runner_requires_fixture_marker_and_records_redaction(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fixture-runner-") as tmp:
            root = Path(tmp)
            repo = root / "external" / "CorridorKey"
            model_dir = root / "models"
            fixture_dir = root / "private"
            output = root / "validation" / "cpu-fixture.json"
            write_fixture_repo(repo)
            write_backend_fixture_model(model_dir, "green")
            fixture_dir.mkdir()
            write_fixture_blobs(fixture_dir)
            env = {
                **os.environ,
                "CORRIDORKEY_REPO": str(repo),
                "CORRIDORKEY_MODEL_DIR": str(model_dir),
                "CORRIDORKEY_BACKEND_FIXTURE_DIR": str(fixture_dir),
            }

            completed = run_cpu_fixture(output, env)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            diagnostic = json.loads(output.read_text(encoding="utf-8"))
            self.assertIs(diagnostic["ok"], False)
            self.assertEqual(diagnostic["status"], "blocked_backend")
            self.assertEqual(diagnostic["fixture_path_redaction_status"], "redacted")
            self.assertEqual(diagnostic["response"]["error"]["code"], "blocked_backend")
            self.assertIn("fixture marker", diagnostic["response"]["payload"]["last_error"])
            self.assertNotIn(str(fixture_dir), output.read_text(encoding="utf-8"))

    def test_cpu_fixture_runner_passes_and_redacts_private_paths(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fixture-runner-ok-") as tmp:
            root = Path(tmp)
            repo = root / "external" / "CorridorKey"
            model_dir = root / "models"
            fixture_dir = root / "private"
            output = root / "validation" / "cpu-fixture.json"
            write_fixture_repo(repo)
            write_backend_fixture_model(model_dir, "green")
            fixture_dir.mkdir()
            (fixture_dir / ".corridorkey-backend-fixture").write_text(
                "fixture-only\n", encoding="utf-8"
            )
            write_fixture_blobs(fixture_dir)
            env = {
                **os.environ,
                "CORRIDORKEY_REPO": str(repo),
                "CORRIDORKEY_MODEL_DIR": str(model_dir),
                "CORRIDORKEY_BACKEND_FIXTURE_DIR": str(fixture_dir),
            }

            completed = run_cpu_fixture(output, env)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            output_text = output.read_text(encoding="utf-8")
            diagnostic = json.loads(output_text)
            self.assertIs(diagnostic["ok"], True)
            self.assertEqual(diagnostic["status"], "passed")
            self.assertEqual(diagnostic["fixture_path_redaction_status"], "redacted")
            self.assertEqual(
                diagnostic["response"]["payload"]["processed_rgba_frame_blob_path"],
                "<redacted-path>",
            )
            for path in (repo, model_dir, fixture_dir, REPO_ROOT):
                self.assertNotIn(str(path), output_text)


if __name__ == "__main__":
    unittest.main()
